#!/usr/bin/env python3
"""STXlib's time zone compiler: IANA tzdata source -> src/zones/zone_data.inc.

Owner decision D12 = a: zones come from an IANA database compiled into
STXlib; the host supplies only the zone's name. This tool reads the tzdata
source format (the "Rule", "Zone" and "Link" lines that zic(8) reads, as
described in tzdata's own zic.8 and theory.html) from third_party/tzdata/
and writes the tables src/core/zones.cpp reads.

Semantics follow zic's outzone() (tzcode zic.c, read as documented
behaviour, not copied): for each zone line, the rules of its rule set are
applied year by year in order of their transition instants, a wall-clock
rule time being converted with the standard offset and the save in effect
just before it; a zone line starts with the save of the last rule
transition before its start; abbreviations come from the line's FORMAT
(%s with the rule's LETTER, A/B for standard/daylight, %z for the numeric
offset). Leap seconds are not used (Unix time, local_time.hpp).

Output per zone: the type before the first transition, the transitions
(UT second, type) up to a cutoff, and a footer of at most two rules that
repeat every year after it. The tool checks, for every zone, that the
footer reproduces its own full expansion through 2263 exactly, and
refuses to write tables otherwise.

    tzcompile.py <tzdata dir> <output .inc>        write the tables
    tzcompile.py --dump <tzdata dir> <zone>        print a zone's expansion

The checks against a zic-built reference are in tools/tzcheck.py.
"""

import sys

# The data files zic reads by default (tzdata Makefile: TDATA = YDATA NDATA
# BACKWARD, DATAFORM main, no PACKRATDATA).
DATA_FILES = ["africa", "antarctica", "asia", "australasia", "europe", "northamerica",
              "southamerica", "etcetera", "factory", "backward"]

# WallTime's range (local_time.hpp): signed 64-bit nanoseconds, so whole
# seconds from -9223372037 to 9223372036, years 1677 to 2262. Expansion
# runs one year past it.
HORIZON_YEAR = 2263
FIRST_INSTANT = -9223372037
LAST_INSTANT = 9223372036

MONTHS = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"]
WEEKDAYS = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
YEAR_MAX = 1 << 40


class DataError(Exception):
    pass


# ---- calendar ---------------------------------------------------------------

def days_from_civil(year, month, day):
    """Days from 1970-01-01 (Howard Hinnant's days_from_civil)."""
    y = year - 1 if month <= 2 else year
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (month - 3 if month > 2 else month + 9) + 2) // 5 + day - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def weekday_of(days):
    """0 = Sunday for a day count from 1970-01-01 (a Thursday)."""
    return (days + 4) % 7


def month_length(year, month):
    if month == 2:
        leap = year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)
        return 29 if leap else 28
    return [31, 0, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][month - 1]


# ---- parsing ----------------------------------------------------------------

def parse_seconds(text, line):
    """h[:mm[:ss[.frac]]], optionally negative; '-' is zero."""
    if text == "-":
        return 0
    sign = 1
    if text.startswith("-"):
        sign = -1
        text = text[1:]
    parts = text.split(":")
    if not 1 <= len(parts) <= 3:
        raise DataError("bad time %r: %s" % (text, line))
    values = []
    for index, part in enumerate(parts):
        if index == 2 and "." in part:
            whole, fraction = part.split(".", 1)
            # zic rounds fractional seconds to the nearest, ties to even.
            value = int(whole) if whole else 0
            if fraction and (int(fraction[0]) > 5 or (fraction[0] == "5" and (fraction[1:].strip("0") or value % 2 == 1))):
                value += 1
            values.append(value)
        else:
            values.append(int(part))
    hours = values[0]
    minutes = values[1] if len(values) > 1 else 0
    seconds = values[2] if len(values) > 2 else 0
    return sign * (hours * 3600 + minutes * 60 + seconds)


def parse_time_of_day(text, line):
    """A rule's AT or an UNTIL time: (seconds, is_standard, is_ut)."""
    is_standard = False
    is_ut = False
    if text and text[-1] in "wsugz":
        suffix = text[-1]
        text = text[:-1]
        if suffix == "s":
            is_standard = True
        elif suffix in "ugz":
            is_standard = True
            is_ut = True
    return parse_seconds(text, line), is_standard, is_ut


