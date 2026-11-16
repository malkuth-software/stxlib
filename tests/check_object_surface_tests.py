#!/usr/bin/env python3
"""Tests of tools/check_object_surface.py on tools/object_surface/stx_minmallocpp.toml.

Usage: python3 -I tests/check_object_surface_tests.py <repository root>

The tool is template_harness's harness/check_object_surface.py, kept
identical here; its own suite is harness/test_object_surface.py there. These
cases check STXlib's use of it: the allocator's component file. The symbol
reader is replaced by a table, so they run on every host without an nm.
Properties:
  S1  each family's host calls pass for that family and fail for the others
      where they are not listed (mmap on the web, the Emscripten allocator
      calls on POSIX);
  S2  the wasm ABI globals pass for the web family only, as named and with
      their leading underscores;
  S3  the toolchain helpers pass for every family; sanitizer entries pass
      only with --sanitized;
  S4  a whole run: the web family's leaves as the objects leave them pass,
      a stray leaf fails, and a run whose objects never name the family's
      reservation call fails;
  S5  every allowed entry of the component file has a reason (the file
      loads), and the allowance is shrink-only (a stale line fails).
"""

import importlib.util
import io
import pathlib
import sys
import tempfile
from contextlib import redirect_stdout

failures: list[str] = []
cases: int = 0
Table = dict[str, tuple[set[str], set[str]]]


def check(condition: bool, label: str) -> None:
    global cases
    cases = cases + 1
    if not condition:
        failures.append(label)


def load_tool(root: pathlib.Path):
    path = root / 'tools' / 'check_object_surface.py'
    spec = importlib.util.spec_from_file_location('check_object_surface', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['check_object_surface'] = module
    spec.loader.exec_module(module)
    return module


def families(tool, component) -> None:
    def allowed(name: str, family: str, sanitized: bool) -> bool:
        return tool.is_allowed(component, family, name, sanitized)

    check(allowed('mmap', 'posix', False), 'S1 mmap on posix')
    check(not allowed('mmap', 'web', False), 'S1 mmap refused on web')
    check(not allowed('mprotect', 'web', False), 'S1 mprotect refused on web')
    check(not allowed('munmap', 'web', False), 'S1 munmap refused on web')
    check(allowed('emscripten_builtin_memalign', 'web', False), 'S1 memalign on web')
    check(allowed('emscripten_builtin_free', 'web', False), 'S1 free on web')
    check(not allowed('emscripten_builtin_memalign', 'posix', False), 'S1 memalign refused on posix')
    check(not allowed('emscripten_builtin_malloc', 'web', False), 'S1 malloc refused on web')
    for name in ('sched_yield', 'sysconf', 'pthread_key_create', 'pthread_setspecific', 'pthread_getspecific', 'abort'):
        check(allowed(name, 'web', False), 'S1 ' + name + ' on web')
    check(allowed('VirtualAlloc', 'windows', False), 'S1 VirtualAlloc on windows')
    check(allowed('__imp_VirtualAlloc', 'windows', False), 'S1 imported VirtualAlloc on windows')
    check(allowed('_tls_used', 'windows', False), 'S1 _tls_used on windows')
    check(not allowed('VirtualAlloc', 'web', False), 'S1 VirtualAlloc refused on web')
    for name in ('__memory_base', '__table_base', '__stack_pointer', '__tls_base', '__indirect_function_table'):
        check(allowed(name, 'web', False), 'S2 ' + name + ' on web')
        check(not allowed(name, 'posix', False), 'S2 ' + name + ' refused on posix')
        check(not allowed(name, 'windows', False), 'S2 ' + name + ' refused on windows')
    for family in ('posix', 'windows', 'web'):
        check(allowed('memset', family, False), 'S3 memset on ' + family)
        check(allowed('__tls_get_addr', family, False), 'S3 tls_get_addr on ' + family)
        check(allowed('__emutls_v.slot', family, False), 'S3 emutls control object on ' + family)
        check(not allowed('__asan_report_load8', family, False), 'S3 asan refused unsanitized on ' + family)
        check(allowed('__asan_report_load8', family, True), 'S3 asan with --sanitized on ' + family)
        check(not allowed('getenv', family, True), 'S3 getenv refused on ' + family)


def run_main(tool, config: pathlib.Path, table: Table, *extra: str) -> int:
    def read(nm: str, path: pathlib.Path) -> tuple[set[str], set[str]]:
        return table[path.name]

    def find(directory: pathlib.Path, prefix: str) -> list[pathlib.Path]:
        return [directory / name for name in sorted(table)]

    with redirect_stdout(io.StringIO()):
        status: int = tool.main(['--config', str(config), '--nm', 'nm', '--family', 'web',
                                 '--objects', 'objects', *extra], read, find)
    return status


def whole_runs(tool, config: pathlib.Path) -> None:
    # The leaves the web run of 2026-11-15 showed, with the POSIX address
    # space replaced by the Emscripten one.
    good: Table = {
        'minmallocpp_os_emscripten.cpp.o': ({'_ZN6stxlib15stx_minmallocpp2os7reserveEmm'},
                                            {'emscripten_builtin_memalign', 'emscripten_builtin_free', 'memset',
                                             '__memory_base', '__stack_pointer'}),
        'minmallocpp_os_posix.cpp.o': ({'_ZN6stxlib15stx_minmallocpp2os5yieldEv'},
                                       {'sched_yield', 'sysconf', 'pthread_key_create', 'pthread_setspecific',
                                        'pthread_getspecific', 'abort', '__tls_base', '__table_base',
                                        '__indirect_function_table'}),
        'region.cpp.o': (set(), {'_ZN6stxlib15stx_minmallocpp2os7reserveEmm', '__memory_base'}),
    }
    check(run_main(tool, config, good) == tool.EXIT_PASS, 'S4 the web leaves pass')
    stray: Table = dict(good)
    stray['heap.cpp.o'] = (set(), {'malloc'})
    check(run_main(tool, config, stray) == tool.EXIT_FINDINGS, 'S4 a stray leaf fails')
    unseen: Table = {'region.cpp.o': (set(), {'memset', '__memory_base'})}
    check(run_main(tool, config, unseen) == tool.EXIT_FINDINGS, 'S4 no reservation call seen fails')
    posix_on_web: Table = dict(good)
    posix_on_web['old.cpp.o'] = (set(), {'mmap', 'mprotect'})
    check(run_main(tool, config, posix_on_web) == tool.EXIT_FINDINGS, 'S4 mmap and mprotect fail on web')
    with tempfile.TemporaryDirectory() as scratch:
        allowance = pathlib.Path(scratch) / 'allowance.txt'
        allowance.write_text('web malloc 1\n', encoding='utf-8')
        check(run_main(tool, config, stray, '--allowance', str(allowance)) == tool.EXIT_PASS,
              'S5 a leaf within the allowance passes')
        check(run_main(tool, config, good, '--allowance', str(allowance)) == tool.EXIT_FINDINGS,
              'S5 a stale allowance line fails')


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_object_surface_tests.py <repository root>')
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    tool = load_tool(root)
    config = root / 'tools' / 'object_surface' / 'stx_minmallocpp.toml'
    component = tool.load_component(config)
    check(component.name == 'stx_minmallocpp', 'S5 the component file loads, every entry with a reason')
    families(tool, component)
    whole_runs(tool, config)
    for label in failures:
        print('FAILED: ' + label)
    print(f'check_object_surface tests (stx_minmallocpp): {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
