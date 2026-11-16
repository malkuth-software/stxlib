#!/usr/bin/env python3
"""Writes src/algorithms/glyphs/hinter_adjust.inc from FreeType's table of accented characters.

The inputs are src/autofit/afadjust.c and src/autofit/afblue.dat of FreeType 2.14.2 as released
(PROVENANCE.md: stxlib::glyphs, the hinter) and this tree's hinter_tables.inc, whose blue zones
the second table is in the order of (a blue zone of a script FreeType 2.14.2 has not is neither
kind of bottom). FreeType's files are not in this tree: the tables are made
again only when a later FreeType is taken, by running this with the files' paths.

    python3 -I tools/gen_hinter_adjust.py <afadjust.c> <afblue.dat> <hinter_tables.inc> <hinter_adjust.inc>

Two things are taken as FreeType has them and not as they were meant, so that a glyph gets what
FreeType gives it:
- afadjust.c's table is searched by halving although one of its entries is out of order. Each
  character is given here what that search finds for it, which for a character the search
  misses is nothing.
- When two characters of the table have one glyph, the later one in the table's order decides:
  the entries are kept in that order.
"""
import hashlib
import re
import sys

PINNED_ADJUST = "4d16e3aa397e7932294e57d0512d9afb2a1326340ff3936e13584dadeb5ce393"
PINNED_BLUE = "3704627872df9e0a3be5eed72474ad8436992d665248082b34460eb59828b132"

FLAGS = {
    "AF_ADJUST_NONE": 0x00, "AF_ADJUST_UP": 0x01, "AF_ADJUST_DOWN": 0x02, "AF_ADJUST_UP2": 0x04, "AF_ADJUST_DOWN2": 0x08,
    "AF_ADJUST_TILDE_TOP": 0x10, "AF_ADJUST_TILDE_BOTTOM": 0x20, "AF_ADJUST_TILDE_TOP2": 0x40,
    "AF_ADJUST_TILDE_BOTTOM2": 0x80, "AF_IGNORE_CAPITAL_TOP": 0x100, "AF_IGNORE_CAPITAL_BOTTOM": 0x200,
    "AF_IGNORE_SMALL_TOP": 0x400, "AF_IGNORE_SMALL_BOTTOM": 0x800, "AF_ADJUST_NO_HEIGHT_CHECK": 0x1000,
}