def parse_save(text, line):
    """A SAVE: (seconds, is_dst)."""
    explicit = None
    if text and text[-1] in "sd":
        explicit = text[-1] == "d"
        text = text[:-1]
    seconds = parse_seconds(text, line)
    return seconds, (seconds != 0) if explicit is None else explicit


def parse_month(text, line):
    key = text[:3].lower()
    if key not in MONTHS:
        raise DataError("bad month %r: %s" % (text, line))
    return MONTHS.index(key) + 1


def parse_on(text, line):
    """A day: ('dom', day, None) | ('geq', day, weekday) | ('leq', day, weekday) | ('last', None, weekday)."""
    lowered = text.lower()
    if lowered.startswith("last"):
        key = lowered[4:7]
        if key not in WEEKDAYS:
            raise DataError("bad day %r: %s" % (text, line))
        return ("last", None, WEEKDAYS.index(key))
    for operator, kind in ((">=", "geq"), ("<=", "leq")):
        if operator in lowered:
            weekday_text, day_text = lowered.split(operator, 1)
            key = weekday_text[:3]
            if key not in WEEKDAYS:
                raise DataError("bad day %r: %s" % (text, line))
            return (kind, int(day_text), WEEKDAYS.index(key))
    return ("dom", int(text), None)


def parse_year(text, line, low=None):
    lowered = text.lower()
    if low is not None and "only".startswith(lowered) and lowered:
        return low
    if lowered in ("max", "maximum"):
        return YEAR_MAX
    if lowered in ("min", "minimum"):
        return -YEAR_MAX
    try:
        return int(text)
    except ValueError:
        raise DataError("bad year %r: %s" % (text, line))


class Rule:
    __slots__ = ("name", "low", "high", "month", "on", "at", "at_standard", "at_ut", "save", "is_dst", "letters")


class ZoneLine:
    __slots__ = ("stdoff", "rule_name", "fixed_save", "fixed_dst", "format", "until")


def split_fields(line):
    """Whitespace-separated fields; double quotes group (tzdata rarely uses them)."""
    fields = []
    current = []
    quoted = False
    in_field = False
    for char in line:
        if char == '"':
            quoted = not quoted
            in_field = True
        elif char.isspace() and not quoted:
            if in_field:
                fields.append("".join(current))
                current = []
                in_field = False
        else:
            current.append(char)
            in_field = True
    if in_field:
        fields.append("".join(current))
    return fields


def parse_zone_fields(fields, line):
    zone_line = ZoneLine()
    zone_line.stdoff = parse_seconds(fields[0], line)
    rules = fields[1]
    zone_line.rule_name = None
    zone_line.fixed_save = 0
    zone_line.fixed_dst = False
    if rules != "-":
        if rules[0].isdigit() or rules[0] == "-":
            zone_line.fixed_save, zone_line.fixed_dst = parse_save(rules, line)
        else:
            zone_line.rule_name = rules
    zone_line.format = fields[2]
    zone_line.until = None
    if len(fields) > 3:
        year = parse_year(fields[3], line)
        month = parse_month(fields[4], line) if len(fields) > 4 else 1
        on = parse_on(fields[5], line) if len(fields) > 5 else ("dom", 1, None)
        at = parse_time_of_day(fields[6], line) if len(fields) > 6 else (0, False, False)
        zone_line.until = (year, month, on, at)
    return zone_line


def parse_files(paths):
    rules = {}
    zones = {}
    links = {}
    current_zone = None
    for path in paths:
        with open(path, encoding="utf-8") as handle:
            for raw in handle:
                line = raw.split("#", 1)[0].rstrip("\n")
                fields = split_fields(line)
                if not fields:
                    continue
                keyword = fields[0].lower()
                continuation = raw[0].isspace()
                if continuation and current_zone is not None:
                    zone_line = parse_zone_fields(fields, raw)
                    zones[current_zone].append(zone_line)
                    if zone_line.until is None:
                        current_zone = None
                    continue
                current_zone = None
                if keyword == "rule" or keyword == "r":
                    rule = Rule()
                    rule.name = fields[1]
                    rule.low = parse_year(fields[2], raw)
                    rule.high = parse_year(fields[3], raw, low=rule.low)
                    rule.month = parse_month(fields[5], raw)
                    rule.on = parse_on(fields[6], raw)
                    rule.at, rule.at_standard, rule.at_ut = parse_time_of_day(fields[7], raw)
                    rule.save, rule.is_dst = parse_save(fields[8], raw)
                    rule.letters = "" if fields[9] == "-" else fields[9]
                    rules.setdefault(rule.name, []).append(rule)
                elif keyword == "zone" or keyword == "z":
                    name = fields[1]
                    if name in zones:
                        raise DataError("duplicate zone %s" % name)
                    zone_line = parse_zone_fields(fields[2:], raw)
                    zones[name] = [zone_line]
                    if zone_line.until is not None:
                        current_zone = name
                elif keyword == "link" or keyword == "l":
                    links[fields[2]] = fields[1]
                else:
                    raise DataError("unknown line: %s" % raw)
    return rules, zones, links


