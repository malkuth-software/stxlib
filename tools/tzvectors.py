#!/usr/bin/env python3
"""Checks the zone reader's rule over the generated tables, and writes the
C++ test vectors.

    tzvectors.py <tzdata dir> <zone_data.inc> [<vectors .inc>]

1. It parses src/zones/zone_data.inc as written (not tzcompile.py's
   objects) and applies src/core/zones.cpp's reader to it, transcribed
   line for line below (type_at, footer_type_at, rule_instant, rule_day).
2. For every zone and link name, it compares that reading with
   tzcompile.py's full expansion of the tzdata source (observable types,
   through 2263, no footer) at every transition instant of either inside
   WallTime's range and one second before each, and at both ends of the
   range. Any difference is printed and the exit status is 1.
3. With a third argument it writes tests/zone_vectors.inc: for each name,
   up to fourteen (instant, offset, daylight, abbreviation) rows from the
   expansion (both range ends, the first and last explicit transitions,
   footer-region transitions in 2026, 2100 and 2262, and evenly spaced
   transitions between), for tests/local_time_tests.cpp to check the C++
   reader against.

Step 2 is exhaustive over the whole seconds of the range: both functions
are constant between consecutive change points, so agreement at each
change point of either and one second before it, for every name, is
agreement at every whole second. The reader's own change
points (its table transitions, footer start and every footer event through
2263) are added to the instants, so that a change of the reader between two
expansion transitions is not missed either. It checks the Python
transcription of the reader, not the compiled C++; the C++ is checked
against the vectors, which are a sample.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tzcompile  # noqa: E402


def parse_tables(path):
    text = open(path, encoding="utf-8").read()

    def block(name):
        start = text.index(name)
        start = text.index("{", start) + 1
        end = text.index("\n};", start)
        return text[start:end]

    pool_start = text.index("abbreviation_pool[] =")
    pool_end = text.index(";", pool_start)
    pool_literals = re.findall(r'"([^"]*)\\0"', text[pool_start:pool_end])
    pool = "".join(literal + "\0" for literal in pool_literals)
    types = [tuple(int(v) for v in row) for row in re.findall(r"\{(-?\d+), (\d), (\d+)\}", block("zone_types[]"))]
    instants = [int(v) for v in re.findall(r"(-?\d+)LL", block("transition_instants[]"))]
    kinds = [int(v) for v in re.findall(r"\d+", block("transition_types[]"))]
    footers = [None]
    footer_pattern = re.compile(r"\{(-?\d+), (-?\d+)LL, \{\{([^}]*)\}, \{([^}]*)\}\}\}")
    for match in footer_pattern.finditer(block("zone_footers[]")):
        rules = [tuple(int(v) for v in match.group(group).split(", ")) for group in (3, 4)]
        footers.append((int(match.group(1)), int(match.group(2)), rules))
    records = [tuple(int(v) for v in row) for row in re.findall(r"\{(\d+), (\d+), (\d+), (\d+)\}, //", block("zone_records[]"))]
    names = [(name, int(zone)) for name, _, zone in re.findall(r'\{"([^"]+)", (\d+), (\d+)\}', block("zone_names[]"))]
    return pool, types, instants, kinds, footers, records, names


class Reader:
    """src/core/zones.cpp's reader over the parsed tables."""

    def __init__(self, tables):
        self.pool, self.types, self.instants, self.kinds, self.footers, self.records, self.names = tables

    def rule_day(self, rule, year):
        month, kind, day, weekday = rule[0], rule[1], rule[2], rule[3]
        if kind == 3:
            first_next = tzcompile.days_from_civil(year + 1, 1, 1) if month == 12 else tzcompile.days_from_civil(year, month + 1, 1)
            last = first_next - 1
            return last - (tzcompile.weekday_of(last) - weekday) % 7
        base = tzcompile.days_from_civil(year, month, day)
        if kind == 1:
            return base + (weekday - tzcompile.weekday_of(base)) % 7
        if kind == 2:
            return base - (tzcompile.weekday_of(base) - weekday) % 7
        return base

    def rule_instant(self, footer, which, year):
        stdoff, _, rules = footer
        rule, other = rules[which], rules[1 - which]
        at, at_standard, at_ut, save = rule[4], rule[5], rule[6], rule[7]
        offset = 0 if at_ut else stdoff
        if not at_standard:
            offset += other[7]
        return self.rule_day(rule, year) * 86400 + at - offset

    def footer_type_at(self, footer, before, seconds):
        year = tzcompile.civil_year_of(seconds)
        best_moment = footer[1] - 1
        best_type = before
        for candidate in (year - 1, year, year + 1):
            for which in (0, 1):
                moment = self.rule_instant(footer, which, candidate)
                if moment <= seconds and moment > best_moment:
                    best_moment = moment
                    best_type = footer[2][which][8]
        return best_type

    def type_at(self, zone, seconds):
        first, count, initial, footer_index = self.records[zone]
        low, high = first, first + count
        while low < high:
            middle = low + (high - low) // 2
            if self.instants[middle] <= seconds:
                low = middle + 1
            else:
                high = middle
        kind = initial
        if low > first:
            kind = self.kinds[low - 1]
        if footer_index != 0:
            footer = self.footers[footer_index]
            if seconds >= footer[1]:
                kind = self.footer_type_at(footer, kind, seconds)
        return kind

    def reading(self, name_index, seconds):
        offset, is_dst, position = self.types[self.type_at(self.names[name_index][1], seconds)]
        text = self.pool[position:self.pool.index("\0", position)]
        return (offset, bool(is_dst), text)


