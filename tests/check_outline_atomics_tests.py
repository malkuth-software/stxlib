#!/usr/bin/env python3
"""Tests of tools/check_outline_atomics.py (the outline-atomics link check).

Usage: python3 -I tests/check_outline_atomics_tests.py <repository root>

The tool is loaded as a module and given symbol tables instead of nm, so the
cases run on every host. Properties:
  L1  a binary whose helpers all lie between STXlib's markers passes;
  L2  a helper outside the markers fails (OUTSIDE), as does one left
      undefined (UNDEFINED);
  L3  the toolchain's memo or constructor fails, defined or undefined
      (TOOLCHAIN);
  L4  missing or reversed markers fail (ABSENT), and a binary with no helper
      fails (BLIND);
  L5  names that are not helpers are ignored; names are matched without
      leading underscores or a version; main's exit status follows the
      findings over several binaries.
"""

import contextlib
import importlib.util
import io
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
    path = root / 'tools' / 'check_outline_atomics.py'
    spec = importlib.util.spec_from_file_location('check_outline_atomics', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['check_outline_atomics'] = module
    spec.loader.exec_module(module)
    return module


MARKERS = [('stxlib_outline_atomics_begin', 't', 0x1000), ('stxlib_outline_atomics_end', 't', 0x3000)]
GOOD = MARKERS + [('__aarch64_cas4_acq_rel', 't', 0x1010), ('__aarch64_ldadd8_relax', 't', 0x2ff0),
                  ('__aarch64_cas16_acq', 't', 0x1000), ('main', 'T', 0x4000), ('memcpy', 'U', None)]


def kinds(findings: list[str]) -> set[str]:
    result: set[str] = set()
    for finding in findings:
        result.add(finding.split(' ', 1)[0])
    return result


def single_binaries(tool) -> None:
    check(tool.check_binary('good', GOOD) == [], 'L1 helpers inside the markers pass')
    outside = GOOD + [('__aarch64_swp1_acq', 't', 0x3000)]
    check(kinds(tool.check_binary('b', outside)) == {'OUTSIDE'}, 'L2 a helper at the end marker is outside')
    below = GOOD + [('__aarch64_ldclr2_rel', 't', 0x0ff0)]
    check(kinds(tool.check_binary('b', below)) == {'OUTSIDE'}, 'L2 a helper below the begin marker is outside')
    sync = GOOD + [('__aarch64_cas4_sync', 't', 0x9000)]
    check(kinds(tool.check_binary('b', sync)) == {'OUTSIDE'}, 'L2 a toolchain _sync helper is outside')
    undefined = GOOD + [('__aarch64_ldeor4_acq', 'U', None)]
    check(kinds(tool.check_binary('b', undefined)) == {'UNDEFINED'}, 'L2 an undefined helper fails')
    memo = GOOD + [('__aarch64_have_lse_atomics', 'b', 0x9000)]
    check(kinds(tool.check_binary('b', memo)) == {'TOOLCHAIN'}, 'L3 the toolchain memo fails')
    constructor = GOOD + [('init_have_lse_atomics', 't', 0x9100)]
    check(kinds(tool.check_binary('b', constructor)) == {'TOOLCHAIN'}, 'L3 the toolchain constructor fails')
    imported = GOOD + [('__aarch64_have_lse_atomics', 'U', None)]
    check(kinds(tool.check_binary('b', imported)) == {'TOOLCHAIN'}, 'L3 an undefined toolchain memo fails')
    no_markers = [('__aarch64_cas4_acq_rel', 't', 0x1010)]
    check(kinds(tool.check_binary('b', no_markers)) == {'ABSENT', 'OUTSIDE'}, 'L4 no markers: absent, outside')
    reversed_markers = [('stxlib_outline_atomics_begin', 't', 0x3000), ('stxlib_outline_atomics_end', 't', 0x1000),
                        ('__aarch64_cas4_acq_rel', 't', 0x2000)]
    check('ABSENT' in kinds(tool.check_binary('b', reversed_markers)), 'L4 reversed markers are absent')
    check(kinds(tool.check_binary('b', MARKERS)) == {'BLIND'}, 'L4 no helper is blind')
    others = GOOD + [('__aarch64_sme_state', 't', 0x9000), ('aarch64_cas4_acq_rel_extra', 'T', 0x9100)]
    check(tool.check_binary('b', others) == [], 'L5 names that are not helpers are ignored')
    versioned = MARKERS + [('___aarch64_cas8_rel@VERS_1', 't', 0x1100)]
    check(tool.check_binary('b', versioned) == [], 'L5 underscores and versions are stripped')


def whole_runs(tool) -> None:
    tables = {'good': GOOD, 'bad': GOOD + [('__aarch64_have_lse_atomics', 'b', 0x9000)]}

    def reader(nm: str, path: pathlib.Path) -> list:
        return tables[path.name]

    with contextlib.redirect_stdout(io.StringIO()):
        check(tool.main(['--nm', 'nm', 'good'], reader) == 0, 'L5 one good binary passes')
        check(tool.main(['--nm', 'nm', 'good', 'bad'], reader) == 1, 'L5 one bad binary of two fails')


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    tool = load_tool(pathlib.Path(sys.argv[1]))
    single_binaries(tool)
    whole_runs(tool)
    for label in failures:
        print('FAILED: ' + label)
    print(f'check_outline_atomics_tests: {cases - len(failures)} checks passed, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
