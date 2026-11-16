#!/usr/bin/env python3
"""Generates stx_textlayout's tables (include/stxlib/textlayout.hpp).

Usage:
  python3 -I tools/gen_textlayout_tables.py <unicode directory> <output .inc>
  python3 -I tools/gen_textlayout_tables.py --check <unicode directory> <output .inc>
  python3 -I tools/gen_textlayout_tables.py --line-break-test <unicode directory> <LineBreakTest.txt>
  python3 -I tools/gen_textlayout_tables.py --stats <unicode directory> <text file>...

The directory is third_party/unicode-18.0.0. Read from it, each pinned by
its SHA-256 (PROVENANCE.md); a file with another hash is refused:
  - UnicodeData.txt: General_Category (field 2), Bidi_Class (field 4), a
    singleton canonical decomposition (field 5, for bracket equivalence),
    Bidi_Mirrored (field 9);
  - DerivedBidiClass.txt: Bidi_Class of every code point, unassigned ones
    included; every code point UnicodeData.txt lists must agree with it;
  - BidiBrackets.txt, BidiMirroring.txt;
  - LineBreak.txt, EastAsianWidth.txt, emoji-data.txt (Extended_Pictographic);
  - Scripts.txt, ScriptExtensions.txt.

Written, in one file:
  - one two-stage table of one 32-bit record per code point: Bidi_Class
    (bits 0-4), BRACKET (bit 5), MIRRORED (bit 6), the line-break symbol
    (bits 8-14), Script (bits 16-23) and the Script_Extensions set (bits
    24-31, 0 for none);
  - the sorted bracket and mirroring tables;
  - the line-break symbols (resolved class by rule LB1, a subtype, East
    Asian width F, W or H) and the line-break automaton: the rule chain of
    UAX #14 below, explored from the start of text over every symbol and
    minimised, with the look-ahead rules as tables over the next one or two
    symbols;
  - the scripts' ISO 15924 tags and the Script_Extensions sets.

The rule chain here (step) is the generator's reading of UAX #14, rules LB2
to LB31, in the specification's order. The C++ chain
(src/algorithms/textlayout/line_break_chain.hpp) is the same reading; the
test proves the automaton equal to it over every reachable state.

--line-break-test runs the chain and the automaton over LineBreakTest.txt.
--check regenerates in memory and compares with the output file, exiting 1
on a difference (the CTest check stxlib_textlayout_tables).
"""

import hashlib
import pathlib
import sys

PINNED = {
    'UnicodeData.txt': '0736451de439ae7baf1425136617da495e09ee5afbe6e394374db7009ea08950',
    'BidiBrackets.txt': '4b3b62e4a14b84ee752808c810c602534921c09a4a1bf78cfbee566d66c125b3',
    'BidiMirroring.txt': 'cd54810ebf52f0e61a730c8b9cb25975de6c85f6d788a559b416afd548923fd6',
    'DerivedBidiClass.txt': 'd9e23222522551348ea1ccfbb4f62efbf98982afb95840f8959c08ed992c5607',
    'LineBreak.txt': 'ae8cf1970c73f3f1a12d77852df96c4b2b1723ca8b388e36bb5739250c6849de',
    'EastAsianWidth.txt': 'a0cf29eacd00cfcaec4381c6b7c281685f18dbb4e7ff82b4076ccb342ca839aa',
    'emoji-data.txt': '80d00f8e616a0ef27fd6b8de3b758c06383b5d917e2977709578e68baf733bf1',
    'Scripts.txt': '0071fd81b6aeae25f6e8bce8efec3066a6476a91b49bdb2f52dc76e817862a6a',
    'ScriptExtensions.txt': '5c9d34a922f687726f2a8bcf57d49f905987e51f1b21b58c95a00fbe255cec23',
}
PINNED_VERSION = '18.0.0'
PINNED_BRACKETS = 130
PINNED_MIRRORS = 438

BIDI_CLASSES = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
                'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI']
BRACKET_BIT = 0x20
MIRRORED_BIT = 0x40
BLOCK = 128
CODE_POINTS = 0x110000

# Line_Break after rule LB1 (AI, SG and XX are AL; SA is CM when its
# General_Category is Mn or Mc, else AL; CJ is NS), in the order of
# LineBreakClass in the header.
LB = ['BK', 'CR', 'LF', 'NL', 'SP', 'ZW', 'WJ', 'GL', 'CM', 'ZWJ', 'OP', 'CL', 'CP', 'QU', 'NS', 'EX',
      'SY', 'IS', 'PR', 'PO', 'NU', 'AL', 'HL', 'ID', 'IN', 'HY', 'HH', 'BA', 'BB', 'B2', 'CB', 'H2',
      'H3', 'JL', 'JV', 'JT', 'RI', 'EB', 'EM', 'AK', 'AP', 'AS', 'VF', 'VI']
LB_RAW_ONLY = ['AI', 'SG', 'XX', 'SA', 'CJ']
(BK, CR, LF, NL, SP, ZW, WJ, GL, CM, ZWJ, OP, CL, CP, QU, NS, EX, SY, IS, PR, PO, NU, AL, HL, ID, IN,
 HY, HH, BA, BB, B2, CB, H2, H3, JL, JV, JT, RI, EB, EM, AK, AP, AS, VF, VI) = range(len(LB))

# Subtypes of a symbol.
SUB_NONE = 0
SUB_INITIAL = 1    # QU with General_Category Pi
SUB_FINAL = 2      # QU with General_Category Pf
SUB_DOTTED = 3     # U+25CC DOTTED CIRCLE (rule LB28a)
SUB_PICTOGRAPH = 4  # Extended_Pictographic and unassigned (rule LB30b)

NO, ALLOW, MUST = 0, 1, 2
FIRST_LOOK = 3

# The context that survives a run of spaces (rules LB8, LB14 to LB17).
CTX_NONE, CTX_ZW, CTX_OP, CTX_QUI, CTX_CLOSE, CTX_B2 = range(6)

SOT = -1
EOT = -2