# ---- rule arithmetic ----------------------------------------------------------

def day_of(year, month, on):
    """The day count (from 1970-01-01) that `on` names in the month; may leave the month."""
    kind, day, weekday = on
    if kind == "dom":
        return days_from_civil(year, month, day)
    if kind == "last":
        last = days_from_civil(year, month, month_length(year, month))
        return last - (weekday_of(last) - weekday) % 7
    base = days_from_civil(year, month, day)
    if kind == "geq":
        return base + (weekday - weekday_of(base)) % 7
    return base - (weekday_of(base) - weekday) % 7


def local_seconds(year, month, on, at_seconds):
    """Seconds from 1970 of the rule's date and time, before any offset."""
    return day_of(year, month, on) * 86400 + at_seconds


def offset_text(seconds):
    """%z: sign, two-digit hours, then minutes and seconds only when nonzero."""
    sign = "-" if seconds < 0 else "+"
    value = abs(seconds)
    hours, rest = divmod(value, 3600)
    minutes, secs = divmod(rest, 60)
    text = "%s%02d" % (sign, hours)
    if minutes or secs:
        text += "%02d" % minutes
    if secs:
        text += "%02d" % secs
    return text


def abbreviation(zone_line, letters, is_dst, save):
    """zic's doabbr; None when %s needs letters that are not known."""
    form = zone_line.format
    if "/" in form:
        standard, daylight = form.split("/", 1)
        return daylight if is_dst else standard
    if "%z" in form:
        return form.replace("%z", offset_text(zone_line.stdoff + save))
    if "%s" in form:
        if letters is None:
            return None
        return form.replace("%s", letters)
    return form


# ---- expansion -----------------------------------------------------------------

class Expansion:
    """A zone's observable history: initial type and transitions (UT, type),
    a type being (utc_offset, is_dst, abbreviation)."""

    def __init__(self):
        self.initial = None
        self.transitions = []

    def add(self, instant, kind):
        self.transitions.append((instant, kind))

    def finish(self):
        """zic's writezone: sort by instant, then merge a transition into the
        previous one when its local time does not come after the previous
        transition's local time (the previous one is then replaced)."""
        ordered = sorted(self.transitions, key=lambda item: item[0])
        merged = []
        for instant, kind in ordered:
            if merged:
                previous_instant, previous_kind = merged[-1]
                before = merged[-2][1] if len(merged) > 1 else self.initial
                if instant + previous_kind[0] <= previous_instant + before[0]:
                    merged[-1] = (previous_instant, kind)
                    continue
            merged.append((instant, kind))
        self.transitions = merged


