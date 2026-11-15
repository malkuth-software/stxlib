#!/usr/bin/env python3
"""Tests of tools/check_stx_minmallocpp_surface.py (the allocator's leaves).

Usage: python3 -I tests/check_stx_minmallocpp_surface_tests.py <repository root>

The tool is loaded as a module and its symbol reader is replaced by a table,
so the cases run on every host without an nm. Properties:
  S1  each family's host calls pass for that family and fail for the others
      where they are not listed (mmap on the web, the Emscripten allocator
      calls on POSIX);
  S2  the wasm ABI globals pass for the web family only, as named and with
      their leading underscores;
  S3  the toolchain helpers pass for every family; sanitizer entries pass
      only with --sanitized;
  S4  a whole run: the web family's leaves as the objects leave them pass,
      a stray leaf fails, and a run whose objects never name the family's
      reservation call fails.
"""

import importlib.util
import pathlib
import sys

failures: list[str] = []
cases: int = 0


def check(condition: bool, label: str) -> None:
    global cases
    cases = cases + 1
    if not condition:
        failures.append(label)


def load_tool(root: pathlib.Path):
    path = root / 'tools' / 'check_stx_minmallocpp_surface.py'
    spec = importlib.util.spec_from_file_location('check_stx_minmallocpp_surface', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def families(tool) -> None:
    check(tool.allowed('mmap', 'posix', False), 'S1 mmap on posix')
    check(not tool.allowed('mmap', 'web', False), 'S1 mmap refused on web')
    check(not tool.allowed('mprotect', 'web', False), 'S1 mprotect refused on web')
    check(not tool.allowed('munmap', 'web', False), 'S1 munmap refused on web')
    check(tool.allowed('emscripten_builtin_memalign', 'web', False), 'S1 memalign on web')
    check(tool.allowed('emscripten_builtin_free', 'web', False), 'S1 free on web')
    check(not tool.allowed('emscripten_builtin_memalign', 'posix', False), 'S1 memalign refused on posix')
    check(not tool.allowed('emscripten_builtin_malloc', 'web', False), 'S1 malloc refused on web')
    for name in ('sched_yield', 'sysconf', 'pthread_key_create', 'pthread_setspecific', 'pthread_getspecific', 'abort'):
        check(tool.allowed(name, 'web', False), 'S1 ' + name + ' on web')
    check(tool.allowed('VirtualAlloc', 'windows', False), 'S1 VirtualAlloc on windows')
    check(not tool.allowed('VirtualAlloc', 'web', False), 'S1 VirtualAlloc refused on web')


def wasm_globals(tool) -> None:
    for name in ('__memory_base', '__table_base', '__stack_pointer', '__tls_base', '__indirect_function_table'):
        check(tool.allowed(name, 'web', False), 'S2 ' + name + ' on web')
        check(not tool.allowed(name, 'posix', False), 'S2 ' + name + ' refused on posix')
        check(not tool.allowed(name, 'windows', False), 'S2 ' + name + ' refused on windows')


def helpers(tool) -> None:
    for family in ('posix', 'windows', 'web'):
        check(tool.allowed('memset', family, False), 'S3 memset on ' + family)
        check(tool.allowed('__tls_get_addr', family, False), 'S3 tls_get_addr on ' + family)
        check(not tool.allowed('__asan_report_load8', family, False), 'S3 asan refused unsanitized on ' + family)
        check(tool.allowed('__asan_report_load8', family, True), 'S3 asan with --sanitized on ' + family)
        check(not tool.allowed('getenv', family, True), 'S3 getenv refused on ' + family)


def run_main(tool, directory: pathlib.Path, table: dict[str, tuple[set[str], set[str]]]) -> int:
    def objects_under(found: pathlib.Path, prefix: str) -> list[pathlib.Path]:
        return [directory / name for name in sorted(table)]

    def symbols_of(nm: str, path: pathlib.Path) -> tuple[set[str], set[str]]:
        return table[path.name]

    tool.objects_under = objects_under
    tool.symbols_of = symbols_of
    saved = sys.argv
    sys.argv = ['check', '--nm', 'nm', '--family', 'web', '--objects', str(directory)]
    try:
        status = tool.main()
    finally:
        sys.argv = saved
    return status


def whole_runs(root: pathlib.Path) -> None:
    directory = pathlib.Path('objects')
    # The leaves the web run of 2026-11-15 showed, with the POSIX address
    # space replaced by the Emscripten one.
    good = {
        'minmallocpp_os_emscripten.cpp.o': ({'_ZN6stxlib15stx_minmallocpp2os7reserveEmm'},
                                            {'emscripten_builtin_memalign', 'emscripten_builtin_free', 'memset',
                                             '__memory_base', '__stack_pointer'}),
        'minmallocpp_os_posix.cpp.o': ({'_ZN6stxlib15stx_minmallocpp2os5yieldEv'},
                                       {'sched_yield', 'sysconf', 'pthread_key_create', 'pthread_setspecific',
                                        'pthread_getspecific', 'abort', '__tls_base', '__table_base',
                                        '__indirect_function_table'}),
        'region.cpp.o': (set(), {'_ZN6stxlib15stx_minmallocpp2os7reserveEmm', '__memory_base'}),
    }
    check(run_main(load_tool(root), directory, good) == 0, 'S4 the web leaves pass')
    stray = dict(good)
    stray['heap.cpp.o'] = (set(), {'malloc'})
    check(run_main(load_tool(root), directory, stray) == 1, 'S4 a stray leaf fails')
    unseen = {'region.cpp.o': (set(), {'memset', '__memory_base'})}
    check(run_main(load_tool(root), directory, unseen) == 1, 'S4 no reservation call seen fails')
    posix_on_web = dict(good)
    posix_on_web['old.cpp.o'] = (set(), {'mmap', 'mprotect'})
    check(run_main(load_tool(root), directory, posix_on_web) == 1, 'S4 mmap and mprotect fail on web')


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_stx_minmallocpp_surface_tests.py <repository root>')
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    families(load_tool(root))
    wasm_globals(load_tool(root))
    helpers(load_tool(root))
    whole_runs(root)
    for label in failures:
        print('FAILED: ' + label)
    print(f'check_stx_minmallocpp_surface tests: {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