# ISO 15924 codes of the Script property's values. PropertyValueAliases.txt
# is not among the pinned files; the codes are written here and checked
# against ScriptExtensions.txt, which uses them (every code there must be
# one of these, and a code point with a real script must list that script's
# code among its extensions). Jurchen's and Seal's codes do not occur in
# ScriptExtensions.txt and are unconfirmed by that check.
SCRIPT_CODES = {
    'Common': 'Zyyy', 'Inherited': 'Zinh', 'Unknown': 'Zzzz',
    'Adlam': 'Adlm', 'Ahom': 'Ahom', 'Anatolian_Hieroglyphs': 'Hluw', 'Arabic': 'Arab', 'Armenian': 'Armn',
    'Avestan': 'Avst', 'Balinese': 'Bali', 'Bamum': 'Bamu', 'Bassa_Vah': 'Bass', 'Batak': 'Batk',
    'Bengali': 'Beng', 'Beria_Erfe': 'Berf', 'Bhaiksuki': 'Bhks', 'Bopomofo': 'Bopo', 'Brahmi': 'Brah',
    'Braille': 'Brai', 'Buginese': 'Bugi', 'Buhid': 'Buhd', 'Canadian_Aboriginal': 'Cans', 'Carian': 'Cari',
    'Caucasian_Albanian': 'Aghb', 'Chakma': 'Cakm', 'Cham': 'Cham', 'Cherokee': 'Cher', 'Chorasmian': 'Chrs',
    'Coptic': 'Copt', 'Cuneiform': 'Xsux', 'Cypriot': 'Cprt', 'Cypro_Minoan': 'Cpmn', 'Cyrillic': 'Cyrl',
    'Deseret': 'Dsrt', 'Devanagari': 'Deva', 'Dives_Akuru': 'Diak', 'Dogra': 'Dogr', 'Duployan': 'Dupl',
    'Egyptian_Hieroglyphs': 'Egyp', 'Elbasan': 'Elba', 'Elymaic': 'Elym', 'Ethiopic': 'Ethi', 'Garay': 'Gara',
    'Georgian': 'Geor', 'Glagolitic': 'Glag', 'Gothic': 'Goth', 'Grantha': 'Gran', 'Greek': 'Grek',
    'Gujarati': 'Gujr', 'Gunjala_Gondi': 'Gong', 'Gurmukhi': 'Guru', 'Gurung_Khema': 'Gukh', 'Han': 'Hani',
    'Hangul': 'Hang', 'Hanifi_Rohingya': 'Rohg', 'Hanunoo': 'Hano', 'Hatran': 'Hatr', 'Hebrew': 'Hebr',
    'Hiragana': 'Hira', 'Imperial_Aramaic': 'Armi', 'Inscriptional_Pahlavi': 'Phli',
    'Inscriptional_Parthian': 'Prti', 'Javanese': 'Java', 'Jurchen': 'Jurc', 'Kaithi': 'Kthi',
    'Kannada': 'Knda', 'Katakana': 'Kana', 'Kawi': 'Kawi', 'Kayah_Li': 'Kali', 'Kharoshthi': 'Khar',
    'Khitan_Small_Script': 'Kits', 'Khmer': 'Khmr', 'Khojki': 'Khoj', 'Khudawadi': 'Sind', 'Kirat_Rai': 'Krai',
    'Lao': 'Laoo', 'Latin': 'Latn', 'Lepcha': 'Lepc', 'Limbu': 'Limb', 'Linear_A': 'Lina', 'Linear_B': 'Linb',
    'Lisu': 'Lisu', 'Lycian': 'Lyci', 'Lydian': 'Lydi', 'Mahajani': 'Mahj', 'Makasar': 'Maka',
    'Malayalam': 'Mlym', 'Mandaic': 'Mand', 'Manichaean': 'Mani', 'Marchen': 'Marc', 'Masaram_Gondi': 'Gonm',
    'Medefaidrin': 'Medf', 'Meetei_Mayek': 'Mtei', 'Mende_Kikakui': 'Mend', 'Meroitic_Cursive': 'Merc',
    'Meroitic_Hieroglyphs': 'Mero', 'Miao': 'Plrd', 'Modi': 'Modi', 'Mongolian': 'Mong', 'Mro': 'Mroo',
    'Multani': 'Mult', 'Myanmar': 'Mymr', 'Nabataean': 'Nbat', 'Nag_Mundari': 'Nagm', 'Nandinagari': 'Nand',
    'New_Tai_Lue': 'Talu', 'Newa': 'Newa', 'Nko': 'Nkoo', 'Nushu': 'Nshu', 'Nyiakeng_Puachue_Hmong': 'Hmnp',
    'Ogham': 'Ogam', 'Ol_Chiki': 'Olck', 'Ol_Onal': 'Onao', 'Old_Hungarian': 'Hung', 'Old_Italic': 'Ital',
    'Old_North_Arabian': 'Narb', 'Old_Permic': 'Perm', 'Old_Persian': 'Xpeo', 'Old_Sogdian': 'Sogo',
    'Old_South_Arabian': 'Sarb', 'Old_Turkic': 'Orkh', 'Old_Uyghur': 'Ougr', 'Oriya': 'Orya', 'Osage': 'Osge',
    'Osmanya': 'Osma', 'Pahawh_Hmong': 'Hmng', 'Palmyrene': 'Palm', 'Pau_Cin_Hau': 'Pauc', 'Phags_Pa': 'Phag',
    'Phoenician': 'Phnx', 'Proto_Cuneiform': 'Pcun', 'Psalter_Pahlavi': 'Phlp', 'Rejang': 'Rjng',
    'Runic': 'Runr', 'Samaritan': 'Samr', 'Saurashtra': 'Saur', 'Seal': 'Seal', 'Sharada': 'Shrd',
    'Shavian': 'Shaw', 'Siddham': 'Sidd', 'Sidetic': 'Sidt', 'SignWriting': 'Sgnw', 'Sinhala': 'Sinh',
    'Sogdian': 'Sogd', 'Sora_Sompeng': 'Sora', 'Soyombo': 'Soyo', 'Sundanese': 'Sund', 'Sunuwar': 'Sunu',
    'Syloti_Nagri': 'Sylo', 'Syriac': 'Syrc', 'Tagalog': 'Tglg', 'Tagbanwa': 'Tagb', 'Tai_Le': 'Tale',
    'Tai_Tham': 'Lana', 'Tai_Viet': 'Tavt', 'Tai_Yo': 'Tayo', 'Takri': 'Takr', 'Tamil': 'Taml',
    'Tangsa': 'Tnsa', 'Tangut': 'Tang', 'Telugu': 'Telu', 'Thaana': 'Thaa', 'Thai': 'Thai', 'Tibetan': 'Tibt',
    'Tifinagh': 'Tfng', 'Tirhuta': 'Tirh', 'Todhri': 'Todr', 'Tolong_Siki': 'Tols', 'Toto': 'Toto',
    'Tulu_Tigalari': 'Tutg', 'Ugaritic': 'Ugar', 'Vai': 'Vaii', 'Vithkuqi': 'Vith', 'Wancho': 'Wcho',
    'Warang_Citi': 'Wara', 'Yezidi': 'Yezi', 'Yi': 'Yiii', 'Zanabazar_Square': 'Zanb',
}


def fail(message: str) -> None:
    print(f'gen_textlayout_tables: {message}', file=sys.stderr)
    sys.exit(1)


