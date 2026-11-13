#!/usr/bin/env python3
"""Checks tools/tzcompile.py against zic's output for the same zones.

    tzcheck.py <tzdata dir> <zoneinfo dir> [zone ...]

<zoneinfo dir> holds TZif files built by zic (for example a host's
/usr/share/zoneinfo; its +VERSION file names the tzdata release, which
may differ from <tzdata dir>'s). For every zone and link in the tzdata
(or the ones named), it compares the observable local time type (UTC
offset, daylight flag, abbreviation) of the compiled zone with the
reference at:
  - every transition instant of either side, and one second before it
    (the reference's transitions are its TZif data's explicit ones),
  - and, after the reference's last explicit transition, where its TZif
    footer rule takes over, every instant found by bisecting a weekly
    grid to the second, through the end of WallTime's range.
The reference is read through Python's zoneinfo, which implements the
TZif format and its footer. A mismatch is printed; the summary counts
zones that agree, differ, or are missing from the reference.

This is a comparison with an independent implementation over the listed
instants, not a proof; zones the two tzdata releases change are expected
to differ, and are listed by the release's NEWS.
"""

import datetime
import os
import struct
import sys
import zoneinfo

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tzcompile  # noqa: E402

# Python's datetime reaches year 1 to 9999; WallTime's range is inside it.
EPOCH = datetime.datetime(1970, 1, 1, tzinfo=datetime.timezone.utc)
WEEK = 7 * 86400


def tzif_transitions(path):
    """The explicit transition instants of a TZif file (version 2+ data)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:4] != b"TZif":
        return []

    def header(offset):
        counts = struct.unpack(">6l", data[offset + 20:offset + 44])
        return counts  # isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt

    isut, isstd, leap, time_count, type_count, char_count = header(0)
    if data[4:5] in (b"2", b"3", b"4"):
        skip = 44 + time_count * 5 + type_count * 6 + char_count + leap * 8 + isstd + isut
        isut, isstd, leap, time_count, type_count, char_count = header(skip)
        start = skip + 44
        return list(struct.unpack(">%dq" % time_count, data[start:start + 8 * time_count]))
    start = 44
    return list(struct.unpack(">%dl" % time_count, data[start:start + 4 * time_count]))


class Reference:
    def __init__(self, directory, name):
        self.zone = zoneinfo.ZoneInfo.from_file(open(os.path.join(directory, name), "rb"), key=name)
        self.transitions = tzif_transitions(os.path.join(directory, name))

    def at(self, instant):
        moment = EPOCH + datetime.timedelta(seconds=instant)
        local = moment.astimezone(self.zone)
        offset = local.utcoffset()
        dst = local.dst()
        return (int(offset.total_seconds()), dst is not None and dst != datetime.timedelta(0), local.tzname())


def compiled_at(initial, transitions, instants_index, instant):
    """The compiled type at `instant` (binary search)."""
    low, high = 0, len(instants_index)
    while low < high:
        middle = (low + high) // 2
        if instants_index[middle] <= instant:
            low = middle + 1
        else:
            high = middle
    return initial if low == 0 else transitions[low - 1][1]


def footer_instants(reference, start, end):
    """Instants after `start` where the reference changes type, found by a
    weekly grid and bisection to the second."""
    found = []
    previous_instant = start
    previous = reference.at(start)
    instant = start + WEEK
    while instant <= end:
        current = reference.at(instant)
        if current != previous:
            low, high = previous_instant, instant
            while high - low > 1:
                middle = (low + high) // 2
                if reference.at(middle) == previous:
                    low = middle
                else:
                    high = middle
            found.append(high)
        previous_instant, previous = instant, current
        instant += WEEK
    return found


def compare(name, target, zones, rules, directory, show):
    reference = Reference(directory, name)
    initial, transitions = tzcompile.observable(tzcompile.expand_zone(target, zones[target], rules))
    index = [instant for instant, _ in transitions]
    instants = set()
    for instant in index:
        instants.add(instant)
        instants.add(instant - 1)
    explicit = [t for t in reference.transitions if tzcompile.FIRST_INSTANT < t <= tzcompile.LAST_INSTANT]
    for instant in explicit:
        instants.add(instant)
        instants.add(instant - 1)
    last_explicit = max(explicit) if explicit else tzcompile.FIRST_INSTANT
    footer_start = max(last_explicit, -2208988800)  # from 1900 at the latest
    for instant in footer_instants(reference, footer_start, tzcompile.LAST_INSTANT - WEEK):
        instants.add(instant)
        instants.add(instant - 1)
    instants.add(tzcompile.FIRST_INSTANT + 86400)
    instants.add(tzcompile.LAST_INSTANT - 86400)
    mismatches = []
    for instant in sorted(instants):
        if not tzcompile.FIRST_INSTANT + 86400 <= instant <= tzcompile.LAST_INSTANT - 86400:
            continue
        ours = compiled_at(initial, transitions, index, instant)
        theirs = reference.at(instant)
        if ours != theirs:
            mismatches.append((instant, ours, theirs))
    if mismatches and show:
        for instant, ours, theirs in mismatches[:6]:
            moment = EPOCH + datetime.timedelta(seconds=instant)
            print("  %s %s (%d): ours %s, reference %s" % (name, moment.isoformat(), instant, ours, theirs))
        if len(mismatches) > 6:
            print("  %s: %d more" % (name, len(mismatches) - 6))
    return len(instants), mismatches


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    tzdata, directory = argv[1], argv[2]
    rules, zones, links = tzcompile.parse_files(["%s/%s" % (tzdata, name) for name in tzcompile.DATA_FILES])
    names = argv[3:] or sorted(list(zones) + list(links))
    agree = differ = missing = 0
    flag_only = 0
    checked = 0
    differing = []
    flag_differing = []
    for name in names:
        target = links.get(name, name)
        while target in links:
            target = links[target]
        if not os.path.exists(os.path.join(directory, name)):
            missing += 1
            continue
        count, mismatches = compare(name, target, zones, rules, directory, False)
        checked += count
        if not mismatches:
            agree += 1
        elif all(ours[0] == theirs[0] and ours[2] == theirs[2] for _, ours, theirs in mismatches):
            # Offset and abbreviation agree; only the daylight flag differs:
            # tzdata's main form gives negative SAVE values (Europe/Dublin,
            # Africa/Casablanca and their links), its rearguard form, which
            # some zic builds use, gives positive ones.
            flag_only += 1
            flag_differing.append(name)
        else:
            differ += 1
            differing.append(name)
            compare(name, target, zones, rules, directory, True)
    print("zones and links: %d agree, %d differ only in the daylight flag, %d differ, %d missing from the "
          "reference; %d instants compared" % (agree, flag_only, differ, missing, checked))
    if flag_differing:
        print("daylight flag only: " + " ".join(flag_differing))
    if differing:
        print("differ: " + " ".join(differing))
    return 0 if differ == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
