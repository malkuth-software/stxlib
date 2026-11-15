#!/usr/bin/env python3
"""Checks that STXlib's objects refer to no part of the exception runtime.

STXlib's own code is built without exceptions (AGENTS.md; notes
planning/stages/A3-no-exceptions.md, section 1.5). This tool lists the
symbols of every object file under each --objects directory with the
toolchain's nm and fails when one names a symbol of the exception runtime:
throwing, rethrowing, catching, the personality routines and the unwinder's
resume, under the Itanium C++ ABI (macOS, Linux, LLVM MinGW) and, for
completeness, the MSVC names. An object compiled without exceptions has no
landing pads and calls none of them, so no reference means no code in that
object throws, catches or runs a cleanup during unwinding.

The scan must be able to see such references: every object under --probe
(compiled with exceptions on purpose) must name at least one, or the tool
fails. Objects whose file names start with an --allow prefix are skipped
(the exception recorder while STXLIB_EXCEPTIONS is ON).

Usage:
  python3 -I tools/check_no_exceptions.py --nm <nm> --probe <dir>
      [--allow <prefix>]... --objects <dir> [--objects <dir>]...

Exit status 0 when every object passes and the probe shows the runtime, 1
otherwise; each finding is printed as object: symbol.
"""

import argparse
import pathlib
import subprocess
import sys

# Symbol names with their leading underscores removed (Mach-O adds one, and
# the Itanium names start with two).
EXCEPTION_RUNTIME = frozenset({
    'cxa_allocate_exception',
    'cxa_free_exception',
    'cxa_throw',
    'cxa_rethrow',
    'cxa_begin_catch',
    'cxa_end_catch',
    'cxa_get_exception_ptr',
    'cxa_current_exception_type',
    'cxa_call_unexpected',
    'gxx_personality_v0',
    'gxx_personality_seh0',
    'gxx_personality_sj0',
    'gcc_personality_v0',
    'Unwind_Resume',
    'Unwind_Resume_or_Rethrow',
    'Unwind_RaiseException',
    'Unwind_SjLj_Resume',
    'clang_call_terminate',
    'CxxFrameHandler3',
    'CxxFrameHandler4',
    'CxxThrowException',
})

OBJECT_SUFFIXES = ('.o', '.obj')


def normalised(name: str) -> str:
    """The symbol without leading underscores, import prefix or ELF version."""
    bare: str = name.split('@', 1)[0].lstrip('_')
    if bare.startswith('imp_'):
        bare = bare[len('imp_'):].lstrip('_')
    return bare


def symbols_of(nm: str, path: pathlib.Path) -> list[str]:
    """Every symbol name nm lists for one object, defined or referenced."""
    completed = subprocess.run([nm, '-P', str(path)], capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f'{path}: {nm} failed: {completed.stderr.strip()}')
    names: list[str] = []
    for line in completed.stdout.splitlines():
        fields: list[str] = line.split()
        if len(fields) < 2 or line.rstrip().endswith(':'):
            continue
        names.append(fields[0])
    return names


def objects_under(directory: pathlib.Path) -> list[pathlib.Path]:
    found: list[pathlib.Path] = sorted(
        path for path in directory.rglob('*') if path.is_file() and path.name.endswith(OBJECT_SUFFIXES))
    return found


def runtime_references(nm: str, path: pathlib.Path) -> tuple[int, list[str]]:
    names: list[str] = symbols_of(nm, path)
    found: list[str] = sorted({name for name in names if normalised(name) in EXCEPTION_RUNTIME})
    return len(names), found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--nm', required=True)
    parser.add_argument('--probe', required=True, type=pathlib.Path)
    parser.add_argument('--allow', action='append', default=[])
    parser.add_argument('--objects', action='append', required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    failed: bool = False

    probe_objects: list[pathlib.Path] = objects_under(arguments.probe)
    if not probe_objects:
        print(f'{arguments.probe}: no probe object; build the tests first')
        return 1
    for path in probe_objects:
        count, found = runtime_references(arguments.nm, path)
        if not found:
            print(f'{path}: the probe names no exception-runtime symbol ({count} symbols); '
                  'the scan cannot see them on this toolchain')
            failed = True
        else:
            print(f'probe {path.name}: sees {len(found)} ({", ".join(found)})')

    for directory in arguments.objects:
        objects: list[pathlib.Path] = objects_under(directory)
        if not objects:
            print(f'{directory}: no objects')
            failed = True
            continue
        allowed: int = 0
        without_symbols: list[str] = []
        listed: int = 0
        for path in objects:
            if any(path.name.startswith(prefix) for prefix in arguments.allow):
                allowed += 1
                continue
            count, found = runtime_references(arguments.nm, path)
            if count == 0:
                without_symbols.append(path.name)
            else:
                listed += 1
            for name in found:
                print(f'{path}: {name}')
                failed = True
        if listed == 0:
            print(f'{directory}: nm listed no symbol in any checked object')
            failed = True
        note: str = f', no symbols in {", ".join(without_symbols)}' if without_symbols else ''
        print(f'{directory.name}: {len(objects)} objects, {allowed} allowed{note}')

    print('stxlib no-exception objects: ' + ('FAILED' if failed else 'clean'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