def read_pinned(directory: pathlib.Path, name: str) -> tuple[bytes, str]:
    data = (directory / name).read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != PINNED[name]:
        fail(f'{name}: SHA-256 {actual}; the pin states {PINNED[name]}')
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


def span_of(text: str) -> tuple[int, int]:
    if '..' in text:
        low, high = [int(piece, 16) for piece in text.split('..')]
        return low, high
    value = int(text, 16)
    return value, value


def parse_unicode_data(data: bytes):
    """Per listed code point: General_Category, Bidi_Class; singleton decompositions; Bidi_Mirrored."""
    categories: dict[int, str] = {}
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
        if bidi not in BIDI_CLASSES:
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
            categories[value] = fields[2]
            classes[value] = bidi
            if fields[9] == 'Y':
                mirrored.add(value)
        decomposition = fields[5]
        if decomposition and not decomposition.startswith('<') and len(decomposition.split()) == 1:
            singletons[code] = int(decomposition, 16)
    return categories, classes, singletons, mirrored


def parse_property(data: bytes, name: str, default: str | None, aliases: dict[str, str]) -> list[str]:
    """A property of every code point from a UCD listing with @missing lines (applied first, in order)."""
    values: list[str | None] = [default] * CODE_POINTS
    for raw in data.decode('utf-8').splitlines():
        if raw.startswith('# @missing:'):
            body = raw[len('# @missing:'):].split('#', 1)[0]
            pieces = [piece.strip() for piece in body.split(';')]
            low, high = span_of(pieces[0])
            value = aliases.get(pieces[1], pieces[1])
            if value == '<script>':
                continue
            for code in range(low, high + 1):
                values[code] = value
    for fields in records(data):
        low, high = span_of(fields[0])
        for code in range(low, high + 1):
            values[code] = fields[1]
    for code in range(CODE_POINTS):
        if values[code] is None:
            fail(f'{name}: U+{code:04X} has no value')
    return values  # type: ignore[return-value]


# ---------------------------------------------------------------------------
# The line-break rule chain (UAX #14, LB2 to LB31).

class Symbols:
    """The distinct (class, subtype, East Asian) triples of the code points."""

    def __init__(self) -> None:
        self.index: dict[tuple[int, int, bool], int] = {}
        self.cls: list[int] = []
        self.sub: list[int] = []
        self.ea: list[bool] = []

    def add(self, triple: tuple[int, int, bool]) -> int:
        found = self.index.get(triple)
        if found is None:
            found = len(self.cls)
            self.index[triple] = found
            self.cls.append(triple[0])
            self.sub.append(triple[1])
            self.ea.append(triple[2])
        return found

    def count(self) -> int:
        return len(self.cls)


HARD = (BK, CR, LF, NL)
LB15A_BEFORE = (BK, CR, LF, NL, OP, QU, GL, SP, ZW)
LB15B_AFTER = (SP, GL, WJ, CL, QU, CP, EX, IS, SY, BK, CR, LF, NL, ZW)
LB20A_BEFORE = (BK, CR, LF, NL, SP, ZW, CB, GL)

START = (SOT, SOT, False, CTX_NONE, 0, False)


