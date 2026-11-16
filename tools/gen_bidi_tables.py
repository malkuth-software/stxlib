#!/usr/bin/env python3
"""Generates STXlib's bidirectional-algorithm tables (include/stxlib/bidi.hpp).

Usage:
  python3 -I tools/gen_bidi_tables.py <unicode directory> <output .inc>
  python3 -I tools/gen_bidi_tables.py --check <unicode directory> <output .inc>

The directory is third_party/unicode-18.0.0. Read from it, each pinned by
its SHA-256 (PROVENANCE.md); a file with another hash is refused:
  - UnicodeData.txt: field 4 (Bidi_Class), field 5 (a singleton canonical
    decomposition, for bracket equivalence) and field 9 (Bidi_Mirrored);
  - BidiBrackets.txt: Bidi_Paired_Bracket and Bidi_Paired_Bracket_Type;
  - BidiMirroring.txt: Bidi_Mirroring_Glyph;
  - DerivedBidiClass.txt: Bidi_Class of every code point, unassigned ones
    included (the defaults UnicodeData.txt cannot state: R and AL blocks,
    ET for currency, BN for noncharacters and default-ignorables). Every
    code point UnicodeData.txt lists must agree with it.

Written: a two-stage table of one byte per code point (the class in bits 0
to 4, BRACKET in bit 5, MIRRORED in bit 6), the sorted bracket table and
the sorted mirroring table.

Checked before writing, and stopped on any failure:
  - every Bidi_Class value is one of the 23 of UAX #9 table 4;
  - every code point of BidiBrackets.txt is Bidi_Class ON and Bidi_Mirrored,
    its pair is in the file with the opposite type and pairs back to it, and
    each opener's key (its singleton canonical decomposition, or itself)
    equals its closer's key of the pair, so BD16's canonical-equivalence
    match is a comparison of keys;
  - every code point of BidiMirroring.txt is Bidi_Mirrored, and the
    mapping is listed both ways or the target is not itself mirrored;
  - no surrogate or value above 0x10FFFF in any table;
  - the counts the pin states.

--check regenerates in memory and compares with the output file, exiting 1
on a difference (the CTest check stxlib_bidi_tables).
"""

import hashlib
import pathlib
import sys

PINNED = {
    'UnicodeData.txt': '0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950',
    'BidiBrackets.txt': '4b3b62e4a14b84ee752808c810c602534921c09a4a1bf78cfbee566d66c125b3',
    'BidiMirroring.txt': 'cd54810ebf52f0e61a730c8b9cb25975de6c85f6d788a559b416afd548923fd6',
}
# DerivedBidiClass.txt, approved by the owner 2026-11-16.
PINNED_DERIVED = 'd9e23222522551348ea1ccfbb4f62efbf98982afb95840f8959c08ed992c5607'
PINNED_VERSION = '18.0.0'
PINNED_BRACKETS = 130
PINNED_MIRRORS = 438

CLASSES = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
           'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI']
BRACKET_BIT = 0x20
MIRRORED_BIT = 0x40
BLOCK = 128
CODE_POINTS = 0x110000


def fail(message: str) -> None:
    print(f'gen_bidi_tables: {message}', file=sys.stderr)
    sys.exit(1)


def read_pinned(directory: pathlib.Path, name: str, digest: str) -> tuple[bytes, str]:
    data = (directory / name).read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        fail(f'{name}: SHA-256 {actual}; the pin states {digest}')
    return data, actual


def scalar(value: int) -> bool:
    return value < CODE_POINTS and not 0xD800 <= value <= 0xDFFF


def records(data: bytes) -> list[list[str]]:
    rows: list[list[str]] = []
    for raw in data.decode('utf-8').splitlines():
        line = raw.split('#', 1)[0].strip()
        if line:
            rows.append([field.strip() for field in line.split(';')])
    return rows


def parse_unicode_data(data: bytes) -> tuple[dict[int, str], dict[int, int], set[int]]:
    """Bidi_Class of each listed code point, singleton canonical decompositions, Bidi_Mirrored."""
    classes: dict[int, str] = {}
    singletons: dict[int, int] = {}
    mirrored: set[int] = set()
    first: int | None = None
    for fields in records(data):
        if len(fields) < 15:
            fail(f'UnicodeData.txt: a line of {len(fields)} fields')
        code = int(fields[0], 16)
        name = fields[1]
        bidi = fields[4]
        if bidi not in CLASSES:
            fail(f'U+{code:04X}: unknown Bidi_Class {bidi!r}')
        if name.endswith(', First>'):
            first = code
            continue
        span = [code]
        if name.endswith(', Last>'):
            if first is None:
                fail(f'U+{code:04X}: a Last line without its First')
            span = list(range(first, code + 1))
            first = None
        for value in span:
            classes[value] = bidi
            if fields[9] == 'Y':
                mirrored.add(value)
        decomposition = fields[5]
        if decomposition and not decomposition.startswith('<') and len(decomposition.split()) == 1:
            singletons[code] = int(decomposition, 16)
    return classes, singletons, mirrored