def expand_zone(name, zone_lines, rules, horizon_year=HORIZON_YEAR):
    expansion = Expansion()
    default_type = None
    start = None  # UT start of the current line; None for the first line
    start_standard = False
    start_ut = False
    for index, zone_line in enumerate(zone_lines):
        use_until = index < len(zone_lines) - 1
        use_start = index > 0
        stdoff = zone_line.stdoff
        save = 0
        start_offset = stdoff
        start_abbreviation = None
        if use_until and zone_line.until is None:
            raise DataError("%s: line %d has no UNTIL" % (name, index))
        until_local = None
        until_standard = until_ut = False
        if use_until:
            year, month, on, (at_seconds, until_standard, until_ut) = zone_line.until
            until_local = local_seconds(year, month, on, at_seconds)
            until_year = year

        def until_instant():
            value = until_local
            if not until_ut:
                value -= stdoff
            if not until_standard:
                value -= save
            return value

        if zone_line.rule_name is None:
            save = zone_line.fixed_save
            text = abbreviation(zone_line, None, zone_line.fixed_dst, save)
            if text is None:
                raise DataError("%s: %%s without rules" % name)
            kind = (stdoff + save, zone_line.fixed_dst, text)
            if use_start:
                expansion.add(start, kind)
                use_start = False
            else:
                default_type = kind
        else:
            ruleset = rules.get(zone_line.rule_name)
            if ruleset is None:
                raise DataError("%s: no rule %s" % (name, zone_line.rule_name))
            low_year = min(rule.low for rule in ruleset)
            last_year = until_year if use_until else horizon_year
            first_year = max(low_year, -YEAR_MAX)
            stop = False
            for year in range(first_year, last_year + 1):
                todo = [rule for rule in ruleset if rule.low <= year <= rule.high]
                while todo:
                    if use_until:
                        until = until_instant()
                    best = None
                    best_time = None
                    for rule in todo:
                        offset = 0 if rule.at_ut else stdoff
                        if not rule.at_standard:
                            offset += save
                        moment = local_seconds(year, rule.month, rule.on, rule.at) - offset
                        if best is None or moment < best_time:
                            best = rule
                            best_time = moment
                    todo.remove(best)
                    if use_until and best_time >= until:
                        if start_abbreviation is None and stdoff + best.save == start_offset:
                            start_abbreviation = abbreviation(zone_line, best.letters, best.is_dst, best.save)
                        stop = True
                        break
                    save = best.save
                    if use_start and best_time == start:
                        use_start = False
                    if use_start:
                        if best_time < start:
                            start_offset = stdoff + save
                            start_abbreviation = abbreviation(zone_line, best.letters, best.is_dst, best.save)
                            continue
                        if start_abbreviation is None and start_offset == stdoff + save:
                            start_abbreviation = abbreviation(zone_line, best.letters, best.is_dst, best.save)
                    text = abbreviation(zone_line, best.letters, best.is_dst, best.save)
                    kind = (stdoff + best.save, best.is_dst, text)
                    if default_type is None and not best.is_dst:
                        default_type = kind
                    expansion.add(best_time, kind)
                if stop:
                    break
            if use_start:
                is_dst = start_offset != stdoff
                if start_abbreviation is None:
                    start_abbreviation = abbreviation(zone_line, None, is_dst, save)
                if start_abbreviation is None:
                    raise DataError("%s: no abbreviation just after line %d's start" % (name, index))
                kind = (start_offset, is_dst, start_abbreviation)
                if default_type is None and not is_dst:
                    default_type = kind
                expansion.add(start, kind)
        if use_until:
            start = until_instant()
    expansion.initial = default_type
    expansion.finish()
    return expansion


def observable(expansion, first=FIRST_INSTANT, last=LAST_INSTANT):
    """Transitions that change the observable type, inside [first, last], with
    the type in effect at `first`."""
    current = expansion.initial
    result = []
    for instant, kind in expansion.transitions:
        if instant <= first:
            current = kind
            continue
        if instant > last:
            break
        if kind != current:
            result.append((instant, kind))
            current = kind
    initial = expansion.initial
    for instant, kind in expansion.transitions:
        if instant <= first:
            initial = kind
    return initial, result


# ---- footers and tables --------------------------------------------------------

class Footer:
    """The rules that repeat every year after a zone's explicit transitions:
    the standard offset and exactly two rules of the last zone line's rule
    set, both running to "max"."""
    __slots__ = ("stdoff", "rules", "kinds")


def footer_events(footer, year):
    """The footer's transitions in `year`: (UT instant, type), the save before
    each rule being the other rule's (they alternate)."""
    events = []
    first, second = footer.rules
    for rule, other, kind in ((first, second, footer.kinds[0]), (second, first, footer.kinds[1])):
        offset = 0 if rule.at_ut else footer.stdoff
        if not rule.at_standard:
            offset += other.save
        events.append((local_seconds(year, rule.month, rule.on, rule.at) - offset, kind))
    return events


def year_start(year):
    return days_from_civil(year, 1, 1) * 86400


def footer_type_at(footer, footer_start, before_type, instant):
    """What the reader does after the explicit transitions: the latest footer
    event at or before `instant` and not before footer_start, from the years
    around `instant`; else the type in effect at footer_start."""
    year = civil_year_of(instant)
    best = None
    for candidate_year in (year - 1, year, year + 1):
        for moment, kind in footer_events(footer, candidate_year):
            if footer_start <= moment <= instant and (best is None or moment > best[0]):
                best = (moment, kind)
    return before_type if best is None else best[1]


