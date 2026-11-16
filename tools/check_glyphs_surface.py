#!/usr/bin/env python3
"""Checks what stxlib::glyphs' compiled objects can reach outside themselves.

stxlib::glyphs (src/algorithms/glyphs) reads fonts an application hands it,
which may be anything. It states that it reaches nothing outside itself but
STXlib's allocator, STXlib's lock, STXlib's error record and STXlib's
requirement failure: no file, no stream, no environment, no clock, no host
call, no C library function, no exception runtime. This tool checks that
statement on the objects themselves, which is what runs: it lists every
symbol the glyph objects leave undefined, removes those another glyph object
defines, and fails unless every remaining one is on the list below or is a
helper the toolchain itself emits.

It is a statement about the whole call graph's leaves: a function reaches
outside code only through a symbol its object does not define. A leaf that
is not listed (malloc, fopen, getenv, a C++ stream, operator new, the
exception runtime) fails the check.

The leaves (proofs/glyphs_graph.md gives the reason for each), matched in
the Itanium and in the Microsoft spelling of the names:
  stxlib::detail::vocabulary_allocate, vocabulary_release   (the arrays)
  stxlib::ErrorRecord::make                                  (a refusal's record)
  stxlib::Mutex: its making, lock, unlock and unmaking       (a Typeface)
  stxlib::detail::requirement_failed                         (a broken requirement stops the program)

Toolchain helpers allowed on every host: block set and copy (memset, memcpy,
memmove, bzero, Apple's memset_pattern4/8/16, and the fortified _chk forms),
the stack protector's symbols, ___chkstk_ms, the global offset table, and
128-bit division helpers (__udivti3, __divti3, __umodti3, __modti3).

For the web family only, the wasm ABI's globals and table (as
check_stx_minmallocpp_surface.py has them).

In a sanitizer or integer-check build only (--sanitized), the
instrumentation's runtime entries are allowed too: symbols starting __asan_,
__lsan_, __msan_, __tsan_, __ubsan_ or __sanitizer_, and __cxa_atexit,
__dso_handle, __start_asan_globals and __stop_asan_globals.

The scan must be able to see such references: the objects must leave
vocabulary_allocate undefined, or the tool fails.

Usage:
  python3 -I tools/check_glyphs_surface.py --nm <nm> --objects <dir>
      [--under <prefix>] [--web] [--sanitized]

--objects is a directory of objects (CMake's CMakeFiles/<target>.dir);
--under keeps only objects whose path below it starts with that prefix. The
check assumes the objects were compiled without exceptions, as
stxlib_algorithms' are.

Exit status 0 when every leaf is listed, 1 otherwise; each finding is
printed as object: symbol.
"""

import argparse
import pathlib
import subprocess
import sys

OBJECT_SUFFIXES = ('.o', '.obj')

# STXlib's own, by what the mangled name contains: (Itanium, Microsoft).
STXLIB_LEAVES = (
    ('6stxlib6detail19vocabulary_allocateE', '?vocabulary_allocate@detail@stxlib@@'),
    ('6stxlib6detail18vocabulary_releaseE', '?vocabulary_release@detail@stxlib@@'),
    ('6stxlib11ErrorRecord4makeE', '?make@ErrorRecord@stxlib@@'),
    ('6stxlib5MutexC', '??0Mutex@stxlib@@'),
    ('6stxlib5MutexD', '??1Mutex@stxlib@@'),
    ('6stxlib5Mutex4lockEv', '?lock@Mutex@stxlib@@'),
    ('6stxlib5Mutex6unlockEv', '?unlock@Mutex@stxlib@@'),
    ('6stxlib6detail18requirement_failedE', '?requirement_failed@detail@stxlib@@'),
)

MUST_SEE = 'vocabulary_allocate'

WASM_ABI_GLOBALS = frozenset({'memory_base', 'table_base', 'stack_pointer', 'tls_base', 'indirect_function_table'})

TOOLCHAIN_HELPERS = frozenset({
    'memset', 'memcpy', 'memmove', 'bzero',
    'memset_pattern4', 'memset_pattern8', 'memset_pattern16',
    'memset_chk', 'memcpy_chk', 'memmove_chk',
    'stack_chk_fail', 'stack_chk_guard', 'chkstk_ms',
    'GLOBAL_OFFSET_TABLE_',
    'udivti3', 'divti3', 'umodti3', 'modti3',
})

SANITIZER_PREFIXES = ('asan_', 'lsan_', 'msan_', 'tsan_', 'ubsan_', 'sanitizer_')
SANITIZER_HELPERS = frozenset({'cxa_atexit', 'dso_handle', 'start_asan_globals', 'stop_asan_globals'})


def normalised(name: str) -> str:
    """The symbol without leading underscores, import prefix or ELF version."""
    bare: str = name.split('@@GLIBC', 1)[0]
    if not bare.startswith('?'):
        bare = bare.split('@', 1)[0].lstrip('_')
    if bare.startswith('imp_'):
        bare = bare[len('imp_'):].lstrip('_')
    return bare


def stxlib_leaf(name: str) -> bool:
    for itanium, microsoft in STXLIB_LEAVES:
        if itanium in name or microsoft in name:
            return True
    return False


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


def objects_under(directory: pathlib.Path, prefix: str) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in sorted(directory.rglob('*')):
        if not path.is_file() or not path.name.endswith(OBJECT_SUFFIXES):
            continue
        if prefix and not path.relative_to(directory).as_posix().startswith(prefix):
            continue
        found.append(path)
    return found


def allowed(name: str, web: bool, sanitized: bool) -> bool:
    if stxlib_leaf(name):
        return True
    bare: str = normalised(name)
    if bare in TOOLCHAIN_HELPERS:
        return True
    if web and bare in WASM_ABI_GLOBALS:
        return True
    result: bool = sanitized and (bare in SANITIZER_HELPERS or bare.startswith(SANITIZER_PREFIXES))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--nm', required=True)
    parser.add_argument('--objects', required=True, type=pathlib.Path)
    parser.add_argument('--under', default='')
    parser.add_argument('--web', action='store_true')
    parser.add_argument('--sanitized', action='store_true')
    arguments = parser.parse_args()

    objects: list[pathlib.Path] = objects_under(arguments.objects, arguments.under)
    if not objects:
        print(f'{arguments.objects}: no objects under "{arguments.under}"; build first')
        return 1

    defined_anywhere: set[str] = set()
    undefined_by_object: dict[pathlib.Path, set[str]] = {}
    for path in objects:
        defined, undefined = symbols_of(arguments.nm, path)
        defined_anywhere |= defined
        undefined_by_object[path] = undefined

    failed: bool = False
    leaves: set[str] = set()
    for path in objects:
        for name in sorted(undefined_by_object[path] - defined_anywhere):
            leaves.add(name)
            if not allowed(name, arguments.web, arguments.sanitized):
                print(f'{path}: {name}')
                failed = True

    if not any(MUST_SEE in name for name in leaves):
        print(f'the objects do not name {MUST_SEE}; the scan cannot see what they reach on this toolchain')
        failed = True

    print(f'{len(objects)} objects; {len(leaves)} leaves: {", ".join(sorted(leaves))}')
    print('stxlib::glyphs surface: ' + ('FAILED' if failed else 'as listed'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