def expansion_reading(initial, transitions, index, seconds):
    position = tzcompile.bisect_right(index, seconds)
    return initial if position == 0 else transitions[position - 1][1]


def main(argv):
    if len(argv) not in (3, 4):
        print(__doc__)
        return 2
    rules, zones, links = tzcompile.parse_files(["%s/%s" % (argv[1], name) for name in tzcompile.DATA_FILES])
    reader = Reader(parse_tables(argv[2]))
    first, last = tzcompile.FIRST_INSTANT, tzcompile.LAST_INSTANT
    failures = 0
    compared = 0
    rows = []
    expansions = {}
    for name_index, (name, _) in enumerate(reader.names):
        target = name
        while target in links:
            target = links[target]
        if target not in expansions:
            expansion = tzcompile.expand_zone(target, zones[target], rules)
            expansions[target] = tzcompile.observable(expansion)
        initial, transitions = expansions[target]
        index = [instant for instant, _ in transitions]
        instants = {first, last}
        for instant in index:
            instants.add(instant)
            if instant - 1 >= first:
                instants.add(instant - 1)
        # The reader's own change points: its table transitions, its footer
        # start and every footer event through the range.
        zone = reader.names[name_index][1]
        start, count, _, footer_index = reader.records[zone]
        for instant in reader.instants[start:start + count]:
            instants.update((instant, instant - 1))
        if footer_index != 0:
            footer = reader.footers[footer_index]
            instants.update((footer[1], footer[1] - 1))
            for year in range(tzcompile.civil_year_of(footer[1]) - 1, tzcompile.HORIZON_YEAR + 1):
                for which in (0, 1):
                    moment = reader.rule_instant(footer, which, year)
                    instants.update((moment, moment - 1))
        instants = {seconds for seconds in instants if first <= seconds <= last}
        for seconds in sorted(instants):
            compared += 1
            expected = expansion_reading(initial, transitions, index, seconds)
            actual = reader.reading(name_index, seconds)
            if expected != actual:
                failures += 1
                if failures <= 20:
                    print("%s at %d: reader %r, expansion %r" % (name, seconds, actual, expected))
        chosen = [first, last]
        if index:
            chosen += [index[0], index[-1]]
            for year in (2026, 2100, 2262):
                start, end = tzcompile.year_start(year), tzcompile.year_start(year + 1)
                chosen += [instant for instant in index if start <= instant < end][:2]
            step = max(1, len(index) // 8)
            chosen += index[::step][:8]
        for seconds in sorted(set(chosen))[:14]:
            for probe in (seconds, seconds - 1):
                if first <= probe <= last:
                    rows.append((name_index, probe, expansion_reading(initial, transitions, index, probe)))
    print("names %d, instants compared %d, differences %d" % (len(reader.names), compared, failures))
    if len(argv) == 4:
        out = ["// Generated by tools/tzvectors.py from tzcompile.py's full expansion of\n",
               "// third_party/tzdata (not from the tables). Do not edit.\n",
               "// {name, unix seconds, offset seconds, daylight, abbreviation}\n",
               "inline constexpr ZoneVector zone_vectors[] = {\n"]
        for name_index, seconds, (offset, is_dst, text) in rows:
            out.append('    {"%s", %dLL, %d, %d, "%s"},\n'
                       % (reader.names[name_index][0], seconds, offset, 1 if is_dst else 0, text))
        out.append("};\n")
        with open(argv[3], "w", encoding="utf-8") as handle:
            handle.write("".join(out))
        print("vectors %d" % len(rows))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
