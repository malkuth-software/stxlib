#!/usr/bin/env python3
"""Writes src/algorithms/glyphs/shape_unicode.inc from harfrust 0.16.0's Unicode tables.

The inputs are four files of harfrust 0.16.0 as released (PROVENANCE.md: stxlib::glyphs, the
shaper): src/unicode/ucd_table.rs and src/unicode/emoji_table.rs (generated there from the
Unicode Character Database), src/unicode/mod.rs (the order marks are put in) and
src/direction.rs with src/script.rs (the scripts and which way each is written). They are not
in this tree: the tables are made again only when a later harfrust is taken, by running this
with the folder of its sources.

    python3 -I tools/gen_shape_unicode.py <harfrust>/src <shape_unicode.inc>

harfrust's tables are packed for its own readers. Here every character's values are worked
out with those readers (their expressions are taken from the files and run as they stand) and
written again in this library's own form: one table of the distinct kinds of character and a
three-level index into it, and lists in order for mirrors and for what a character is made of.
A sum over every character's values is written too; the test of the reading code computes the
same sum from the tables as the library reads them.
"""
import hashlib
import re
import sys

PINNED = {
    "unicode/ucd_table.rs": "bf33212c3218f1f855245eb4e409ed644ad16021bec054564d982e25695f944d",
    "unicode/emoji_table.rs": "f3ce774f5a7f2e4f76eea7a51dffae28b74070cb070ecd125699b6ebb866b7f7",
    "unicode/mod.rs": "2711a7a9366313daae1b5121f5a17f1c3889cec6b5520332ccbd408b18524977",
    "direction.rs": "5d263acb38b4f6171b3d5328c533a92664b6967f29ad2d8475b3cd6e12974426",
    "script.rs": "d0553c475bc0418af7fd59272de24373928323473c4c0934a6f0bb9b318762bf",
}
LAST = 0x10FFFF
MASK64 = (1 << 64) - 1


def fail(message):
    sys.stderr.write("gen_shape_unicode: %s\n" % message)
    sys.exit(1)


def read_pinned(folder, name):
    data = open(folder + "/" + name, "rb").read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != PINNED[name]:
        fail("%s is not the pinned file (%s)" % (name, digest))
    return data.decode("utf-8")


def arrays_of(text):
    """Every `static NAME: [T; N]= [ ... ];` of numbers, by name."""
    found = {}
    for match in re.finditer(r"static (\w+): \[(\w+); (\d+)\]=\s*\[(.*?)\];", text, re.S):
        name, kind, count, body = match.group(1), match.group(2), int(match.group(3)), match.group(4)
        if kind == "Script":
            values = re.findall(r"Script::(\w+)", body)
        elif "HB_CODEPOINT_ENCODE3_11_7_14" in body:
            # harfrust's short form of a pair and what it makes: 11, 7 and 14 bits.
            values = [((int(x, 16) & 0x07FF) << 21) | ((int(y, 16) & 0x007F) << 14) | (int(z, 16) & 0x3FFF)
                      for x, y, z in re.findall(r"HB_CODEPOINT_ENCODE3_11_7_14 \((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)", body)]
        elif "HB_CODEPOINT_ENCODE3" in body:
            values = [(int(x, 16) << 42) | (int(y, 16) << 21) | int(z, 16)
                      for x, y, z in re.findall(r"HB_CODEPOINT_ENCODE3 \((0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+)\)", body)]
        else:
            values = [int(item, 0) for item in re.findall(r"-?(?:0x[0-9A-Fa-f_]+|\d+)", body.replace("_", ""))]
        if len(values) != count:
            fail("%s: %d values where %d are declared" % (name, len(values), count))
        found[name] = values
    return found


def reader_of(text, name, env):
    """The body of harfrust's `fn NAME (u: usize)` as a Python function of u."""
    match = re.search(r"fn " + name + r" \(u: usize\) -> \w+\s*\{\s*/\*.*?\*/\s*(.*?)\n\}", text, re.S)
    if match is None:
        fail("no reader %s" % name)
    body = match.group(1).strip()
    shape = re.fullmatch(r"if u<(\d+)usize \{ (.*) \} else \{ (-?\d+) \}", body)
    if shape is None:
        fail("%s: not the expected shape" % name)
    below, expression, otherwise = int(shape.group(1)), shape.group(2), int(shape.group(3))
    expression = re.sub(r"(\d+)usize", r"\1", expression)
    expression = re.sub(r" as [iu](?:8|16|32|64|size)", "", expression)

    def slice_of(found):
        array, start = found.group(1), int(found.group(2))
        key = "%s_from_%d" % (array, start)
        env[key] = env[array][start:]
        return key

    expression = re.sub(r"&(\w+)\[(\d+)\.\.\]", slice_of, expression)
    expression = expression.replace("&", "__AND__")
    # `&name` is a reference to an array; `x&3` is arithmetic and has no space-free name after it.
    expression = re.sub(r"__AND__([A-Za-z_]\w*)", r"\1", expression).replace("__AND__", "&")
    if re.search(r"[^\w\s()\[\]+<>&,]", expression):
        fail("%s: something unexpected in its expression" % name)
    inner = eval("lambda u: " + expression, env)  # the pinned file's own arithmetic, nothing else
    return lambda u: inner(u) if u < below else otherwise


