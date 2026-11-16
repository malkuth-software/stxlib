#!/usr/bin/env python3
"""Generates STXlib's simple case folding table from Unicode's CaseFolding.txt.

Usage:
  python3 -I tools/gen_case_folding.py <CaseFolding.txt> <output .inc> [<full output .inc>]
  python3 -I tools/gen_case_folding.py --check <CaseFolding.txt> <output .inc> [<full output .inc>]

With the third argument it also writes (or checks) the table of full case
folding, src/algorithms/case_folding_full_table.inc: the F mappings, which
are the code points whose full folding is more than one code point. Checked
first: 105 of them, ascending, none ASCII, none with a C mapping too, each
to two or three scalar values, none of which folds again.

The input is third_party/unicode-18.0.0/CaseFolding.txt, pinned by its
SHA-256 (PROVENANCE.md); a file with another hash is refused, so a new
Unicode version is a deliberate change of the pin here, in PROVENANCE.md
and in include/stxlib/case_folding.hpp's version constant.

Simple case folding is the mappings with status C and S (the file's own
"Usage" A). F (full folding, which can grow a string by characters) and T
(the Turkic dotted and dotless I) are not used. Code points not listed fold
to themselves.

Before writing, the generator checks what include/stxlib/case_folding.hpp
and src/algorithms/case_folding.cpp rely on, and stops on any failure:
  - each listed code point is a scalar value (not a surrogate, at most
    0x10FFFF), appears once among C and S, and the lines are in ascending
    order;
  - each mapping is one scalar value, different from its source;
  - folding is idempotent: no target is itself a source (so
    fold(fold(c)) == fold(c) for every code point);
  - no ASCII code point maps outside ASCII, and only A-Z map at all in ASCII;
  - the UTF-8 length of a target is at most 3/2 of its source's, rounded
    down (so folded text takes at most count + count / 2 bytes);
  - the number of C and S mappings is the count the pin states.

--check regenerates in memory and compares with the output file, exiting 1
on a difference (the CTest check that the committed table is the
generator's).
"""

import hashlib
import pathlib
import sys

PINNED_SHA256 = 'a004797658a457bec4dc11683e39f69249ea3b595b752dbea6721c4c9f587b0d'
PINNED_VERSION = '18.0.0'
PINNED_COMMON = 1501
PINNED_SIMPLE = 32
PINNED_FULL = 105
PER_LINE = 8


def utf8_length(code_point: int) -> int:
    if code_point < 0x80:
        return 1
    if code_point < 0x800:
        return 2
    if code_point < 0x10000:
        return 3
    return 4


def fail(message: str) -> None:
    print(f'gen_case_folding: {message}', file=sys.stderr)
    sys.exit(1)


def parse(data: bytes) -> tuple[list[tuple[int, int]], int, int]:
    """The C and S mappings in file order, and the count of each status."""
    mappings: list[tuple[int, int]] = []
    common = 0
    simple = 0
    for number, raw in enumerate(data.decode('utf-8').splitlines(), start=1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(';')]
        if len(fields) < 3:
            fail(f'line {number}: expected <code>; <status>; <mapping>;')
        status = fields[1]
        if status not in ('C', 'F', 'S', 'T'):
            fail(f'line {number}: unknown status {status!r}')
        if status not in ('C', 'S'):
            continue
        targets = fields[2].split()
        if len(targets) != 1:
            fail(f'line {number}: a {status} mapping must be one code point')
        source = int(fields[0], 16)
        target = int(targets[0], 16)
        mappings.append((source, target))
        if status == 'C':
            common += 1
        else:
            simple += 1
    return mappings, common, simple


def parse_full(data: bytes) -> list[tuple[int, list[int]]]:
    """The F mappings in file order: each source with its folded code points."""
    full: list[tuple[int, list[int]]] = []
    for raw in data.decode('utf-8').splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        fields = [field.strip() for field in line.split(';')]
        if len(fields) >= 3 and fields[1] == 'F':
            full.append((int(fields[0], 16), [int(piece, 16) for piece in fields[2].split()]))
    return full