def civil_year_of(instant):
    days = instant // 86400
    # Hinnant's civil_from_days, year only.
    z = days + 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    month = mp + 3 if mp < 10 else mp - 9
    return y + 1 if month <= 2 else y


class CompiledZone:
    __slots__ = ("initial", "transitions", "footer", "footer_start")


def compile_zone(name, zone_lines, rules):
    """The zone's table form, checked against its full expansion."""
    expansion = expand_zone(name, zone_lines, rules)
    initial, full = observable(expansion)
    compiled = CompiledZone()
    compiled.initial = initial
    compiled.footer = None
    compiled.footer_start = None
    compiled.transitions = full
    last = zone_lines[-1]
    if last.rule_name is None:
        return compiled
    ruleset = rules[last.rule_name]
    ongoing = [rule for rule in ruleset if rule.high == YEAR_MAX]
    if not ongoing:
        return compiled
    if len(ongoing) != 2:
        raise DataError("%s: %d rules run to max; the footer takes two" % (name, len(ongoing)))
    footer = Footer()
    footer.stdoff = last.stdoff
    footer.rules = ongoing
    footer.kinds = [(last.stdoff + rule.save, rule.is_dst, abbreviation(last, rule.letters, rule.is_dst, rule.save))
                    for rule in ongoing]
    # The cutoff: after every non-repeating rule year, every repeating rule's
    # first year and the last line's start.
    cutoff = max([rule.low for rule in ongoing] + [rule.high for rule in ruleset if rule.high != YEAR_MAX])
    if len(zone_lines) > 1:
        cutoff = max(cutoff, zone_lines[-2].until[0])
    footer_start = year_start(cutoff + 2)
    explicit = [(instant, kind) for instant, kind in full if instant < footer_start]
    before_type = explicit[-1][1] if explicit else initial
    # The check: the reader's rule reproduces the full expansion at every
    # instant where either changes type, through the end of the range.
    moments = set()
    for instant, _ in full:
        if instant >= footer_start:
            moments.add(instant)
            moments.add(instant - 1)
    for year in range(cutoff + 1, HORIZON_YEAR + 1):
        for moment, _ in footer_events(footer, year):
            if footer_start <= moment <= LAST_INSTANT:
                moments.add(moment)
                moments.add(moment - 1)
    full_instants = [instant for instant, _ in full]
    for moment in sorted(moments):
        if moment < footer_start or moment > LAST_INSTANT:
            continue
        position = bisect_right(full_instants, moment)
        expected = initial if position == 0 else full[position - 1][1]
        actual = footer_type_at(footer, footer_start, before_type, moment)
        if expected != actual:
            raise DataError("%s: the footer gives %r at %d, the expansion %r" % (name, actual, moment, expected))
    compiled.transitions = explicit
    compiled.footer = footer
    compiled.footer_start = footer_start
    return compiled


def bisect_right(values, value):
    low, high = 0, len(values)
    while low < high:
        middle = (low + high) // 2
        if values[middle] <= value:
            low = middle + 1
        else:
            high = middle
    return low


ON_KINDS = {"dom": 0, "geq": 1, "leq": 2, "last": 3}


