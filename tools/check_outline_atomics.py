#!/usr/bin/env python3
"""Checks that linked aarch64 binaries take the outline-atomics helpers from STXlib.

On Linux and FreeBSD aarch64 the compilers turn atomic read-modify-writes
into calls to __aarch64_<operation><size>_<ordering> helpers. STXlib defines
every one of them (src/hosted/elf/outline_atomics_aarch64.cpp), all in one
section between the hidden symbols stxlib_outline_atomics_begin and
stxlib_outline_atomics_end. The toolchain's runtime (libgcc's lse.o and
lse-init.o, compiler-rt's outline-atomics members) defines the same helpers
and the memo __aarch64_have_lse_atomics, set by a constructor that calls
getauxval.

For each linked binary (an executable or a shared library, with its symbol
table) this tool reads every symbol with nm and requires:
  - STXlib's markers are there (the helpers object was linked);
  - every helper the binary defines lies in [begin, end), so it is STXlib's;
  - no helper is left undefined (it would be resolved at run time by
    another module);
  - neither __aarch64_have_lse_atomics nor init_have_lse_atomics appears,
    defined or not (the toolchain's helpers were not pulled in);
  - the binary defines at least one helper, so the scan saw something.

Hidden symbols become local when a binary is linked, and nm still lists
them from the symbol table, so a stripped binary cannot be checked and
fails as ABSENT.

Usage:
  python3 -I tools/check_outline_atomics.py --nm <nm> <binary>...

Exit status 0 when every binary passes, 1 on any finding, 2 when nm fails.
Output lines (one finding a line):
  ABSENT <binary>               STXlib's markers are missing
  OUTSIDE <binary>: <symbol>    a helper defined outside STXlib's range
  UNDEFINED <binary>: <symbol>  a helper left for another module
  TOOLCHAIN <binary>: <symbol>  the toolchain's memo or its constructor
  BLIND <binary>                the binary defines no helper
  SUMMARY <binary>: PASS|FAIL (<helpers> helpers)
"""

import argparse
import pathlib
import re
import subprocess
import sys
from typing import Callable

# Every helper name the compilers emit, with GCC's _sync ordering too, which
# STXlib does not define: a binary that defines one got it from the toolchain.
HELPER = re.compile(r'^aarch64_(cas(1|2|4|8|16)|swp[1248]|ld(add|clr|eor|set)[1248])_(relax|acq|rel|acq_rel|sync)$')
TOOLCHAIN_NAMES = frozenset({'aarch64_have_lse_atomics', 'init_have_lse_atomics'})
BEGIN = 'stxlib_outline_atomics_begin'
END = 'stxlib_outline_atomics_end'
UNDEFINED_TYPES = ('U', 'w', 'v')

# (name, type letter, value); value is None for an undefined symbol.
Symbol = tuple[str, str, int | None]
SymbolReader = Callable[[str, pathlib.Path], list[Symbol]]


def normalised(name: str) -> str:
    """The symbol without leading underscores or ELF version."""
    bare: str = name.split('@', 1)[0].lstrip('_')
    return bare


def symbols_of(nm: str, path: pathlib.Path) -> list[Symbol]:
    """Every symbol of a linked binary, local ones included (nm -P)."""
    completed = subprocess.run([nm, '-P', str(path)], capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f'{path}: {nm} failed: {completed.stderr.strip()}')
    found: list[Symbol] = []
    for line in completed.stdout.splitlines():
        fields: list[str] = line.split()
        if len(fields) < 2 or line.rstrip().endswith(':'):
            continue
        kind: str = fields[1]
        value: int | None = None
        if kind not in UNDEFINED_TYPES and len(fields) >= 3:
            value = int(fields[2], 16)
        found.append((fields[0], kind, value))
    return found


def check_binary(label: str, symbols: list[Symbol]) -> list[str]:
    """The findings for one binary; an empty list passes."""
    findings: list[str] = []
    begin: int | None = None
    end: int | None = None
    for name, kind, value in symbols:
        bare: str = normalised(name)
        if bare == BEGIN and value is not None:
            begin = value
        if bare == END and value is not None:
            end = value
    if begin is None or end is None or end <= begin:
        findings.append(f'ABSENT {label}')
    helpers: int = 0
    for name, kind, value in symbols:
        bare = normalised(name)
        if bare in TOOLCHAIN_NAMES:
            findings.append(f'TOOLCHAIN {label}: {name}')
            continue
        if not HELPER.match(bare):
            continue
        if value is None:
            findings.append(f'UNDEFINED {label}: {name}')
            continue
        helpers = helpers + 1
        inside: bool = begin is not None and end is not None and begin <= value < end
        if not inside:
            findings.append(f'OUTSIDE {label}: {name}')
    if helpers == 0:
        findings.append(f'BLIND {label}')
    return findings


def main(arguments: list[str] | None = None, reader: SymbolReader = symbols_of) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--nm', required=True, help='the toolchain nm (CMAKE_NM)')
    parser.add_argument('binaries', nargs='+', type=pathlib.Path, help='linked executables or shared libraries')
    options = parser.parse_args(arguments)
    failed: bool = False
    for path in options.binaries:
        try:
            symbols: list[Symbol] = reader(options.nm, path)
        except RuntimeError as problem:
            print(f'ERROR {problem}')
            return 2
        findings: list[str] = check_binary(str(path), symbols)
        for finding in findings:
            print(finding)
        helpers: int = 0
        for name, kind, value in symbols:
            if value is not None and HELPER.match(normalised(name)):
                helpers = helpers + 1
        verdict: str = 'FAIL' if findings else 'PASS'
        print(f'SUMMARY {path}: {verdict} ({helpers} helpers)')
        if findings:
            failed = True
    status: int = 1 if failed else 0
    return status


if __name__ == '__main__':
    sys.exit(main())
