#!/usr/bin/env python3
"""Checks what stx_minmallocpp's compiled objects can reach outside themselves.

stx_minmallocpp (src/state/stx_minmallocpp, with its host layer under
src/hosted) states that it calls nothing but a short list of host
functions, none of which can unwind, call back or act on a thread
cancellation (os.hpp). This tool checks that statement on the objects
themselves, which is what runs: it lists every symbol the allocator's
objects leave undefined, removes those another of its objects defines, and
fails unless every remaining one is on the list for the host family or is a
helper the toolchain itself emits.

It is a statement about the whole call graph's leaves: a function reaches
outside code only through a symbol its object does not define. A leaf that
is not listed (malloc, getenv, write, a stream, a lock of the C library,
the exception runtime, a thread-local initialiser the compiler should not
have needed) fails the check.

The host lists (os.hpp gives the reason for each call):
  posix    mmap mprotect munmap sched_yield sysconf pthread_key_create
           pthread_setspecific pthread_getspecific abort
  windows  VirtualAlloc VirtualFree GetSystemInfo SwitchToThread abort,
           GetModuleHandleW GetProcAddress (ntdll's
           RtlDllShutdownInProgress, looked up once at process attach),
           and _tls_used (the C runtime's TLS directory, named so that the
           thread-exit callback is kept)
  web      emscripten_builtin_memalign emscripten_builtin_free (the
           address space, minmallocpp_os_emscripten.cpp) and the POSIX
           list's sched_yield sysconf pthread_key_create
           pthread_setspecific pthread_getspecific abort; no mmap,
           mprotect or munmap

Toolchain helpers allowed on every host: block set and copy (memset,
memcpy, memmove, bzero), the thread-local access routines (__tls_get_addr,
_tlv_bootstrap, __emutls_get_address and the __emutls_v. control objects,
and _tls_index, the PE image's TLS slot that Windows thread-local access
reads),
the stack protector's symbols, ___chkstk_ms, and the global offset table.

For the web family only, the wasm ABI's globals and table, which the
compiler names in every position-independent object and which are no
calls: __memory_base, __table_base, __stack_pointer, __tls_base and
__indirect_function_table (WebAssembly tool conventions, DynamicLinking.md,
read 2026-11-15). They are refused for every other family.

In a sanitizer or integer-check build only (--sanitized, which CMake passes
when the flags enable -fsanitize= or STXLIB_INTEGER_CHECKS is on), the
instrumentation's runtime entries are allowed too: symbols starting
__asan_, __lsan_, __msan_, __tsan_, __ubsan_ or __sanitizer_, and
__cxa_atexit, __dso_handle and the ELF section bounds __start_asan_globals
and __stop_asan_globals, through which AddressSanitizer registers each
module's globals. Every other build is checked exactly as above.

The scan must be able to see such references: the objects must leave the
family's reservation call undefined (mmap, VirtualAlloc, or
emscripten_builtin_memalign), or the tool
fails.

Usage:
  python3 -I tools/check_stx_minmallocpp_surface.py --nm <nm>
      --family posix|windows|web --objects <dir> [--under <prefix>] [--sanitized]

--objects is a directory of objects (CMake's CMakeFiles/<target>.dir);
--under keeps only objects whose path below it starts with that prefix
(for a target that also holds other objects). The check assumes the
objects were compiled without exceptions, as stxlib_state's are.

Exit status 0 when every leaf is listed, 1 otherwise; each finding is
printed as object: symbol.
"""

import argparse
import pathlib
import subprocess
import sys

OBJECT_SUFFIXES = ('.o', '.obj')

HOST_CALLS = {
    'posix': frozenset({
        'mmap', 'mprotect', 'munmap', 'sched_yield', 'sysconf',
        'pthread_key_create', 'pthread_setspecific', 'pthread_getspecific', 'abort',
    }),
    'windows': frozenset({
        'VirtualAlloc', 'VirtualFree', 'GetSystemInfo', 'SwitchToThread', 'abort', 'tls_used',
        'GetModuleHandleW', 'GetProcAddress',
    }),
    'web': frozenset({
        'emscripten_builtin_memalign', 'emscripten_builtin_free', 'sched_yield', 'sysconf',
        'pthread_key_create', 'pthread_setspecific', 'pthread_getspecific', 'abort',
    }),
}

MUST_SEE = {'posix': 'mmap', 'windows': 'VirtualAlloc', 'web': 'emscripten_builtin_memalign'}

# The web family only: the wasm ABI's globals and table (normalised names).
WASM_ABI_GLOBALS = frozenset({'memory_base', 'table_base', 'stack_pointer', 'tls_base', 'indirect_function_table'})

TOOLCHAIN_HELPERS = frozenset({
    'memset', 'memcpy', 'memmove', 'bzero',
    'tls_get_addr', 'tlv_bootstrap', 'emutls_get_address', 'tls_index',
    'stack_chk_fail', 'stack_chk_guard', 'chkstk_ms',
    'GLOBAL_OFFSET_TABLE_',
})

TOOLCHAIN_PREFIXES = ('emutls_v.',)

# Sanitizer and integer-check builds only (--sanitized): the runtime's entries.
SANITIZER_PREFIXES = ('asan_', 'lsan_', 'msan_', 'tsan_', 'ubsan_', 'sanitizer_')
SANITIZER_HELPERS = frozenset({'cxa_atexit', 'dso_handle', 'start_asan_globals', 'stop_asan_globals'})


def normalised(name: str) -> str:
    """The symbol without leading underscores, import prefix or ELF version."""
    bare: str = name.split('@', 1)[0].lstrip('_')
    if bare.startswith('imp_'):
        bare = bare[len('imp_'):].lstrip('_')
    return bare


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


def allowed(name: str, family: str, sanitized: bool) -> bool:
    bare: str = normalised(name)
    if bare in HOST_CALLS[family] or bare in TOOLCHAIN_HELPERS:
        return True
    if family == 'web' and bare in WASM_ABI_GLOBALS:
        return True
    if sanitized and (bare in SANITIZER_HELPERS or bare.startswith(SANITIZER_PREFIXES)):
        return True
    result: bool = bare.startswith(TOOLCHAIN_PREFIXES)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--nm', required=True)
    parser.add_argument('--family', required=True, choices=sorted(HOST_CALLS))
    parser.add_argument('--objects', required=True, type=pathlib.Path)
    parser.add_argument('--under', default='')
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
            leaves.add(normalised(name))
            if not allowed(name, arguments.family, arguments.sanitized):
                print(f'{path}: {name}')
                failed = True

    if MUST_SEE[arguments.family] not in leaves:
        print(f'the objects do not name {MUST_SEE[arguments.family]}; the scan cannot see host calls on this toolchain')
        failed = True

    print(f'{len(objects)} objects; leaves: {", ".join(sorted(leaves))}')
    print('stx_minmallocpp host surface: ' + ('FAILED' if failed else 'as listed'))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
