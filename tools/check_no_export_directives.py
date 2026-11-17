#!/usr/bin/env python3
"""Checks that a static stxlib_state's COFF objects carry no export directive.

A static stxlib_state (STXLIB_STATE_SHARED OFF) is private to the module
that links it: the one program or DLL that holds STXlib's state. On Windows
a function declared __declspec(dllexport) leaves an export directive
(-export:name, or /EXPORT:name from MSVC) in its object's .drectve
section, and the linker exports that name from whatever DLL the object is
linked into, whatever that DLL's .def lists. So stxlib_state defines
STXLIB_STATE_BUILDING (state.hpp's dllexport) only when it is built shared,
and this tool checks the result in the objects themselves.

The tool reads the COFF objects directly (regular and big-object headers;
x86-64, i386, arm64 and armv7), so it needs no objdump. A file that is not
such an object fails, so the scan cannot pass by not reading it. Other
directives (-exclude-symbols:, -defaultlib:, ...) are not exports and pass.

The scan must be able to see directives: every object under --probe
(compiled with a dllexport on purpose) must carry one, or the tool fails.

Usage:
  python3 -I tools/check_no_export_directives.py --probe <dir>
      --objects <dir> [--objects <dir>]...

Exit status 0 when no checked object exports and the probe shows its
directive, 1 otherwise; each finding is printed as object: directive.
"""

import argparse
import pathlib
import re
import struct
import sys

OBJECT_SUFFIXES = ('.o', '.obj')
# IMAGE_FILE_MACHINE_ values of the targets STXlib builds for on Windows.
COFF_MACHINES = frozenset({0x8664, 0x014C, 0xAA64, 0x01C4})
REGULAR_HEADER_SIZE = 20
BIG_HEADER_SIZE = 56
SECTION_HEADER_SIZE = 40
DIRECTIVE_SECTION = b'.drectve'
# A directive token that exports a name: -export: (GNU and LLVM MinGW) or
# /EXPORT: (MSVC), either case, optionally opened by a quote.
EXPORT_TOKEN = re.compile(r'(?:^|\s)"?([-/]export:\S*)', re.IGNORECASE)


class NotCoff(Exception):
    """The file is not a COFF object this tool can read."""


def directive_text(data: bytes) -> str:
    """The text of the object's .drectve sections, joined by spaces ('' when none).

    Raises NotCoff when the header or a section lies outside the data."""
    if len(data) < REGULAR_HEADER_SIZE:
        raise NotCoff('shorter than a COFF header')
    first, second = struct.unpack_from('<HH', data, 0)
    if first == 0 and second == 0xFFFF:
        # The big-object header: Sig1, Sig2, Version, Machine, then the
        # section count at byte 44.
        if len(data) < BIG_HEADER_SIZE:
            raise NotCoff('shorter than a big-object header')
        version, machine = struct.unpack_from('<HH', data, 4)
        if version < 2:
            raise NotCoff('an import object, not a COFF object')
        sections: int = struct.unpack_from('<I', data, 44)[0]
        table: int = BIG_HEADER_SIZE
    else:
        machine = first
        sections = second
        optional: int = struct.unpack_from('<H', data, 16)[0]
        table = REGULAR_HEADER_SIZE + optional
    if machine not in COFF_MACHINES:
        raise NotCoff(f'machine 0x{machine:04x} is not a COFF target STXlib builds for')
    if table + sections * SECTION_HEADER_SIZE > len(data):
        raise NotCoff('the section table runs past the end of the file')
    pieces: list[str] = []
    for index in range(sections):
        header: int = table + index * SECTION_HEADER_SIZE
        name: bytes = data[header:header + 8].rstrip(b'\0')
        if name != DIRECTIVE_SECTION:
            continue
        size, offset = struct.unpack_from('<II', data, header + 16)
        if offset + size > len(data):
            raise NotCoff('the .drectve section runs past the end of the file')
        pieces.append(data[offset:offset + size].decode('latin-1'))
    joined: str = ' '.join(pieces)
    return joined


def exports_in(text: str) -> list[str]:
    """The export directives among the directive text's tokens."""
    found: list[str] = EXPORT_TOKEN.findall(text)
    return found


def objects_under(directory: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = sorted(
        path for path in directory.rglob('*') if path.is_file() and path.name.endswith(OBJECT_SUFFIXES))
    return found


def object_exports(path: pathlib.Path) -> list[str]:
    text: str = directive_text(path.read_bytes())
    found: list[str] = exports_in(text)
    return found


def main(argument_list: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--probe', required=True, type=pathlib.Path)
    parser.add_argument('--objects', action='append', required=True, type=pathlib.Path)
    arguments = parser.parse_args(argument_list)
    failed: bool = False

    probe_objects: list[pathlib.Path] = objects_under(arguments.probe)
    if not probe_objects:
        print(f'{arguments.probe}: no probe object; build the tests first')
        return 1
    for path in probe_objects:
        try:
            found: list[str] = object_exports(path)
        except NotCoff as error:
            print(f'{path}: {error}')
            failed = True
            continue
        if not found:
            print(f'{path}: the probe carries no export directive; the scan cannot see them')
            failed = True
        else:
            print(f'probe {path.name}: sees {", ".join(found)}')

    for directory in arguments.objects:
        objects: list[pathlib.Path] = objects_under(directory)
        if not objects:
            print(f'{directory}: no objects')
            failed = True
            continue
        for path in objects:
            try:
                found = object_exports(path)
            except NotCoff as error:
                print(f'{path}: {error}')
                failed = True
                continue
            for directive in found:
                print(f'{path}: {directive}')
                failed = True
        print(f'{directory.name}: {len(objects)} objects')

    print('stxlib static state exports: ' + ('FAILED' if failed else 'none'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