class Chain:
    """step(state, symbol, look) -> (decision before the symbol, next state).

    A state is (w0, p, zwj, ctx, num, ri): the symbol of the last base (a
    character with the combining marks rule LB9 attaches to it; a mark that
    attaches to nothing is AL by LB10), the base before it, whether the last
    code point was ZWJ (LB8a), the context that a run of spaces keeps
    (LB8, LB14 to LB17), the number state of LB25 (1 inside NU (SY|IS)*,
    2 after its CL or CP) and whether w0 ends an odd run of RI (LB30a).
    look(k) is the symbol of the k-th base after the symbol, or EOT.
    """

    def __init__(self, symbols: Symbols) -> None:
        self.symbols = symbols
        self.plain_al = (symbols.add((AL, SUB_NONE, False)), symbols.add((AL, SUB_NONE, True)))

    def aksara(self, symbol: int) -> bool:
        cls = self.symbols.cls[symbol]
        return cls == AK or cls == AS or self.symbols.sub[symbol] == SUB_DOTTED

    def advance(self, state, base: int, zwj: bool):
        cls = self.symbols.cls
        w0, _, _, ctx, num, ri = state
        b = cls[base]
        a = cls[w0] if w0 != SOT else SOT
        if b != SP:
            if b == ZW:
                ctx = CTX_ZW
            elif b == OP:
                ctx = CTX_OP
            elif b == QU and self.symbols.sub[base] == SUB_INITIAL and (a == SOT or a in LB15A_BEFORE):
                ctx = CTX_QUI
            elif b in (CL, CP):
                ctx = CTX_CLOSE
            elif b == B2:
                ctx = CTX_B2
            else:
                ctx = CTX_NONE
        if b == NU:
            new_num = 1
        elif b in (SY, IS):
            new_num = 1 if num == 1 else 0
        elif b in (CL, CP):
            new_num = 2 if num == 1 else 0
        else:
            new_num = 0
        new_ri = b == RI and not (a == RI and ri)
        return (base, w0, zwj, ctx, new_num, new_ri)

    def step(self, state, symbol: int, look):
        cls = self.symbols.cls
        sub = self.symbols.sub
        ea = self.symbols.ea
        w0, p, zwj, ctx, num, ri = state
        c = cls[symbol]
        # LB9: a mark after a base that takes marks joins it. LB10: any
        # other mark is AL.
        base = symbol
        is_mark = c in (CM, ZWJ)
        if is_mark:
            base = self.plain_al[1 if ea[symbol] else 0]
        b = cls[base]
        next_state_as_base = self.advance(state, base, c == ZWJ)
        if w0 == SOT:
            return NO, next_state_as_base                                     # LB2
        a = cls[w0]
        if a == BK:
            return MUST, next_state_as_base                                   # LB4
        if a == CR:
            return (NO if c == LF else MUST), next_state_as_base              # LB5
        if a in (LF, NL):
            return MUST, next_state_as_base                                   # LB5
        if c in HARD:
            return NO, next_state_as_base                                     # LB6
        if c in (SP, ZW):
            return NO, next_state_as_base                                     # LB7
        if ctx == CTX_ZW:
            return ALLOW, next_state_as_base                                  # LB8
        if zwj:
            return NO, (next_state_as_base if not (is_mark and a not in (SP, ZW)) else
                        (w0, p, c == ZWJ, ctx, num, ri))                      # LB8a
        if is_mark and a not in (SP, ZW):
            return NO, (w0, p, c == ZWJ, ctx, num, ri)                        # LB9
        return self.rules(state, base, look), next_state_as_base

    def rules(self, state, base: int, look) -> int:
        cls = self.symbols.cls
        sub = self.symbols.sub
        ea = self.symbols.ea
        w0, p, _, ctx, num, ri = state
        a = cls[w0]
        b = cls[base]
        pc = cls[p] if p != SOT else SOT
        if a == WJ or b == WJ:
            return NO                                                         # LB11
        if a == GL:
            return NO                                                         # LB12
        if b == GL and a not in (SP, HY, HH):
            return NO                                                         # LB12a (18.0.0: BA no longer excepted)
        if b in (CL, CP, EX, SY):
            return NO                                                         # LB13
        if ctx == CTX_OP:
            return NO                                                         # LB14
        if ctx == CTX_QUI:
            return NO                                                         # LB15a
        if b == QU and sub[base] == SUB_FINAL:
            after = look(1)
            if after == EOT or cls[after] in LB15B_AFTER:
                return NO                                                     # LB15b
        if a == SP and b == IS:
            after = look(1)
            if after != EOT and cls[after] == NU:
                return ALLOW                                                  # LB15c
        if b == IS:
            return NO                                                         # LB15d
        if ctx == CTX_CLOSE and b == NS:
            return NO                                                         # LB16
        if ctx == CTX_B2 and b == B2:
            return NO                                                         # LB17
        if a == SP:
            return ALLOW                                                      # LB18
        if b == QU and sub[base] != SUB_INITIAL:
            return NO                                                         # LB19
        if a == QU and sub[w0] != SUB_FINAL:
            return NO                                                         # LB19
        if b == QU:                                                           # LB19a
            if not ea[w0]:
                return NO
            after = look(1)
            if after == EOT or not ea[after]:
                return NO
        if a == QU:                                                           # LB19a
            if not ea[base]:
                return NO
            if p == SOT or not ea[p]:
                return NO
        if a == CB or b == CB:
            return ALLOW                                                      # LB20
        if a in (HY, HH) and (pc == SOT or pc in LB20A_BEFORE) and b in (AL, HL):
            return NO                                                         # LB20a
        if b in (BA, HH, HY, NS) or a == BB:
            return NO                                                         # LB21
        if pc == HL and a in (HY, HH) and b != HL:
            return NO                                                         # LB21a
        if a == SY and b == HL:
            return NO                                                         # LB21b
        if b == IN:
            return NO                                                         # LB22
        if (a in (AL, HL) and b == NU) or (a == NU and b in (AL, HL)):
            return NO                                                         # LB23
        if (a == PR and b in (ID, EB, EM)) or (a in (ID, EB, EM) and b == PO):
            return NO                                                         # LB23a
        if (a in (PR, PO) and b in (AL, HL)) or (a in (AL, HL) and b in (PR, PO)):
            return NO                                                         # LB24
        if num != 0 and b in (PO, PR):
            return NO                                                         # LB25, NU (SY|IS)* (CL|CP)? x (PO|PR)
        if a in (PO, PR) and b == OP:
            after = look(1)
            if after != EOT and cls[after] == NU:
                return NO                                                     # LB25, (PO|PR) x OP NU
            if after != EOT and cls[after] == IS:
                second = look(2)
                if second != EOT and cls[second] == NU:
                    return NO                                                 # LB25, (PO|PR) x OP IS NU
        if a in (PO, PR, HY, IS) and b == NU:
            return NO                                                         # LB25
        if num == 1 and b == NU:
            return NO                                                         # LB25, NU (SY|IS)* x NU
        if a == JL and b in (JL, JV, H2, H3):
            return NO                                                         # LB26
        if a in (JV, H2) and b in (JV, JT):
            return NO                                                         # LB26
        if a in (JT, H3) and b == JT:
            return NO                                                         # LB26
        if a in (JL, JV, JT, H2, H3) and b == PO:
            return NO                                                         # LB27
        if a == PR and b in (JL, JV, JT, H2, H3):
            return NO                                                         # LB27
        if a in (AL, HL) and b in (AL, HL):
            return NO                                                         # LB28
        a_aksara = self.aksara(w0)
        b_aksara = self.aksara(base)
        if a == AP and b_aksara:
            return NO                                                         # LB28a
        if a_aksara and b in (VF, VI):
            return NO                                                         # LB28a
        if p != SOT and self.aksara(p) and a == VI and (b == AK or sub[base] == SUB_DOTTED):
            return NO                                                         # LB28a
        if a_aksara and b_aksara:
            after = look(1)
            if after != EOT and cls[after] == VF:
                return NO                                                     # LB28a
        if a == IS and b in (AL, HL):
            return NO                                                         # LB29
        if a in (AL, HL, NU) and b == OP and not ea[base]:
            return NO                                                         # LB30
        if a == CP and not ea[w0] and b in (AL, HL, NU):
            return NO                                                         # LB30
        if a == RI and ri and b == RI:
            return NO                                                         # LB30a
        if a == EB and b == EM:
            return NO                                                         # LB30b
        if sub[w0] == SUB_PICTOGRAPH and b == EM:
            return NO                                                         # LB30b
        return ALLOW                                                          # LB31


class Peeked(Exception):
    pass


def absorbed(symbols: Symbols, symbol: int) -> bool:
    return symbols.cls[symbol] in (CM, ZWJ)


def build_automaton(symbols: Symbols, chain: Chain):
    """Explores the chain from START over every symbol, then minimises."""
    count = symbols.count()
    bases = [symbol for symbol in range(count) if not absorbed(symbols, symbol)] + [EOT]
    look_rows: dict[tuple, int] = {}
    second_rows: dict[tuple, int] = {}
    first_table: list[list[int]] = []
    second_table: list[list[int]] = []

    def column(symbol: int) -> int:
        return count if symbol == EOT else symbol

    def decide(state, symbol: int) -> tuple[int, tuple]:
        def refuse(_: int) -> int:
            raise Peeked()
        try:
            return chain.step(state, symbol, refuse)
        except Peeked:
            pass
        next_state = None
        first = [0] * (count + 1)
        for one in bases:
            def look_one(k: int, one=one) -> int:
                if k == 1:
                    return one
                raise Peeked()
            try:
                decision, next_state = chain.step(state, symbol, look_one)
                first[column(one)] = decision
                continue
            except Peeked:
                pass
            second = [0] * (count + 1)
            for two in bases:
                def look_two(k: int, one=one, two=two) -> int:
                    return one if k == 1 else two
                decision, next_state = chain.step(state, symbol, look_two)
                second[column(two)] = decision
            key = tuple(second)
            if key not in second_rows:
                second_rows[key] = len(second_table)
                second_table.append(second)
            first[column(one)] = 2 + second_rows[key]
        key = tuple(first)
        if key not in look_rows:
            look_rows[key] = len(first_table)
            first_table.append(first)
        return FIRST_LOOK + look_rows[key], next_state

    index = {START: 0}
    order = [START]
    actions: list[list[int]] = []
    targets: list[list[int]] = []
    position = 0
    while position < len(order):
        state = order[position]
        position += 1
        row_actions = []
        row_targets = []
        for symbol in range(count):
            action, next_state = decide(state, symbol)
            found = index.get(next_state)
            if found is None:
                found = len(order)
                index[next_state] = found
                order.append(next_state)
            row_actions.append(action)
            row_targets.append(found)
        actions.append(row_actions)
        targets.append(row_targets)

    # Moore's partition refinement: states with equal actions, then equal
    # blocks of their targets.
    total = len(order)
    block = [0] * total
    keys: dict[tuple, int] = {}
    for state in range(total):
        block[state] = keys.setdefault(tuple(actions[state]), len(keys))
    while True:
        keys = {}
        refined = [0] * total
        for state in range(total):
            key = (block[state], tuple(block[target] for target in targets[state]))
            refined[state] = keys.setdefault(key, len(keys))
        stable = len(keys) == len(set(block))
        block = refined
        if stable:
            break
    states = len(set(block))
    # Number the blocks in order of first reach, so the start is state 0.
    renumber: dict[int, int] = {}
    for state in range(total):
        renumber.setdefault(block[state], len(renumber))
    table = [[(0, 0)] * count for _ in range(states)]
    for state in range(total):
        row = renumber[block[state]]
        for symbol in range(count):
            table[row][symbol] = (actions[state][symbol], renumber[block[targets[state][symbol]]])
    return table, first_table, second_table, total


