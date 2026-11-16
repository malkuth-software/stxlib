#!/usr/bin/env python3
"""Tests of tools/check_glyphs_surface.py (the glyph code's leaves).

Usage: python3 -I tests/check_glyphs_surface_tests.py <repository root>

The tool is loaded as a module and its symbol reader is replaced by a table,
so the cases run on every host without an nm. Properties:
  G1  STXlib's own leaves pass in the Itanium spelling (with and without the
      leading underscore a host adds) and in the Microsoft one; other names
      of STXlib's do not;
  G2  the toolchain helpers pass; the C library, the C++ runtime and the
      exception runtime do not; sanitizer entries pass only with --sanitized;
      the wasm globals only with --web;
  G3  a whole run: the leaves as the objects leave them pass, a stray leaf
      fails, a leaf another glyph object defines is no leaf, and a run whose
      objects never name the allocator fails.
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
    path = root / 'tools' / 'check_glyphs_surface.py'
    spec = importlib.util.spec_from_file_location('check_glyphs_surface', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ALLOCATE = '_ZN6stxlib6detail19vocabulary_allocateEmm'
RELEASE = '_ZN6stxlib6detail18vocabulary_releaseEPv'
RECORD = '_ZN6stxlib11ErrorRecord4makeENS_9ErrorKindENS_9OperationE'
LOCK = '_ZN6stxlib5Mutex4lockEv'
UNLOCK = '_ZN6stxlib5Mutex6unlockEv'
MADE = '_ZN6stxlib5MutexC1Ev'
UNMADE = '_ZN6stxlib5MutexD1Ev'
FAILED = '_ZN6stxlib6detail18requirement_failedEPKcS2_i'


def own_leaves(tool) -> None:
    for name in (ALLOCATE, RELEASE, RECORD, LOCK, UNLOCK, MADE, UNMADE, FAILED):
        check(tool.allowed(name, False, False), 'G1 ' + name)
        check(tool.allowed('_' + name, False, False), 'G1 _' + name)
    for name in ('?vocabulary_allocate@detail@stxlib@@YAPEAX_K0@Z', '?make@ErrorRecord@stxlib@@SA?AV12@W4ErrorKind@2@VOperation@2@@Z',
                 '?lock@Mutex@stxlib@@QEAAXXZ', '??0Mutex@stxlib@@QEAA@XZ', '??1Mutex@stxlib@@QEAA@XZ'):
        check(tool.allowed(name, False, False), 'G1 ' + name)
    for name in ('_ZN6stxlib5files9read_fileENS_8TextViewE', '_ZN6stxlib6detail15allocate_tracedEm', '_ZN6stxlib5Mutex8try_lockEv',
                 '_ZN6stxlib7console5writeENS_8TextViewE'):
        check(not tool.allowed(name, False, False), 'G1 refused ' + name)


def helpers(tool) -> None:
    for name in ('memcpy', '_memcpy', '_bzero', '_memset_pattern16', '___stack_chk_fail', '___stack_chk_guard', '__memcpy_chk', '__udivti3'):
        check(tool.allowed(name, False, False), 'G2 ' + name)
    for name in ('malloc', '_free', 'fopen', '_getenv', 'printf', '_Znwm', '_ZdlPv', '__cxa_throw', '__cxa_begin_catch', '__gxx_personality_v0',
                 '_Unwind_Resume', 'pthread_mutex_lock', 'abort', 'strlen', '_ZNSt3__14coutE', 'time', 'read', 'open'):
        check(not tool.allowed(name, False, False), 'G2 refused ' + name)
        check(not tool.allowed(name, True, True), 'G2 refused everywhere ' + name)
    check(not tool.allowed('__asan_report_load8', False, False), 'G2 asan refused unsanitized')
    check(tool.allowed('__asan_report_load8', False, True), 'G2 asan with --sanitized')
    check(tool.allowed('__ubsan_handle_add_overflow', False, True), 'G2 ubsan with --sanitized')
    check(not tool.allowed('__stack_pointer', False, False), 'G2 wasm global refused off the web')
    check(tool.allowed('__stack_pointer', True, False), 'G2 wasm global on the web')


def run_main(tool, table: dict[str, tuple[set[str], set[str]]], extra: list[str]) -> int:
    directory = pathlib.Path('objects')

    def objects_under(found: pathlib.Path, prefix: str) -> list[pathlib.Path]:
        return [found / name for name in sorted(table) if name.startswith(prefix)]

    def symbols_of(nm: str, path: pathlib.Path) -> tuple[set[str], set[str]]:
        return table[path.name]

    tool.objects_under = objects_under
    tool.symbols_of = symbols_of
    saved = sys.argv
    sys.argv = ['check_glyphs_surface.py', '--nm', 'none', '--objects', str(directory)] + extra
    try:
        status: int = tool.main()
    finally:
        sys.argv = saved
    return status


def whole_runs(tool) -> None:
    good = {
        'a.o': ({'_first'}, {ALLOCATE, RELEASE, '_memcpy', '_second'}),
        'b.o': ({'_second'}, {RECORD, LOCK, UNLOCK, '___stack_chk_fail', '_first'}),
    }
    check(run_main(tool, good, []) == 0, 'G3 the listed leaves pass')
    stray = {'a.o': ({'_first'}, {ALLOCATE, '_fopen'})}
    check(run_main(tool, stray, []) == 1, 'G3 a stray leaf fails')
    blind = {'a.o': ({'_first'}, {'_memcpy'})}
    check(run_main(tool, blind, []) == 1, 'G3 objects that never name the allocator fail')
    sanitized = {'a.o': ({'_first'}, {ALLOCATE, '___asan_init'})}
    check(run_main(tool, sanitized, []) == 1, 'G3 a sanitizer entry fails unsanitized')
    check(run_main(tool, sanitized, ['--sanitized']) == 0, 'G3 and passes with --sanitized')


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    tool = load_tool(pathlib.Path(sys.argv[1]))
    own_leaves(tool)
    helpers(tool)
    whole_runs(tool)
    for label in failures:
        print('FAILED: ' + label)
    print(f'check_glyphs_surface_tests: {cases - len(failures)} checks passed, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