def check_full(full: list[tuple[int, list[int]]], mappings: list[tuple[int, int]], statuses: dict[int, str]) -> None:
    """What fold_case_full (src/algorithms/case_folding.cpp) relies on."""
    if len(full) != PINNED_FULL:
        fail(f'{len(full)} F mappings; the pin states {PINNED_FULL}')
    sources = [source for source, _ in full]
    if sources != sorted(sources) or len(set(sources)) != len(sources):
        fail('the F lines are not in ascending order, each source once')
    every_source = set(sources) | {source for source, _ in mappings}
    for source, targets in full:
        if source < 0x80:
            fail(f'ASCII U+{source:04X} has a full folding')
        if statuses.get(source) == 'C':
            fail(f'U+{source:04X} has both a C and an F mapping')
        if not 2 <= len(targets) <= 3:
            fail(f'U+{source:04X} folds to {len(targets)} code points; the reader holds 2 or 3')
        for value in [source] + targets:
            if value > 0x10FFFF or 0xD800 <= value <= 0xDFFF:
                fail(f'U+{value:04X} is not a scalar value')
        for target in targets:
            if target in every_source:
                fail(f'U+{source:04X} folds to U+{target:04X}, which folds again')


def render_full(full: list[tuple[int, list[int]]], digest: str) -> str:
    lines: list[str] = []
    lines.append('// Generated by tools/gen_case_folding.py from third_party/unicode-18.0.0/CaseFolding.txt')
    lines.append(f'// (Unicode {PINNED_VERSION}, SHA-256 {digest[:32]}')
    lines.append(f'// {digest[32:]}). Do not edit; regenerate.')
    lines.append('//')
    lines.append(f'// Full case folding where it differs from simple folding: the {len(full)} F mappings,')
    lines.append('// sources ascending, each to two or three scalar values none of which folds')
    lines.append('// again. Unicode License v3 (third_party/unicode-18.0.0/LICENSE).')
    lines.append('')
    lines.append(f'inline constexpr usize case_folding_full_count = {len(full)};')
    lines.append('')
    lines.append('inline constexpr u32 case_folding_full_sources[case_folding_full_count] = {')
    values = [source for source, _ in full]
    for start in range(0, len(values), PER_LINE):
        lines.append('    ' + ' '.join(f'0x{value:05X},' for value in values[start:start + PER_LINE]))
    lines.append('};')
    lines.append('')
    lines.append('// The folded code points of each source (two or three), and how many.')
    lines.append('inline constexpr u8 case_folding_full_lengths[case_folding_full_count] = {')
    lengths = [len(targets) for _, targets in full]
    for start in range(0, len(lengths), 24):
        lines.append('    ' + ' '.join(f'{value},' for value in lengths[start:start + 24]))
    lines.append('};')
    lines.append('')
    lines.append('inline constexpr u32 case_folding_full_targets[case_folding_full_count][3] = {')
    for _, targets in full:
        padded = targets + [0] * (3 - len(targets))
        lines.append('    {' + ', '.join(f'0x{value:05X}' for value in padded) + '},')
    lines.append('};')
    lines.append('')
    return '\n'.join(lines)


def statuses_of(data: bytes) -> dict[int, str]:
    """The status of each code point's C line, for check_full."""
    statuses: dict[int, str] = {}
    for raw in data.decode('utf-8').splitlines():
        line = raw.split('#', 1)[0].strip()
        fields = [field.strip() for field in line.split(';')]
        if len(fields) >= 3 and fields[1] == 'C':
            statuses[int(fields[0], 16)] = 'C'
    return statuses