def write_tables(directory, output):
    paths = ["%s/%s" % (directory, name) for name in DATA_FILES]
    rules, zones, links = parse_files(paths)
    version = open("%s/version" % directory, encoding="utf-8").read().strip()
    names = sorted(zones)
    compiled = {name: compile_zone(name, zones[name], rules) for name in names}
    abbreviations = {}
    types = {}

    def type_index(kind):
        if kind not in types:
            text = kind[2]
            if text not in abbreviations:
                abbreviations[text] = None
            types[kind] = len(types)
        return types[kind]

    zone_rows = []
    transition_instants = []
    transition_types = []
    footer_rows = []
    for name in names:
        zone = compiled[name]
        first = len(transition_instants)
        for instant, kind in zone.transitions:
            transition_instants.append(instant)
            transition_types.append(type_index(kind))
        footer_index = 0
        if zone.footer is not None:
            for kind in zone.footer.kinds:
                type_index(kind)
            footer_rows.append(zone)
            footer_index = len(footer_rows)
        zone_rows.append((name, first, len(zone.transitions), type_index(zone.initial), footer_index))
    pool = []
    pool_offsets = {}
    for text in sorted(abbreviations):
        pool_offsets[text] = sum(len(item) + 1 for item in pool)
        pool.append(text)
    all_names = sorted(list(zones) + list(links), key=lambda text: text.encode("utf-8"))
    zone_number = {name: index for index, name in enumerate(names)}

    def resolve(name):
        while name in links:
            name = links[name]
        return name

    out = []
    out.append("// Generated by tools/tzcompile.py from IANA tzdata %s (third_party/tzdata,\n" % version)
    out.append("// PROVENANCE.md). Do not edit; regenerate (docs: src/core/zones.cpp).\n")
    out.append("// %d zones, %d names with links, %d types, %d transitions, %d footers.\n\n"
               % (len(names), len(all_names), len(types), len(transition_instants), len(footer_rows)))
    out.append('inline constexpr char tzdata_version_text[] = "%s";\n\n' % version)
    out.append("inline constexpr char abbreviation_pool[] =\n")
    for text in sorted(abbreviations):
        out.append('    "%s\\0"\n' % text)
    out.append("    ;\n\n")
    out.append("inline constexpr ZoneType zone_types[] = {\n")
    for kind, index in sorted(types.items(), key=lambda item: item[1]):
        out.append("    {%d, %d, %d},\n" % (kind[0], 1 if kind[1] else 0, pool_offsets[kind[2]]))
    out.append("};\n\n")
    out.append("inline constexpr i64 transition_instants[] = {\n")
    for start in range(0, len(transition_instants), 6):
        out.append("    " + ", ".join("%dLL" % value for value in transition_instants[start:start + 6]) + ",\n")
    out.append("};\n\n")
    out.append("inline constexpr u16 transition_types[] = {\n")
    for start in range(0, len(transition_types), 16):
        out.append("    " + ", ".join(str(value) for value in transition_types[start:start + 16]) + ",\n")
    out.append("};\n\n")
    out.append("// Index 0 is no footer.\n")
    out.append("inline constexpr ZoneFooter zone_footers[] = {\n")
    out.append("    {},\n")
    for zone in footer_rows:
        footer = zone.footer
        parts = []
        for rule, kind in zip(footer.rules, footer.kinds):
            kind_code = ON_KINDS[rule.on[0]]
            day = rule.on[1] if rule.on[1] is not None else 0
            weekday = rule.on[2] if rule.on[2] is not None else 0
            parts.append("{%d, %d, %d, %d, %d, %d, %d, %d, %d}" % (
                rule.month, kind_code, day, weekday, rule.at, 1 if rule.at_standard else 0,
                1 if rule.at_ut else 0, rule.save, type_index(kind)))
        out.append("    {%d, %dLL, {%s}},\n" % (footer.stdoff, zone.footer_start, ", ".join(parts)))
    out.append("};\n\n")
    out.append("inline constexpr ZoneRecord zone_records[] = {\n")
    for name, first, count, initial, footer_index in zone_rows:
        out.append("    {%d, %d, %d, %d}, // %s\n" % (first, count, initial, footer_index, name))
    out.append("};\n\n")
    out.append("// Every zone and link name, sorted by bytes, with its zone.\n")
    out.append("inline constexpr ZoneName zone_names[] = {\n")
    for name in all_names:
        out.append('    {"%s", %d, %d},\n' % (name, len(name.encode("utf-8")), zone_number[resolve(name)]))
    out.append("};\n\n")
    if "Etc/UTC" not in zones:
        raise DataError("the data has no zone Etc/UTC")
    out.append("// The position of Etc/UTC in zone_names (Zone::utc).\n")
    out.append("inline constexpr u16 utc_name_index = %d;\n" % all_names.index("Etc/UTC"))
    out.append("// The longest name, in bytes.\n")
    out.append("inline constexpr usize longest_zone_name = %d;\n"
               % max(len(name.encode("utf-8")) for name in all_names))
    with open(output, "w", encoding="utf-8") as handle:
        handle.write("".join(out))
    return len(names), len(all_names), len(types), len(transition_instants), len(footer_rows)


def main(argv):
    if len(argv) == 4 and argv[1] == "--dump":
        directory, zone = argv[2], argv[3]
        rules, zones, links = parse_files(["%s/%s" % (directory, name) for name in DATA_FILES])
        target = links.get(zone, zone)
        initial, transitions = observable(expand_zone(target, zones[target], rules))
        print("initial", initial)
        for instant, kind in transitions:
            print(instant, kind)
        return 0
    if len(argv) == 3:
        counts = write_tables(argv[1], argv[2])
        print("zones %d, names %d, types %d, transitions %d, footers %d" % counts)
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