def read_pinned(path, pinned):
    data = open(path, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != pinned:
        sys.stderr.write("gen_hinter_adjust: %s is not the pinned file (%s)\n" % (path, digest))
        sys.exit(1)
    return data.decode("utf-8")


def without_disabled(text):
    """The text without its `#if 0` blocks."""
    out = []
    depth = 0
    for line in text.split("\n"):
        stripped = line.strip()
        if depth > 0:
            if stripped.startswith("#if"):
                depth += 1
            elif stripped.startswith("#endif"):
                depth -= 1
            continue
        if re.match(r"#\s*if\s+0\b", stripped):
            depth = 1
            continue
        out.append(line)
    return "\n".join(out)


def search(table, character):
    """FreeType's af_adjustment_database_lookup, on the table as it is."""
    low = 0
    high = len(table) - 1
    while high >= low:
        mid = (low + high) // 2
        if mid >= len(table):
            raise SystemExit("gen_hinter_adjust: the search for U+%04X leaves the table" % character)
        if table[mid][0] < character:
            low = mid + 1
        elif table[mid][0] > character:
            if mid == 0:
                raise SystemExit("gen_hinter_adjust: the search for U+%04X goes below the table" % character)
            high = mid - 1
        else:
            return table[mid][1]
    return 0


def main() -> int:
    if len(sys.argv) != 5:
        sys.stderr.write(__doc__)
        return 2
    adjust_text = without_disabled(read_pinned(sys.argv[1], PINNED_ADJUST))
    blue_text = read_pinned(sys.argv[2], PINNED_BLUE)
    tables_text = open(sys.argv[3], encoding="utf-8").read()

    # The table of characters.
    body = adjust_text[adjust_text.index("adjustment_database[] ="):]
    body = body[:body.index("};")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    table = []
    for character, flags_text in re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,([^{}]*)\}", body):
        flags = 0
        for name in re.findall(r"AF_\w+", flags_text):
            flags |= FLAGS[name]
        table.append((int(character, 16), flags))
    found = [(character, search(table, character)) for character, _ in table]
    missed = [character for (character, own), (_, got) in zip(table, found) if own != got]
    out_of_order = sum(1 for index in range(1, len(table)) if table[index][0] <= table[index - 1][0])

    # The blue strings and which of them are the bottoms of capitals and of small letters.
    strings_part = blue_text[blue_text.index("AF_BLUE_STRING_ENUM"):blue_text.index("AF_BLUE_STRINGSET_ENUM")]
    sets_part = blue_text[blue_text.index("AF_BLUE_STRINGSET_ENUM"):]
    strings = {}
    name = None
    for line in strings_part.split("\n"):
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("#"):
            continue
        named = re.fullmatch(r"(AF_BLUE_STRING_\w+)", stripped)
        if named:
            name = named.group(1)
            strings[name] = ""
            continue
        quoted = re.fullmatch(r'"(.*)"', stripped)
        if quoted and name is not None:
            strings[name] += quoted.group(1)
    bottoms = {}
    for entry_name, properties in re.findall(r"\{\s*(AF_BLUE_STRING_\w+)\s*,([^{}]*)\}", sets_part):
        if entry_name == "AF_BLUE_STRING_MAX":
            continue
        bottom = (1 if "AF_BLUE_PROPERTY_LATIN_CAPITAL_BOTTOM" in properties else 0) | \
                 (2 if "AF_BLUE_PROPERTY_LATIN_SMALL_BOTTOM" in properties else 0)
        key = strings[entry_name].replace(" ", "")
        if bottoms.setdefault(key, bottom) != bottom:
            raise SystemExit("gen_hinter_adjust: %s is used with two sets of properties" % entry_name)
    characters = [int(number, 16) for number in re.findall(
        r"0x[0-9A-Fa-f]+", tables_text[tables_text.index("table_characters[]"):tables_text.index("table_blues[]")])]
    blues_part = tables_text[tables_text.index("table_blues[]"):tables_text.index("table_scripts[]")]
    blue_bottoms = []
    unknown = []
    for first, count, _ in re.findall(r"\{(\d+), (\d+), (\d+)\}", blues_part):
        key = "".join(chr(c) for c in characters[int(first):int(first) + int(count)]).replace(" ", "")
        if key not in bottoms:
            # A script FreeType 2.14.2 has not: neither kind of bottom.
            unknown.append(key)
            blue_bottoms.append(0)
            continue
        blue_bottoms.append(bottoms[key])
    if len(unknown) > 2:
        raise SystemExit("gen_hinter_adjust: %d blue strings of hinter_tables.inc are not in afblue.dat" % len(unknown))

    out = []
    out.append("// Generated by tools/gen_hinter_adjust.py from FreeType 2.14.2's src/autofit/afadjust.c")
    out.append("// (SHA-256 %s)" % PINNED_ADJUST)
    out.append("// and afblue.dat (SHA-256 %s)." % PINNED_BLUE)
    out.append("// Do not edit: %d characters, %d blue zones." % (len(found), len(blue_bottoms)))
    out.append("")
    out.append("// For each blue zone of hinter_tables.inc's table_blues: 1 the bottom of capitals, 2 the")
    out.append("// bottom of small letters.")
    out.append("inline constexpr u8 table_blue_bottoms[] = {")
    for start in range(0, len(blue_bottoms), 32):
        out.append("    " + " ".join("%d," % b for b in blue_bottoms[start:start + 32]))
    out.append("};")
    out.append("")
    out.append("// A character and what is done for it (the adjust_ bits), in FreeType's order.")
    out.append("inline constexpr TableAdjust table_adjust[] = {")
    for start in range(0, len(found), 6):
        out.append("    " + " ".join("{0x%X, 0x%X}," % entry for entry in found[start:start + 6]))
    out.append("};")
    open(sys.argv[4], "w").write("\n".join(out) + "\n")
    print("characters %d (out of order %d; given other than their own entry: %s), blue zones %d (capital bottoms %d, small bottoms %d, "
          "not in afblue.dat %d)" %
          (len(found), out_of_order, " ".join("U+%04X" % c for c in missed) or "none", len(blue_bottoms),
           sum(1 for b in blue_bottoms if b & 1), sum(1 for b in blue_bottoms if b & 2), len(unknown)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