# ---------------------------------------------------------------------------

class Data:
    pass


def load(directory: pathlib.Path) -> Data:
    data = Data()
    digests: dict[str, str] = {}
    raw: dict[str, bytes] = {}
    for name in PINNED:
        raw[name], digests[name] = read_pinned(directory, name)
    data.digests = digests
    categories, listed, singletons, mirrored = parse_unicode_data(raw['UnicodeData.txt'])
    data.categories = categories

    # Bidi_Class.
    long_names = {'Left_To_Right': 'L', 'Right_To_Left': 'R', 'Arabic_Letter': 'AL',
                  'European_Terminator': 'ET', 'Boundary_Neutral': 'BN'}
    bidi = parse_property(raw['DerivedBidiClass.txt'], 'DerivedBidiClass.txt', 'L', long_names)
    for code in range(CODE_POINTS):
        if bidi[code] not in BIDI_CLASSES:
            fail(f'DerivedBidiClass.txt: unknown Bidi_Class {bidi[code]!r}')
    for code, value in listed.items():
        if bidi[code] != value:
            fail(f'U+{code:04X}: UnicodeData.txt says {value}, DerivedBidiClass.txt {bidi[code]}')
    data.bidi = bidi

    # Brackets.
    brackets: list[tuple[int, int, str]] = []
    for fields in records(raw['BidiBrackets.txt']):
        if len(fields) != 3 or fields[2] not in ('o', 'c'):
            fail(f'BidiBrackets.txt: unexpected line {fields!r}')
        brackets.append((int(fields[0], 16), int(fields[1], 16), fields[2]))
    if len(brackets) != PINNED_BRACKETS:
        fail(f'{len(brackets)} brackets; the pin states {PINNED_BRACKETS}')
    bracket_map = {code: (pair, kind) for code, pair, kind in brackets}
    if [code for code, _, _ in brackets] != sorted(bracket_map):
        fail('BidiBrackets.txt is not in ascending order, each code point once')
    for code, pair, kind in brackets:
        if not scalar(code) or not scalar(pair):
            fail(f'U+{code:04X}: not a scalar value')
        if bidi[code] != 'ON':
            fail(f'bracket U+{code:04X} is {bidi[code]}, not ON')
        if code not in mirrored:
            fail(f'bracket U+{code:04X} is not Bidi_Mirrored')
        back = bracket_map.get(pair)
        if back is None or back[0] != code or back[1] == kind:
            fail(f'bracket U+{code:04X} and its pair U+{pair:04X} do not pair back')
    keys = {}
    for code, pair, kind in brackets:
        opener = code if kind == 'o' else pair
        keys[code] = singletons.get(opener, opener)
    for code, pair, kind in brackets:
        if kind == 'o' and keys[code] != keys[pair]:
            fail(f'opener U+{code:04X} and closer U+{pair:04X} have different keys')
    opener_keys = {keys[code] for code, _, kind in brackets if kind == 'o'}
    if len(opener_keys) != PINNED_BRACKETS // 2 - 1:
        # One pair of classes merges: U+2329 and U+3008 (U+232A and U+3009 with them).
        fail(f'{len(opener_keys)} distinct bracket keys; expected {PINNED_BRACKETS // 2 - 1}')
    data.brackets = brackets
    data.bracket_map = bracket_map
    data.bracket_keys = keys

    # Mirroring.
    mirrors: list[tuple[int, int]] = []
    for fields in records(raw['BidiMirroring.txt']):
        if len(fields) != 2:
            fail(f'BidiMirroring.txt: unexpected line {fields!r}')
        mirrors.append((int(fields[0], 16), int(fields[1], 16)))
    if len(mirrors) != PINNED_MIRRORS:
        fail(f'{len(mirrors)} mirroring lines; the pin states {PINNED_MIRRORS}')
    if len(dict(mirrors)) != len(mirrors):
        fail('BidiMirroring.txt lists a code point twice')
    mirrors = sorted(mirrors)
    for code, target in mirrors:
        if not scalar(code) or not scalar(target):
            fail(f'U+{code:04X}: not a scalar value')
        if code not in mirrored:
            fail(f'U+{code:04X} has a mirroring glyph but is not Bidi_Mirrored')
    for code in mirrored:
        if not scalar(code):
            fail(f'U+{code:04X} is Bidi_Mirrored but not a scalar value')
    data.mirrors = mirrors
    data.mirrored = mirrored

    # Line_Break, East_Asian_Width, Extended_Pictographic.
    line_break = parse_property(raw['LineBreak.txt'], 'LineBreak.txt', 'XX', {})
    east_asian = parse_property(raw['EastAsianWidth.txt'], 'EastAsianWidth.txt', 'N', {'Neutral': 'N', 'Wide': 'W'})
    pictographic: set[int] = set()
    for fields in records(raw['emoji-data.txt']):
        if fields[1] == 'Extended_Pictographic':
            low, high = span_of(fields[0])
            pictographic.update(range(low, high + 1))
    if not pictographic:
        fail('emoji-data.txt: no Extended_Pictographic line')
    for code, expected in ((0x2FFFD, 'ID'), (0x1FAFF, 'ID'), (0x3FFFD, 'ID'), (0x20CF, 'PR'), (0x0378, 'XX')):
        if line_break[code] != expected:
            fail(f'LineBreak.txt: U+{code:04X} is {line_break[code]}, expected the default {expected}')
    symbols = Symbols()
    chain = Chain(symbols)
    symbol_of = [0] * CODE_POINTS
    for code in range(CODE_POINTS):
        raw_class = line_break[code]
        category = categories.get(code, 'Cn')
        if raw_class in ('AI', 'SG', 'XX'):
            resolved = AL
        elif raw_class == 'SA':
            resolved = CM if category in ('Mn', 'Mc') else AL
        elif raw_class == 'CJ':
            resolved = NS
        elif raw_class in LB:
            resolved = LB.index(raw_class)
        else:
            fail(f'U+{code:04X}: unknown Line_Break {raw_class!r}')
        sub = SUB_NONE
        if resolved == QU and category == 'Pi':
            sub = SUB_INITIAL
        elif resolved == QU and category == 'Pf':
            sub = SUB_FINAL
        elif code == 0x25CC:
            sub = SUB_DOTTED
        elif code in pictographic and category == 'Cn':
            sub = SUB_PICTOGRAPH
        if east_asian[code] not in ('N', 'A', 'Na', 'H', 'W', 'F'):
            fail(f'U+{code:04X}: unknown East_Asian_Width {east_asian[code]!r}')
        wide = east_asian[code] in ('F', 'W', 'H')
        symbol_of[code] = symbols.add((resolved, sub, wide))
    if line_break[0x25CC] != 'AL':
        fail('U+25CC is not Line_Break AL')
    if symbols.count() > 127:
        fail(f'{symbols.count()} line-break symbols; the record holds 127')
    data.line_break_raw = line_break
    data.east_asian = east_asian
    data.pictographic = pictographic
    data.symbols = symbols
    data.chain = chain
    data.symbol_of = symbol_of

    # Script and Script_Extensions.
    script_names = parse_property(raw['Scripts.txt'], 'Scripts.txt', 'Unknown', {})
    used = sorted(set(script_names))
    for name in used:
        if name not in SCRIPT_CODES:
            fail(f'Scripts.txt: script {name!r} has no ISO 15924 code in SCRIPT_CODES')
    codes = ['Zyyy', 'Zinh', 'Zzzz'] + sorted(SCRIPT_CODES[name] for name in used
                                               if name not in ('Common', 'Inherited', 'Unknown'))
    if len(codes) != len(set(codes)) or len(codes) > 255:
        fail('script codes are not distinct or exceed 255')
    code_index = {code: position for position, code in enumerate(codes)}
    script_of = [code_index[SCRIPT_CODES[name]] for name in script_names]
    sets: dict[tuple[int, ...], int] = {}
    set_list: list[tuple[int, ...]] = []
    extension_of = [0] * CODE_POINTS
    for fields in records(raw['ScriptExtensions.txt']):
        low, high = span_of(fields[0])
        members = []
        for code in fields[1].split():
            if code not in code_index:
                fail(f'ScriptExtensions.txt: code {code!r} is not a known script')
            members.append(code_index[code])
        key = tuple(sorted(members))
        for code in range(low, high + 1):
            own = script_of[code]
            if own > 2 and own not in key:
                fail(f'U+{code:04X}: its script {codes[own]} is not among its extensions')
            if key == (own,):
                continue
            if key not in sets:
                sets[key] = len(set_list) + 1
                set_list.append(key)
            extension_of[code] = sets[key]
    if len(set_list) > 255:
        fail(f'{len(set_list)} Script_Extensions sets; the record holds 255')
    data.script_codes = codes
    data.script_of = script_of
    data.extension_sets = set_list
    data.extension_of = extension_of
    return data