def scripts_of(text):
    return dict(re.findall(r'pub const (\w+): Self = Self::from_bytes\(b"(\w{4})"\);', text))


def directions_of(text):
    """Scripts written right to left, and scripts with no one direction."""
    match = re.search(r"fn from_script\(script: Script\).*?match script \{(.*?)=> \{\s*Some\(Direction::RightToLeft\)\s*\}(.*?)=> \{\s*None\s*\}",
                      text, re.S)
    if match is None:
        fail("direction.rs: from_script is not as expected")
    return set(re.findall(r"Script::(\w+)", match.group(1))), set(re.findall(r"Script::(\w+)", match.group(2)))


def modified_classes_of(text):
    """harfrust's MODIFIED_COMBINING_CLASS: a class is itself unless the module gives it another."""
    module = re.search(r"pub\(crate\) mod modified_combining_class \{(.*?)\n\}", text, re.S)
    table = re.search(r"static MODIFIED_COMBINING_CLASS: &\[u8; 256\] = &\[(.*?)\];", text, re.S)
    names = re.search(r"pub\(crate\) mod combining_class \{(.*?)\n\}", text, re.S)
    if module is None or table is None or names is None:
        fail("mod.rs: the modified classes are not as expected")
    changed = {"modified_combining_class::" + name: int(value) for name, value in re.findall(r"pub const (\w+): u8 = (\d+);", module.group(1))}
    named = {"combining_class::" + name: int(value) for name, value in re.findall(r"pub const (\w+): u8 = (\d+);", names.group(1))}
    body = re.sub(r"//[^\n]*", "", table.group(1))
    result = []
    for item in [piece.strip() for piece in body.split(",") if piece.strip()]:
        if item in changed:
            result.append(changed[item])
        elif item in named:
            result.append(named[item])
        else:
            result.append(int(item))
    if len(result) != 256:
        fail("mod.rs: %d modified classes" % len(result))
    return result


def best_index(kinds):
    """The three-level index of least size: (low bits, middle bits, level 1, level 2, level 3)."""
    best = None
    for low in range(3, 9):
        leaves = {}
        leaf_of = []
        level3 = []
        for start in range(0, LAST + 1, 1 << low):
            block = tuple(kinds[start:start + (1 << low)])
            if block not in leaves:
                leaves[block] = len(leaves)
                level3.extend(block)
            leaf_of.append(leaves[block])
        for middle in range(3, 9):
            groups = {}
            level1 = []
            level2 = []
            for start in range(0, len(leaf_of), 1 << middle):
                block = tuple(leaf_of[start:start + (1 << middle)])
                block = block + (0,) * ((1 << middle) - len(block))
                if block not in groups:
                    groups[block] = len(groups)
                    level2.extend(block)
                level1.append(groups[block])
            size = len(level1) * 2 + len(level2) * 2 + len(level3) * 2
            if best is None or size < best[0]:
                best = (size, low, middle, level1, level2, level3)
    return best[1:]


def mix(total, value):
    total ^= value
    total ^= (total << 13) & MASK64
    total ^= total >> 7
    total ^= (total << 17) & MASK64
    return total


