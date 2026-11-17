#!/usr/bin/env python3
"""Tests of tools/check_no_export_directives.py on objects built in memory.

Usage: python3 -I tests/check_no_export_directives_tests.py <repository root>

The objects are COFF files assembled here, so the cases run on every host
without a Windows toolchain. Properties:
  E1  an object whose .drectve holds -export: or /EXPORT: (any case,
      quoted or not) is found, in a regular and in a big-object header;
  E2  other directives (-exclude-symbols:, -defaultlib:) and objects with
      no .drectve section pass;
  E3  a file that is not a COFF object of a known machine, an import
      object, or a section past the end of the file is refused;
  E4  a whole run: clean objects with a seeing probe pass; an exporting
      object fails; a probe without a directive fails; an empty object
      directory fails.
"""

import importlib.util
import io
import pathlib
import struct
import sys
import tempfile
from contextlib import redirect_stdout

failures: list[str] = []
cases: int = 0
AMD64 = 0x8664


def check(condition: bool, label: str) -> None:
    global cases
    cases = cases + 1
    if not condition:
        failures.append(label)


def load_tool(root: pathlib.Path):
    path = root / 'tools' / 'check_no_export_directives.py'
    spec = importlib.util.spec_from_file_location('check_no_export_directives', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['check_no_export_directives'] = module
    spec.loader.exec_module(module)
    return module


def section_table(sections: list[tuple[bytes, bytes]], data_start: int) -> tuple[bytes, bytes]:
    """The section headers and the raw data that follows them."""
    headers = b''
    raw = b''
    for name, contents in sections:
        offset = data_start + len(raw)
        headers += name.ljust(8, b'\0') + struct.pack('<IIIIIIHHI', 0, 0, len(contents), offset, 0, 0, 0, 0, 0)
        raw += contents
    return headers, raw


def regular_object(sections: list[tuple[bytes, bytes]], machine: int = AMD64) -> bytes:
    start = 20 + 40 * len(sections)
    headers, raw = section_table(sections, start)
    header = struct.pack('<HHIIIHH', machine, len(sections), 0, 0, 0, 0, 0)
    return header + headers + raw


def big_object(sections: list[tuple[bytes, bytes]], machine: int = AMD64) -> bytes:
    start = 56 + 40 * len(sections)
    headers, raw = section_table(sections, start)
    header = struct.pack('<HHHHI', 0, 0xFFFF, 2, machine, 0) + bytes(16)
    header += struct.pack('<IIIIIII', 0, 0, 0, 0, len(sections), 0, 0)
    return header + headers + raw


def exports(tool, data: bytes) -> list[str]:
    found: list[str] = tool.exports_in(tool.directive_text(data))
    return found


def refused(tool, data: bytes) -> bool:
    try:
        tool.directive_text(data)
    except tool.NotCoff:
        return True
    return False


def directives(tool) -> None:
    text = b'.text'
    code = b'\xc3'
    gnu = regular_object([(text, code), (b'.drectve', b' -export:_ZN6stxlib5state9allocatorEj')])
    check(exports(tool, gnu) == ['-export:_ZN6stxlib5state9allocatorEj'], 'E1 a GNU export directive is found')
    msvc = regular_object([(b'.drectve', b'/DEFAULTLIB:libcmt /EXPORT:thread_state ')])
    check(exports(tool, msvc) == ['/EXPORT:thread_state'], 'E1 an MSVC export directive is found')
    quoted = regular_object([(b'.drectve', b' "-export:allocator",data')])
    check(exports(tool, quoted) == ['-export:allocator",data'], 'E1 a quoted export directive is found')
    big = big_object([(text, code), (b'.drectve', b' -export:process_state')])
    check(exports(tool, big) == ['-export:process_state'], 'E1 an export directive in a big object is found')
    arm = regular_object([(b'.drectve', b' -export:x')], 0xAA64)
    check(exports(tool, arm) == ['-export:x'], 'E1 an arm64 object is read')
    hidden = regular_object([(text, code), (b'.drectve', b' -exclude-symbols:_ZN6stxlib4hiddenEv -defaultlib:kernel32')])
    check(exports(tool, hidden) == [], 'E2 exclude-symbols and defaultlib are not exports')
    plain = regular_object([(text, code)])
    check(exports(tool, plain) == [], 'E2 an object without .drectve passes')
    check(refused(tool, b'\x7fELF' + bytes(60)), 'E3 an ELF object is refused')
    check(refused(tool, b'\xcf\xfa\xed\xfe' + bytes(60)), 'E3 a Mach-O object is refused')
    check(refused(tool, bytes(10)), 'E3 a short file is refused')
    import_object = struct.pack('<HHHH', 0, 0xFFFF, 0, AMD64) + bytes(60)
    check(refused(tool, import_object), 'E3 an import object is refused')
    truncated = regular_object([(b'.drectve', b' -export:x')])[:-4]
    check(refused(tool, truncated), 'E3 a section past the end is refused')


def run_main(tool, arguments: list[str]) -> int:
    with redirect_stdout(io.StringIO()):
        status: int = tool.main(arguments)
    return status


def whole_runs(tool) -> None:
    with tempfile.TemporaryDirectory() as scratch:
        root = pathlib.Path(scratch)
        probe = root / 'probe'
        seeing = root / 'seeing'
        clean = root / 'clean'
        leaking = root / 'leaking'
        empty = root / 'empty'
        for directory in (probe, seeing, clean, leaking, empty):
            directory.mkdir()
        (probe / 'export_probe.cpp.obj').write_bytes(regular_object([(b'.drectve', b' -export:stxlib_export_probe')]))
        (seeing / 'export_probe.cpp.obj').write_bytes(regular_object([(b'.text', b'\xc3')]))
        (clean / 'state.cpp.obj').write_bytes(regular_object([(b'.drectve', b' -exclude-symbols:hidden')]))
        (clean / 'heap.cpp.obj').write_bytes(big_object([(b'.text', b'\xc3')]))
        (leaking / 'state.cpp.obj').write_bytes(regular_object([(b'.drectve', b' -export:thread_state')]))
        check(run_main(tool, ['--probe', str(probe), '--objects', str(clean)]) == 0, 'E4 clean objects pass')
        check(run_main(tool, ['--probe', str(probe), '--objects', str(clean), '--objects', str(leaking)]) == 1,
              'E4 an exporting object fails')
        check(run_main(tool, ['--probe', str(seeing), '--objects', str(clean)]) == 1,
              'E4 a probe without a directive fails')
        check(run_main(tool, ['--probe', str(probe), '--objects', str(empty)]) == 1,
              'E4 an empty object directory fails')


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_no_export_directives_tests.py <repository root>')
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    tool = load_tool(root)
    directives(tool)
    whole_runs(tool)
    for label in failures:
        print('FAILED: ' + label)
    print(f'check_no_export_directives tests: {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