def record_of(data: Data, code: int) -> int:
    value = BIDI_CLASSES.index(data.bidi[code])
    if code in data.bracket_map:
        value |= BRACKET_BIT
    if code in data.mirrored:
        value |= MIRRORED_BIT
    value |= data.symbol_of[code] << 8
    value |= data.script_of[code] << 16
    value |= data.extension_of[code] << 24
    return value


def rows_of(values: list[int], per_line: int, form: str) -> list[str]:
    return ['    ' + ' '.join(form.format(value) + ',' for value in values[start:start + per_line])
            for start in range(0, len(values), per_line)]


def build(directory: pathlib.Path) -> str:
    data = load(directory)
    symbols = data.symbols
    table, first_table, second_table, explored = build_automaton(symbols, data.chain)
    count = symbols.count()
    states = len(table)
    shift = max(1, (count - 1).bit_length())
    if states > 4095:
        fail(f'{states} automaton states; an entry holds 4095')
    if FIRST_LOOK + len(first_table) > 15:
        fail(f'{len(first_table)} look-ahead rows; an entry holds 12')
    if 2 + len(second_table) > 255:
        fail(f'{len(second_table)} second look-ahead rows')

    # The state inside a word of letters: from the start, one plain AL;
    # another AL must stay there with no break.
    letter = data.symbol_of[ord('a')]
    for other in list(range(ord('a'), ord('z') + 1)) + list(range(ord('A'), ord('Z') + 1)):
        if data.symbol_of[other] != letter:
            fail('the ASCII letters are not one line-break symbol')
    letter_state = table[0][letter][1]
    if table[letter_state][letter] != (NO, letter_state):
        fail('a letter after a letter does not stay in the letter state with no break')
    for row in table:
        if row[letter][1] != letter_state:
            fail('a letter does not lead to the letter state from every state')
    latin = data.script_codes.index('Latn')
    for other in list(range(ord('a'), ord('z') + 1)) + list(range(ord('A'), ord('Z') + 1)):
        if data.script_of[other] != latin or data.bidi[other] != 'L' or data.extension_of[other] != 0:
            fail('an ASCII letter is not Latin, left to right and without Script_Extensions')

    values = [record_of(data, code) for code in range(CODE_POINTS)]
    blocks: list[tuple[int, ...]] = []
    block_index: dict[tuple[int, ...], int] = {}
    stage1: list[int] = []
    for start in range(0, CODE_POINTS, BLOCK):
        block = tuple(values[start:start + BLOCK])
        if block not in block_index:
            block_index[block] = len(blocks)
            blocks.append(block)
        stage1.append(block_index[block])
    if len(blocks) > 65535:
        fail('too many blocks')

    d = data.digests
    lines: list[str] = []
    lines.append('// Generated by tools/gen_textlayout_tables.py from third_party/unicode-18.0.0/ (Unicode')
    lines.append(f'// {PINNED_VERSION}). SHA-256, first 16 digits:')
    for name in PINNED:
        lines.append(f'//   {name} {d[name][:16]}')
    lines.append('// Do not edit; regenerate. Unicode License v3 (third_party/unicode-18.0.0/LICENSE).')
    lines.append('')
    lines.append(f'inline constexpr u32 property_block_shift = {BLOCK.bit_length() - 1};')
    lines.append(f'inline constexpr u32 property_block_mask = {BLOCK - 1};')
    lines.append(f'inline constexpr usize property_block_count = {len(blocks)};')
    lines.append('')
    lines.append('// Block of each 128 code points.')
    lines.append(f'inline constexpr u16 property_stage1[{len(stage1)}] = {{')
    lines.extend(rows_of(stage1, 24, '{}'))
    lines.append('};')
    lines.append('')
    lines.append('// Per code point: Bidi_Class (bits 0-4), BRACKET (bit 5), MIRRORED (bit 6),')
    lines.append('// line-break symbol (bits 8-14), Script (bits 16-23), Script_Extensions set')
    lines.append('// (bits 24-31, 0 for none).')
    lines.append(f'inline constexpr u32 property_stage2[property_block_count][{BLOCK}] = {{')
    for block in blocks:
        lines.append('    {')
        lines.extend('    ' + row for row in rows_of(list(block), 12, '0x{:08X}'))
        lines.append('    },')
    lines.append('};')
    lines.append('')
    lines.append(f'inline constexpr usize bidi_bracket_count = {len(data.brackets)};')
    lines.append('')
    lines.append('// Brackets, ascending: the code point, its Bidi_Paired_Bracket, its key (equal')
    lines.append('// for an opener and the closers that match it, by canonical equivalence:')
    lines.append('// BD16) and whether it opens.')
    lines.append('inline constexpr BracketEntry bidi_brackets[bidi_bracket_count] = {')
    for code, pair, kind in data.brackets:
        lines.append(f'    {{0x{code:05X}, 0x{pair:05X}, 0x{data.bracket_keys[code]:05X}, {1 if kind == "o" else 0}}},')
    lines.append('};')
    lines.append('')
    lines.append(f'inline constexpr usize bidi_mirror_count = {len(data.mirrors)};')
    lines.append('')
    lines.append('// Bidi_Mirroring_Glyph, ascending by code point.')
    lines.append('inline constexpr MirrorEntry bidi_mirrors[bidi_mirror_count] = {')
    for start in range(0, len(data.mirrors), 4):
        lines.append('    ' + ' '.join(f'{{0x{code:05X}, 0x{target:05X}}},'
                                       for code, target in data.mirrors[start:start + 4]))
    lines.append('};')
    lines.append('')
    lines.append('// Line-break symbols: resolved class (LineBreakClass order), subtype (1 QU')
    lines.append('// with Pi, 2 QU with Pf, 3 U+25CC, 4 Extended_Pictographic and unassigned),')
    lines.append('// East Asian width F, W or H.')
    lines.append(f'inline constexpr u32 lb_symbol_count = {count};')
    lines.append('//   ' + ' '.join(f'{index}:{LB[symbols.cls[index]]}'
                                    + ('' if symbols.sub[index] == 0 else f'/{symbols.sub[index]}')
                                    + ('+ea' if symbols.ea[index] else '') for index in range(count)))
    lines.append('inline constexpr u8 lb_symbol_class[lb_symbol_count] = {')
    lines.extend(rows_of(symbols.cls, 24, '{}'))
    lines.append('};')
    lines.append('inline constexpr u8 lb_symbol_subtype[lb_symbol_count] = {')
    lines.extend(rows_of(symbols.sub, 24, '{}'))
    lines.append('};')
    lines.append('inline constexpr u8 lb_symbol_east_asian[lb_symbol_count] = {')
    lines.extend(rows_of([1 if wide else 0 for wide in symbols.ea], 24, '{}'))
    lines.append('};')
    lines.append('// 1 for a symbol of class CM or ZWJ (a look-ahead passes over it).')
    lines.append('inline constexpr u8 lb_symbol_mark[lb_symbol_count] = {')
    lines.extend(rows_of([1 if absorbed(symbols, index) else 0 for index in range(count)], 24, '{}'))
    lines.append('};')
    lines.append('// The symbol of plain AL, not East Asian and East Asian (rule LB10).')
    lines.append(f'inline constexpr u8 lb_symbol_plain_al[2] = {{{data.chain.plain_al[0]}, {data.chain.plain_al[1]}}};')
    lines.append('')
    lines.append(f'// The automaton: {states} states (minimised from {explored} reachable chain states),')
    lines.append('// state 0 the start of text. Entry [state << lb_dfa_shift | symbol]: the next')
    lines.append('// state in bits 0-11; in bits 12-15 the decision before the symbol (0 no')
    lines.append('// break, 1 break allowed, 2 mandatory break) or 3 + a row of lb_look_first.')
    lines.append(f'inline constexpr u32 lb_dfa_shift = {shift};')
    lines.append(f'inline constexpr u32 lb_dfa_state_count = {states};')
    lines.append(f'inline constexpr u32 lb_dfa_letter_state = {letter_state};')
    lines.append(f'inline constexpr u32 lb_dfa_letter_symbol = {letter};')
    lines.append(f'inline constexpr u16 lb_dfa[lb_dfa_state_count << lb_dfa_shift] = {{')
    for row in table:
        entries = [(action << 12) | target for action, target in row] + [0] * ((1 << shift) - count)
        lines.extend(rows_of(entries, 16, '0x{:04X}'))
    lines.append('};')
    lines.append('')
    lines.append('// Look-ahead, by the symbol of the next base (column lb_symbol_count: the end')
    lines.append('// of text): 0 no break, 1 break allowed, or 2 + a row of lb_look_second, read')
    lines.append('// by the base after that.')
    lines.append(f'inline constexpr u32 lb_look_first_count = {len(first_table)};')
    lines.append('inline constexpr u8 lb_look_first[lb_look_first_count][lb_symbol_count + 1] = {')
    for row in first_table:
        lines.append('    {' + ' '.join(f'{value},' for value in row) + '},')
    lines.append('};')
    lines.append(f'inline constexpr u32 lb_look_second_count = {max(1, len(second_table))};')
    lines.append('inline constexpr u8 lb_look_second[lb_look_second_count][lb_symbol_count + 1] = {')
    for row in (second_table or [[0] * (count + 1)]):
        lines.append('    {' + ' '.join(f'{value},' for value in row) + '},')
    lines.append('};')
    lines.append('')
    lines.append('// Scripts: 0 Common, 1 Inherited, 2 Unknown, then by ISO 15924 code. The tag')
    lines.append('// is the code\'s four letters, the first in the highest byte.')
    lines.append(f'inline constexpr u32 script_count = {len(data.script_codes)};')
    lines.append(f'inline constexpr u8 script_latin = {data.script_codes.index("Latn")};')
    lines.append('inline constexpr u32 script_tags[script_count] = {')
    tags = [int.from_bytes(code.encode('ascii'), 'big') for code in data.script_codes]
    for start in range(0, len(tags), 6):
        lines.append('    ' + ' '.join(f'0x{tag:08X},' for tag in tags[start:start + 6])
                     + '  // ' + ' '.join(data.script_codes[start:start + 6]))
    lines.append('};')
    lines.append('')
    lines.append('// Script_Extensions sets: set s (1-based, as in the record) is')
    lines.append('// script_extension_members[script_extension_begin[s - 1], script_extension_begin[s]).')
    begins = [0]
    members: list[int] = []
    for key in data.extension_sets:
        members.extend(key)
        begins.append(len(members))
    lines.append(f'inline constexpr u32 script_extension_set_count = {len(data.extension_sets)};')
    lines.append('inline constexpr u16 script_extension_begin[script_extension_set_count + 1] = {')
    lines.extend(rows_of(begins, 20, '{}'))
    lines.append('};')
    lines.append(f'inline constexpr u8 script_extension_members[{len(members)}] = {{')
    lines.extend(rows_of(members, 24, '{}'))
    lines.append('};')
    lines.append('')
    return '\n'.join(lines)


