#!/usr/bin/env python3
"""Checks what the text layout objects can reach outside themselves.

src/algorithms/textlayout (include/stxlib/textlayout.hpp) states that it
calls nothing outside STXlib: no C library function, no C++ runtime entry
(no operator new, no exception machinery, no guard for a local static), no
stream, no locale. This tool checks that statement on the compiled objects,
which is what runs: it lists every symbol the two objects leave undefined,
removes those the other defines, and fails unless each remaining one is

  - a symbol of namespace stxlib (the workspaces' storage goes through
    stxlib's allocation entry, and failures are stxlib's Status and Error):
    an Itanium-mangled name with the component "6stxlib"; or
  - a helper the toolchain itself emits: block set and copy (memset,
    memcpy, memmove, bzero), the stack protector's symbols, the global
    offset table.

It is a statement about the call graph's leaves: a function reaches outside
code only through a symbol its object does not define. The stxlib symbols
are printed, so the report can name them.

In a sanitizer or integer-check build only (--sanitized), the
instrumentation's runtime entries are allowed too, as in
tools/check_stx_minmallocpp_surface.py.

The scan must be able to see such references: the objects must leave at
least one stxlib symbol undefined, or the tool fails.

Usage:
  python3 -I tools/check_textlayout_surface.py --nm <nm>
      --objects <CMakeFiles/stxlib_algorithms.dir> [--sanitized]
"""

import argparse
import pathlib
import subprocess
import sys

OBJECT_SUFFIXES = ('.o', '.obj')
UNDER = 'src/algorithms/textlayout/'

TOOLCHAIN_HELPERS = frozenset({
    'memset', 'memcpy', 'memmove', 'bzero',
    'stack_chk_fail', 'stack_chk_guard', 'chkstk_ms', 'GLOBAL_OFFSET_TABLE_',
})

SANITIZER_PREFIXES = ('asan_', 'lsan_', 'msan_', 'tsan_', 'ubsan_', 'sanitizer_')
SANITIZER_HELPERS = frozenset({'cxa_atexit', 'dso_handle', 'start_asan_globals', 'stop_asan_globals'})


def normalised(name: str) -> str:
    """The symbol without leading underscores or ELF version."""
    bare: str = name.split('@', 1)[0].lstrip('_')
    return bare


def of_stxlib(name: str) -> bool:
    bare: str = normalised(name)
    result: bool = bare.startswith('Z') and '6stxlib' in bare
    return result


def symbols_of(nm: str, path: pathlib.Path) -> tuple[set[str], set[str]]:
    """The names one object defines, and the names it leaves undefined."""
    completed = subprocess.run([nm, '-P', str(path)], capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f'{path}: {nm} failed: {completed.stderr.strip()}')
    defined: set[str] = set()
    undefined: set[str] = set()
    for line in completed.stdout.splitlines():
        fields: list[str] = line.split()
        if len(fields) < 2 or line.rstrip().endswith(':'):
            continue
        if fields[1] in ('U', 'w', 'v'):
            undefined.add(fields[0])
        else:
            defined.add(fields[0])
    return defined, undefined


def allowed(name: str, sanitized: bool) -> bool:
    bare: str = normalised(name)
    if of_stxlib(name) or bare in TOOLCHAIN_HELPERS:
        return True
    result: bool = sanitized and (bare in SANITIZER_HELPERS or bare.startswith(SANITIZER_PREFIXES))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--nm', required=True)
    parser.add_argument('--objects', required=True, type=pathlib.Path)
    parser.add_argument('--sanitized', action='store_true')
    arguments = parser.parse_args()

    objects: list[pathlib.Path] = []
    for path in sorted(arguments.objects.rglob('*')):
        if path.is_file() and path.name.endswith(OBJECT_SUFFIXES) and UNDER in path.as_posix():
            objects.append(path)
    if not objects:
        print(f'{arguments.objects}: no objects under {UNDER}; build first')
        return 1

    defined_anywhere: set[str] = set()
    undefined_by_object: dict[pathlib.Path, set[str]] = {}
    for path in objects:
        defined, undefined = symbols_of(arguments.nm, path)
        defined_anywhere |= defined
        undefined_by_object[path] = undefined

    failed: bool = False
    own: set[str] = set()
    helpers: set[str] = set()
    for path in objects:
        for name in sorted(undefined_by_object[path] - defined_anywhere):
            if of_stxlib(name):
                own.add(name)
            else:
                helpers.add(normalised(name))
            if not allowed(name, arguments.sanitized):
                print(f'{path}: {name}')
                failed = True

    if not own:
        print('the objects name no stxlib symbol; the scan cannot see calls on this toolchain')
        failed = True

    print(f'{len(objects)} objects; stxlib symbols reached: {len(own)}')
    for name in sorted(own):
        print(f'  {name}')
    print('other leaves: ' + (', '.join(sorted(helpers)) if helpers else 'none'))
    print('text layout surface: ' + ('FAILED' if failed else 'as listed'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