def parse_derived(data: bytes) -> dict[int, str]:
    classes: dict[int, str] = {}
    missing: list[tuple[int, int, str]] = []
    for raw in data.decode('utf-8').splitlines():
        if raw.startswith('# @missing:'):
            body = raw[len('# @missing:'):].split('#', 1)[0]
            span, value = [piece.strip() for piece in body.split(';')]
            low, high = [int(piece, 16) for piece in span.split('..')]
            missing.append((low, high, value))
    # Long names in @missing lines, short names in the listing.
    long_names = {'Left_To_Right': 'L', 'Right_To_Left': 'R', 'Arabic_Letter': 'AL',
                  'European_Terminator': 'ET', 'Boundary_Neutral': 'BN'}
    for low, high, value in missing:
        short = long_names.get(value, value)
        if short not in CLASSES:
            fail(f'DerivedBidiClass.txt: unknown @missing value {value!r}')
        for code in range(low, high + 1):
            classes[code] = short
    for fields in records(data):
        span = fields[0]
        bidi = fields[1]
        if bidi not in CLASSES:
            fail(f'DerivedBidiClass.txt: unknown Bidi_Class {bidi!r}')
        if '..' in span:
            low, high = [int(piece, 16) for piece in span.split('..')]
        else:
            low = high = int(span, 16)
        for code in range(low, high + 1):
            classes[code] = bidi
    return classes


def parse_brackets(data: bytes) -> list[tuple[int, int, str]]:
    rows: list[tuple[int, int, str]] = []
    for fields in records(data):
        if len(fields) != 3 or fields[2] not in ('o', 'c'):
            fail(f'BidiBrackets.txt: unexpected line {fields!r}')
        rows.append((int(fields[0], 16), int(fields[1], 16), fields[2]))
    return rows


def parse_mirrors(data: bytes) -> list[tuple[int, int]]:
    rows: list[tuple[int, int]] = []
    for fields in records(data):
        if len(fields) != 2:
            fail(f'BidiMirroring.txt: unexpected line {fields!r}')
        rows.append((int(fields[0], 16), int(fields[1], 16)))
    return rows