# ---------------------------------------------------------------------------
# LineBreakTest.txt through the chain and through the automaton.

def chain_breaks(data: Data, points: list[int]) -> list[int]:
    symbols = data.symbols
    stream = [data.symbol_of[point] for point in points]
    state = START
    result = []
    for position, symbol in enumerate(stream):
        def look(k: int, position=position) -> int:
            index = position
            for _ in range(k):
                index += 1
                while index < len(stream) and absorbed(symbols, stream[index]):
                    index += 1
                if index >= len(stream):
                    return EOT
            return stream[index]
        decision, state = data.chain.step(state, symbol, look)
        if position > 0:
            result.append(decision)
    result.append(MUST)  # LB3
    return result


def automaton_breaks(data: Data, automaton, points: list[int]) -> list[int]:
    table, first_table, second_table, _ = automaton
    symbols = data.symbols
    count = symbols.count()
    stream = [data.symbol_of[point] for point in points]

    def base_after(index: int) -> tuple[int, int]:
        index += 1
        while index < len(stream) and absorbed(symbols, stream[index]):
            index += 1
        if index >= len(stream):
            return count, index
        return stream[index], index

    state = 0
    result = []
    for position, symbol in enumerate(stream):
        action, state = table[state][symbol]
        if action >= FIRST_LOOK:
            one, index = base_after(position)
            action = first_table[action - FIRST_LOOK][one]
            if action >= 2:
                two, _ = base_after(index)
                action = second_table[action - 2][two]
        if position > 0:
            result.append(action)
    result.append(MUST)
    return result