def check(mappings: list[tuple[int, int]], common: int, simple: int) -> None:
    if common != PINNED_COMMON or simple != PINNED_SIMPLE:
        fail(f'{common} C and {simple} S mappings; the pin states {PINNED_COMMON} and {PINNED_SIMPLE}')
    sources = [source for source, _ in mappings]
    if sources != sorted(sources):
        fail('the C and S lines are not in ascending order')
    if len(set(sources)) != len(sources):
        fail('a code point has more than one C or S mapping')
    source_set = set(sources)
    for source, target in mappings:
        for value in (source, target):
            if value > 0x10FFFF or 0xD800 <= value <= 0xDFFF:
                fail(f'U+{value:04X} is not a scalar value')
        if source == target:
            fail(f'U+{source:04X} maps to itself')
        if target in source_set:
            fail(f'U+{source:04X} maps to U+{target:04X}, which folds again')
        if source < 0x80 and not (0x41 <= source <= 0x5A and target == source + 0x20):
            fail(f'ASCII U+{source:04X} folds other than A-Z to a-z')
        if utf8_length(target) > utf8_length(source) + utf8_length(source) // 2:
            fail(f'U+{source:04X} grows from {utf8_length(source)} to {utf8_length(target)} UTF-8 bytes')
    ascii_sources = [source for source in sources if source < 0x80]
    if ascii_sources != list(range(0x41, 0x5B)):
        fail('the ASCII mappings are not exactly A-Z')


def render(mappings: list[tuple[int, int]], common: int, simple: int, digest: str) -> str:
    lines: list[str] = []
    lines.append('// Generated by tools/gen_case_folding.py from third_party/unicode-18.0.0/CaseFolding.txt')
    lines.append(f'// (Unicode {PINNED_VERSION}, SHA-256 {digest[:32]}')
    lines.append(f'// {digest[32:]}). Do not edit; regenerate.')
    lines.append('//')
    lines.append(f'// Simple case folding: the {common} C and {simple} S mappings, sources ascending,')
    lines.append('// each target a scalar value that is not itself a source. Unicode License v3')
    lines.append('// (third_party/unicode-18.0.0/LICENSE).')
    lines.append('')
    lines.append(f'inline constexpr usize case_folding_count = {len(mappings)};')
    lines.append('')
    for name, column in (('case_folding_sources', 0), ('case_folding_targets', 1)):
        lines.append(f'inline constexpr u32 {name}[case_folding_count] = {{')
        values = [pair[column] for pair in mappings]
        for start in range(0, len(values), PER_LINE):
            chunk = values[start:start + PER_LINE]
            lines.append('    ' + ' '.join(f'0x{value:05X},' for value in chunk))
        lines.append('};')
        lines.append('')
    return '\n'.join(lines)


def main() -> int:
    arguments = sys.argv[1:]
    checking = bool(arguments) and arguments[0] == '--check'
    if checking:
        arguments = arguments[1:]
    if len(arguments) not in (2, 3):
        print('usage: gen_case_folding.py [--check] <CaseFolding.txt> <output .inc> [<full output .inc>]',
              file=sys.stderr)
        return 2
    source_path = pathlib.Path(arguments[0])
    output_path = pathlib.Path(arguments[1])
    full_path = pathlib.Path(arguments[2]) if len(arguments) == 3 else None
    data = source_path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != PINNED_SHA256:
        fail(f'{source_path} has SHA-256 {digest}, not the pinned {PINNED_SHA256}')
    mappings, common, simple = parse(data)
    check(mappings, common, simple)
    text = render(mappings, common, simple, digest)
    outputs = [(output_path, text, len(mappings))]
    if full_path is not None:
        full = parse_full(data)
        check_full(full, mappings, statuses_of(data))
        outputs.append((full_path, render_full(full, digest), len(full)))
    if checking:
        for path, wanted, count in outputs:
            current = path.read_text(encoding='utf-8') if path.exists() else ''
            if current != wanted:
                print(f'gen_case_folding: {path} is not the generator\'s output; regenerate it', file=sys.stderr)
                return 1
            print(f'gen_case_folding: {path} matches ({count} mappings)')
        return 0
    for path, wanted, count in outputs:
        path.write_text(wanted, encoding='utf-8')
        print(f'gen_case_folding: wrote {count} mappings to {path}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