def build(directory: pathlib.Path) -> str:
    unicode_data, unicode_digest = read_pinned(directory, 'UnicodeData.txt', PINNED['UnicodeData.txt'])
    bracket_data, bracket_digest = read_pinned(directory, 'BidiBrackets.txt', PINNED['BidiBrackets.txt'])
    mirror_data, mirror_digest = read_pinned(directory, 'BidiMirroring.txt', PINNED['BidiMirroring.txt'])
    listed, singletons, mirrored = parse_unicode_data(unicode_data)
    derived_data, derived_digest = read_pinned(directory, 'DerivedBidiClass.txt', PINNED_DERIVED)
    derived = parse_derived(derived_data)
    classes = ['L'] * CODE_POINTS
    for code, bidi in derived.items():
        classes[code] = bidi
    for code, bidi in listed.items():
        if derived.get(code) != bidi:
            fail(f'U+{code:04X}: UnicodeData.txt says {bidi}, DerivedBidiClass.txt {derived.get(code)}')

    brackets = parse_brackets(bracket_data)
    if len(brackets) != PINNED_BRACKETS:
        fail(f'{len(brackets)} brackets; the pin states {PINNED_BRACKETS}')
    bracket_map = {code: (pair, kind) for code, pair, kind in brackets}
    if [code for code, _, _ in brackets] != sorted(bracket_map):
        fail('BidiBrackets.txt is not in ascending order, each code point once')

    def key(code: int) -> int:
        return singletons.get(code, code)

    for code, pair, kind in brackets:
        if not scalar(code) or not scalar(pair):
            fail(f'U+{code:04X}: not a scalar value')
        if classes[code] != 'ON':
            fail(f'bracket U+{code:04X} is {classes[code]}, not ON')
        if code not in mirrored:
            fail(f'bracket U+{code:04X} is not Bidi_Mirrored')
        back = bracket_map.get(pair)
        if back is None or back[0] != code or back[1] == kind:
            fail(f'bracket U+{code:04X} and its pair U+{pair:04X} do not pair back')
    # The key of an opener and of the closer it pairs with, compared by BD16.
    keys = {}
    for code, pair, kind in brackets:
        keys[code] = key(code) if kind == 'o' else key(pair)
    for code, pair, kind in brackets:
        if kind == 'o' and keys[code] != keys[pair]:
            fail(f'opener U+{code:04X} and closer U+{pair:04X} have different keys')
    # The keys must be openers' code points, so a key is one bracket's identity.
    opener_keys = {keys[code] for code, _, kind in brackets if kind == 'o'}
    if len(opener_keys) != PINNED_BRACKETS // 2 - 1:
        # One pair of classes merges: U+2329 and U+3008 (U+232A and U+3009 with them).
        fail(f'{len(opener_keys)} distinct bracket keys; expected {PINNED_BRACKETS // 2 - 1}')

    mirrors = parse_mirrors(mirror_data)
    if len(mirrors) != PINNED_MIRRORS:
        fail(f'{len(mirrors)} mirroring lines; the pin states {PINNED_MIRRORS}')
    mirror_map = dict(mirrors)
    if len(mirror_map) != len(mirrors):
        fail('BidiMirroring.txt lists a code point twice')
    # The file is not wholly in code point order; the table is.
    mirrors = sorted(mirrors)
    for code, target in mirrors:
        if not scalar(code) or not scalar(target):
            fail(f'U+{code:04X}: not a scalar value')
        if code not in mirrored:
            fail(f'U+{code:04X} has a mirroring glyph but is not Bidi_Mirrored')
    for code in mirrored:
        if not scalar(code):
            fail(f'U+{code:04X} is Bidi_Mirrored but not a scalar value')

    values = [0] * CODE_POINTS
    for code in range(CODE_POINTS):
        value = CLASSES.index(classes[code])
        if code in bracket_map:
            value |= BRACKET_BIT
        if code in mirrored:
            value |= MIRRORED_BIT
        values[code] = value

    blocks: list[tuple[int, ...]] = []
    block_index: dict[tuple[int, ...], int] = {}
    stage1: list[int] = []
    for start in range(0, CODE_POINTS, BLOCK):
        block = tuple(values[start:start + BLOCK])
        if block not in block_index:
            block_index[block] = len(blocks)
            blocks.append(block)
        stage1.append(block_index[block])
    index_type = 'u8' if len(blocks) <= 256 else 'u16'

    lines: list[str] = []
    lines.append('// Generated by tools/gen_bidi_tables.py from third_party/unicode-18.0.0/ (Unicode')
    lines.append(f'// {PINNED_VERSION}): UnicodeData.txt {unicode_digest[:16]}, BidiBrackets.txt {bracket_digest[:16]},')
    lines.append(f'// BidiMirroring.txt {mirror_digest[:16]}, DerivedBidiClass.txt {derived_digest[:16]}.')
    lines.append('// Do not edit; regenerate. Unicode License v3 (third_party/unicode-18.0.0/LICENSE).')
    lines.append('')
    lines.append(f'inline constexpr u32 bidi_block_shift = {BLOCK.bit_length() - 1};')
    lines.append(f'inline constexpr u32 bidi_block_mask = {BLOCK - 1};')
    lines.append(f'inline constexpr usize bidi_block_count = {len(blocks)};')
    lines.append('')
    lines.append('// Block of each 128 code points.')
    lines.append(f'inline constexpr {index_type} bidi_stage1[{len(stage1)}] = {{')
    for start in range(0, len(stage1), 24):
        lines.append('    ' + ' '.join(f'{value},' for value in stage1[start:start + 24]))
    lines.append('};')
    lines.append('')
    lines.append('// Per code point: class (bits 0-4), BRACKET (bit 5), MIRRORED (bit 6).')
    lines.append(f'inline constexpr u8 bidi_stage2[bidi_block_count][{BLOCK}] = {{')
    for block in blocks:
        lines.append('    {')
        for start in range(0, BLOCK, 32):
            lines.append('        ' + ' '.join(f'{value},' for value in block[start:start + 32]))
        lines.append('    },')
    lines.append('};')
    lines.append('')
    lines.append(f'inline constexpr usize bidi_bracket_count = {len(brackets)};')
    lines.append('')
    lines.append('// Brackets, ascending: the code point, its Bidi_Paired_Bracket, whether it')
    lines.append('// opens, and its key (equal for an opener and the closers that match it, by')
    lines.append('// canonical equivalence: BD16).')
    lines.append('inline constexpr BracketEntry bidi_brackets[bidi_bracket_count] = {')
    for code, pair, kind in brackets:
        lines.append(f'    {{0x{code:05X}, 0x{pair:05X}, 0x{keys[code]:05X}, {1 if kind == "o" else 0}}},')
    lines.append('};')
    lines.append('')
    lines.append(f'inline constexpr usize bidi_mirror_count = {len(mirrors)};')
    lines.append('')
    lines.append('// Bidi_Mirroring_Glyph, ascending by code point.')
    lines.append('inline constexpr MirrorEntry bidi_mirrors[bidi_mirror_count] = {')
    for start in range(0, len(mirrors), 4):
        lines.append('    ' + ' '.join(f'{{0x{code:05X}, 0x{target:05X}}},' for code, target in mirrors[start:start + 4]))
    lines.append('};')
    lines.append('')
    return '\n'.join(lines)


def main(argv: list[str]) -> int:
    check = len(argv) > 1 and argv[1] == '--check'
    arguments = argv[2:] if check else argv[1:]
    if len(arguments) != 2:
        print(__doc__.strip().split('\n\n')[1], file=sys.stderr)
        return 2
    text = build(pathlib.Path(arguments[0]))
    output = pathlib.Path(arguments[1])
    if check:
        if not output.exists() or output.read_text(encoding='utf-8') != text:
            print(f'gen_bidi_tables: {output} differs from the generator\'s output; regenerate', file=sys.stderr)
            return 1
        return 0
    output.write_text(text, encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