def run_line_break_test(directory: pathlib.Path, test_file: pathlib.Path) -> int:
    data = load(directory)
    automaton = build_automaton(data.symbols, data.chain)
    print(f'{data.symbols.count()} symbols, {len(automaton[0])} states from {automaton[3]} chain states, '
          f'{len(automaton[1])} + {len(automaton[2])} look-ahead rows')
    cases = 0
    failed = 0
    automaton_differs = 0
    for number, raw in enumerate(test_file.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        tokens = line.split()
        points = [int(token, 16) for token in tokens[1::2]]
        expected = [token == '÷' for token in tokens[2::2]]
        cases += 1
        got = chain_breaks(data, points)
        if [value != NO for value in got] != expected:
            failed += 1
            if failed <= 40:
                print(f'line {number}: {raw}')
                print('   chain: ' + ' '.join('÷' if value != NO else '×' for value in got))
        if automaton_breaks(data, automaton, points) != got:
            automaton_differs += 1
    print(f'LineBreakTest: {cases - failed} of {cases} cases pass the chain; '
          f'the automaton differs from the chain on {automaton_differs}')
    return 0 if failed == 0 and automaton_differs == 0 else 1


def run_stats(directory: pathlib.Path, files: list[str]) -> int:
    data = load(directory)
    automaton = build_automaton(data.symbols, data.chain)
    table = automaton[0]
    symbols = data.symbols
    classes: dict[str, int] = {}
    pairs: dict[str, int] = {}
    actions = [0, 0, 0, 0]
    total = 0
    ascii_letters_in_letter_state = 0
    letter_state = table[0][data.symbol_of[ord('a')]][1]
    for name in files:
        text = pathlib.Path(name).read_text(encoding='utf-8', errors='replace')
        state = 0
        previous = 'sot'
        for character in text:
            symbol = data.symbol_of[ord(character)]
            label = LB[symbols.cls[symbol]]
            classes[label] = classes.get(label, 0) + 1
            pairs[previous + ' ' + label] = pairs.get(previous + ' ' + label, 0) + 1
            if state == letter_state and character.isascii() and character.isalpha():
                ascii_letters_in_letter_state += 1
            action, state = table[state][symbol]
            actions[min(action, 3)] += 1
            previous = label
            total += 1
    print(f'{total} code points')
    print('classes: ' + ', '.join(f'{label} {100 * value / total:.2f}%'
                                  for label, value in sorted(classes.items(), key=lambda item: -item[1])[:14]))
    print('pairs: ' + ', '.join(f'{label} {100 * value / total:.2f}%'
                                for label, value in sorted(pairs.items(), key=lambda item: -item[1])[:14]))
    print(f'decisions: no break {100 * actions[0] / total:.2f}%, allowed {100 * actions[1] / total:.2f}%, '
          f'mandatory {100 * actions[2] / total:.2f}%, by look-ahead {100 * actions[3] / total:.3f}%')
    print(f'ASCII letters met in the letter state: {100 * ascii_letters_in_letter_state / total:.2f}%')
    return 0


def main(argv: list[str]) -> int:
    if len(argv) >= 4 and argv[1] == '--line-break-test':
        return run_line_break_test(pathlib.Path(argv[2]), pathlib.Path(argv[3]))
    if len(argv) >= 4 and argv[1] == '--stats':
        return run_stats(pathlib.Path(argv[2]), argv[3:])
    check = len(argv) > 1 and argv[1] == '--check'
    arguments = argv[2:] if check else argv[1:]
    if len(arguments) != 2:
        print(__doc__.strip().split('\n\n')[1], file=sys.stderr)
        return 2
    text = build(pathlib.Path(arguments[0]))
    output = pathlib.Path(arguments[1])
    if check:
        if not output.exists() or output.read_text(encoding='utf-8') != text:
            print(f'gen_textlayout_tables: {output} differs from the generator\'s output; regenerate',
                  file=sys.stderr)
            return 1
        return 0
    output.write_text(text, encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