def rows(values, per_line, form):
    lines = []
    for start in range(0, len(values), per_line):
        lines.append("    " + " ".join(form % value + "," for value in values[start:start + per_line]))
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        fail("usage: gen_shape_unicode.py <harfrust>/src <shape_unicode.inc>")
    folder, out_path = sys.argv[1], sys.argv[2]
    ucd = read_pinned(folder, "unicode/ucd_table.rs")
    emoji = read_pinned(folder, "unicode/emoji_table.rs")
    module = read_pinned(folder, "unicode/mod.rs")
    directions = read_pinned(folder, "direction.rs")
    script_text = read_pinned(folder, "script.rs")

    env = {"__builtins__": {}}
    env.update(arrays_of(ucd))
    env.update(arrays_of(emoji))
    env["ucd_b4"] = lambda a, i: (a[i >> 1] >> ((i & 1) << 2)) & 15
    env["emoji_b4"] = env["ucd_b4"]
    env["emoji_b1"] = lambda a, i: (a[i >> 3] >> (i & 7)) & 1
    gc = reader_of(ucd, "ucd_gc", env)
    ccc = reader_of(ucd, "ucd_ccc", env)
    bmg = reader_of(ucd, "ucd_bmg", env)
    sc = reader_of(ucd, "ucd_sc", env)
    dm = reader_of(ucd, "ucd_dm", env)
    pictographic = reader_of(emoji, "emoji_is_extended_pictographic_u8", env)

    # Scripts, in harfrust's order, with which way each is written.
    tags = scripts_of(script_text)
    right_to_left, either = directions_of(directions)
    script_names = env["ucd_sc_map"]
    for name in script_names + sorted(right_to_left) + sorted(either):
        if name not in tags:
            fail("script %s has no tag" % name)
    if script_names[0] != "COMMON" or script_names[1] != "INHERITED" or script_names[2] != "UNKNOWN":
        fail("the first three scripts are not Common, Inherited, Unknown")

    # What a character is made of, and what two characters make (Hangul is worked out, not listed).
    p0, p2, pairs32, pairs64 = env["ucd_dm1_p0_map"], env["ucd_dm1_p2_map"], env["ucd_dm2_u32_map"], env["ucd_dm2_u64_map"]
    compositions = {}
    for value in pairs32:
        a, b, made = value >> 21, ((value >> 14) & 0x7F) | 0x0300, value & 0x3FFF
        if made != 0:
            compositions[(a, b)] = made
    for value in pairs64:
        a, b, made = value >> 42, (value >> 21) & 0x1FFFFF, value & 0x1FFFFF
        if (a & 0xFFFFF800) == 0 and (b & 0xFFFFFF80) == 0x0300:
            fail("a pair of the long list is one the short list is searched for")
        if made != 0:
            compositions[(a, b)] = made

    kinds = []
    kind_numbers = {}
    kind_values = []
    mirrors = []
    decompositions = []
    total = 0
    for u in range(LAST + 1):
        category, order, script, picture = gc(u), ccc(u), sc(u), pictographic(u)
        if category > 29 or order > 255 or script >= len(script_names) or picture > 1:
            fail("U+%04X: a value out of range" % u)
        value = category | (script << 5) | (order << 13) | (picture << 21)
        if value not in kind_numbers:
            kind_numbers[value] = len(kind_values)
            kind_values.append(value)
        kinds.append(kind_numbers[value])
        delta = bmg(u)
        mirror = u + delta if delta != 0 else 0
        if delta != 0:
            mirrors.append((u, mirror))
        first = 0
        second = 0
        place = dm(u)
        if place != 0:
            place -= 1
            if place < len(p0):
                first = p0[place]
            elif place < len(p0) + len(p2):
                first = 0x20000 | p2[place - len(p0)]
            elif place < len(p0) + len(p2) + len(pairs32):
                value32 = pairs32[place - len(p0) - len(p2)]
                first, second = value32 >> 21, ((value32 >> 14) & 0x7F) | 0x0300
            else:
                value64 = pairs64[place - len(p0) - len(p2) - len(pairs32)]
                first, second = value64 >> 42, (value64 >> 21) & 0x1FFFFF
            if first == 0 or first > LAST or second > LAST:
                fail("U+%04X: what it is made of is out of range" % u)
            decompositions.append((u, first, second))
        for part in (category, order, int.from_bytes(tags[script_names[script]].encode("ascii"), "big"), picture, mirror, first, second):
            total = mix(total, part)
    if len(kind_values) > 65535:
        fail("too many kinds of character")
    made_of = {u: (a, b) for u, a, b in decompositions}
    for (a, b), made in compositions.items():
        if made_of.get(made) != (a, b):
            fail("U+%04X is made of a pair it is not listed as made of" % made)
    low, middle, level1, level2, level3 = best_index(kinds)
    # The index is read back for every character before it is written.
    for u in range(LAST + 1):
        a = level1[u >> (low + middle)]
        b = level2[(a << middle) + ((u >> low) & ((1 << middle) - 1))]
        if level3[(b << low) + (u & ((1 << low) - 1))] != kinds[u]:
            fail("U+%04X: the index does not give its kind" % u)
    modified = modified_classes_of(module)

    out = []
    out.append("// Generated by tools/gen_shape_unicode.py from harfrust 0.16.0's src/unicode/ucd_table.rs")
    out.append("// (SHA-256 %s)," % PINNED["unicode/ucd_table.rs"])
    out.append("// emoji_table.rs (%s)," % PINNED["unicode/emoji_table.rs"])
    out.append("// mod.rs (%s)," % PINNED["unicode/mod.rs"])
    out.append("// src/direction.rs (%s)" % PINNED["direction.rs"])
    out.append("// and src/script.rs (%s)." % PINNED["script.rs"])
    out.append("// Do not edit: %d kinds of character, %d scripts, %d mirrors, %d characters made of others," % (
        len(kind_values), len(script_names), len(mirrors), len(decompositions)))
    out.append("// %d pairs that make one." % len(compositions))
    out.append("")
    out.append("// A character's kind is unicode_kinds[level 3[(level 2[(level 1[c >> %d] << %d) + ((c >> %d) & %d)] << %d) + (c & %d)]]." % (
        low + middle, middle, low, (1 << middle) - 1, low, (1 << low) - 1))
    out.append("inline constexpr u32 unicode_low_bits = %d;" % low)
    out.append("inline constexpr u32 unicode_middle_bits = %d;" % middle)
    out.append("inline constexpr u16 unicode_level1[] = {\n%s\n};" % rows(level1, 20, "%d"))
    out.append("inline constexpr u16 unicode_level2[] = {\n%s\n};" % rows(level2, 20, "%d"))
    out.append("inline constexpr u16 unicode_level3[] = {\n%s\n};" % rows(level3, 20, "%d"))
    out.append("")
    out.append("// A kind: the general category (5 bits), the script's number (8), the combining class (8),")
    out.append("// and whether it is an Extended_Pictographic (1).")
    out.append("inline constexpr u32 unicode_kinds[] = {\n%s\n};" % rows(kind_values, 10, "0x%06X"))
    out.append("")
    out.append("// A script's tag (ISO 15924), by its number; then which way it is written: 1 left to right,")
    out.append("// 2 right to left, 0 either.")
    out.append("inline constexpr u32 unicode_script_tags[] = {\n%s\n};" % rows(
        [int.from_bytes(tags[name].encode("ascii"), "big") for name in script_names], 8, "0x%08X"))
    out.append("inline constexpr u8 unicode_script_ways[] = {\n%s\n};" % rows(
        [2 if name in right_to_left else (0 if name in either else 1) for name in script_names], 32, "%d"))
    out.append("")
    out.append("// Scripts written right to left or either way that no character of the table has (a caller")
    out.append("// may name any script): tag, way.")
    extra = sorted(name for name in (right_to_left | either) if name not in script_names)
    out.append("inline constexpr UnicodeWay unicode_other_ways[] = {\n%s\n    {0, 0},\n};" % "\n".join(
        "    {0x%08X, %d}," % (int.from_bytes(tags[name].encode("ascii"), "big"), 2 if name in right_to_left else 0) for name in extra))
    out.append("")
    out.append("// A character and its mirror image, in order.")
    out.append("inline constexpr UnicodePair unicode_mirrors[] = {\n%s\n};" % "\n".join(
        "    " + " ".join("{0x%04X, 0x%04X}," % pair for pair in mirrors[start:start + 6]) for start in range(0, len(mirrors), 6)))
    out.append("")
    out.append("// A character and the one or two it is made of, in order: the character, then its first and")
    out.append("// second parts, 21 bits each (a second part of 0: there is none).")
    out.append("inline constexpr u64 unicode_made_of[] = {\n%s\n};" % rows(
        [(u << 42) | (a << 21) | b for u, a, b in decompositions], 5, "0x%016XULL"))
    out.append("")
    out.append("// Two characters and the one they make, in order: the first, the second, the one made.")
    out.append("inline constexpr u64 unicode_makes[] = {\n%s\n};" % rows(
        [(a << 42) | (b << 21) | made for (a, b), made in sorted(compositions.items())], 5, "0x%016XULL"))
    out.append("")
    out.append("// The order marks are put in, by combining class (harfrust's MODIFIED_COMBINING_CLASS).")
    out.append("inline constexpr u8 unicode_modified_classes[256] = {\n%s\n};" % rows(modified, 20, "%d"))
    out.append("")
    out.append("// Over every character from 0 to 0x10FFFF, of its category, class, script tag, picture bit,")
    out.append("// mirror and parts, each mixed in by: sum ^= value; sum ^= sum << 13; sum ^= sum >> 7;")
    out.append("// sum ^= sum << 17 (64 bits).")
    out.append("inline constexpr u64 unicode_sum = 0x%016XULL;" % total)
    out.append("")
    open(out_path, "w").write("\n".join(out))
    sys.stderr.write("gen_shape_unicode: %d kinds, index %d+%d+%d entries (low %d, middle %d), %d mirrors, %d made of, %d makes\n" % (
        len(kind_values), len(level1), len(level2), len(level3), low, middle, len(mirrors), len(decompositions), len(compositions)))


main()
