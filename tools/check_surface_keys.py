#!/usr/bin/env python3
"""check_surface_keys.py: holds the display surface's key table
(src/core/surface_keys.cpp, usage_by_code) to FreeBSD's own USB-to-evdev
table at the pin (sys/dev/evdev/evdev_utils.c, evdev_usb_scancodes[], with
the KEY_* numbers of sys/dev/evdev/input-event-codes.h), entry by entry
over all 256 usages:

  - every code our table maps to usage u is a key FreeBSD's table names at
    usage u;
  - every usage FreeBSD's table maps to a key below 256 is in ours, at that
    key's code, unless FreeBSD maps several usages to the same key (our
    table, an inverse, keeps one of them; each such case is printed).

Usage: check_surface_keys.py <freebsd-src> [<stxlib-src>]
Exit 0 when the tables agree, 1 with each disagreement printed.
"""

import os
import re
import sys
from typing import Dict, List


def key_numbers(path: str) -> Dict[str, int]:
    numbers: Dict[str, int] = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        match = re.match(r'#define\s+(KEY_\w+|BTN_\w+)\s+(0x[0-9a-fA-F]+|\d+|KEY_\w+|BTN_\w+)\b', line)
        if match is None:
            continue
        name, value = match.group(1), match.group(2)
        if value in numbers:
            numbers[name] = numbers[value]
        elif not value.startswith(('KEY_', 'BTN_')):
            numbers[name] = int(value, 0)
    return numbers


def freebsd_table(path: str, numbers: Dict[str, int]) -> List[int]:
    text = open(path, encoding='utf-8', errors='replace').read()
    match = re.search(r'evdev_usb_scancodes\s*\[[^\]]*\]\s*=\s*\{(.*?)\};', text, re.S)
    if match is None:
        raise ValueError(f'no evdev_usb_scancodes in {path}')
    body = re.sub(r'/\*.*?\*/', '', match.group(1), flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    entries = [entry.strip() for entry in body.split(',') if entry.strip()]
    table = []
    for entry in entries:
        if entry == 'NONE' or entry == 'KEY_RESERVED':
            table.append(0)
        elif entry in numbers:
            table.append(numbers[entry])
        else:
            raise ValueError(f'unknown key name {entry!r} in evdev_usb_scancodes')
    return table


def our_table(path: str) -> List[int]:
    text = open(path, encoding='utf-8').read()
    match = re.search(r'usage_by_code\[evdev_code_count\]\s*=\s*\{(.*?)\};', text, re.S)
    if match is None:
        raise ValueError(f'no usage_by_code in {path}')
    body = re.sub(r'//[^\n]*', '', match.group(1))
    values = [int(value, 16) for value in re.findall(r'0x[0-9A-Fa-f]+', body)]
    if len(values) != 256:
        raise ValueError(f'usage_by_code has {len(values)} entries, not 256')
    return values


def main(argv: List[str]) -> int:
    if len(argv) not in (1, 2):
        print(__doc__)
        return 2
    freebsd = argv[0]
    stxlib = argv[1] if len(argv) == 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    numbers = key_numbers(os.path.join(freebsd, 'sys/dev/evdev/input-event-codes.h'))
    theirs = freebsd_table(os.path.join(freebsd, 'sys/dev/evdev/evdev_utils.c'), numbers)
    ours = our_table(os.path.join(stxlib, 'src/core/surface_keys.cpp'))
    findings = 0
    for code, usage in enumerate(ours):
        if usage == 0:
            continue
        if usage >= len(theirs) or theirs[usage] != code:
            print(f'check_surface_keys: code {code} -> usage 0x{usage:02X}; FreeBSD names '
                  f'{theirs[usage] if usage < len(theirs) else "nothing"} there')
            findings += 1
    usages_of: Dict[int, List[int]] = {}
    for usage, code in enumerate(theirs):
        if code != 0 and code < 256:
            usages_of.setdefault(code, []).append(usage)
    for code, usages in sorted(usages_of.items()):
        if ours[code] in usages:
            if len(usages) > 1:
                print(f'check_surface_keys: note: FreeBSD maps usages {[hex(u) for u in usages]} to code {code}; '
                      f'ours keeps 0x{ours[code]:02X}')
            continue
        print(f'check_surface_keys: code {code}: FreeBSD has usages {[hex(u) for u in usages]}, ours 0x{ours[code]:02X}')
        findings += 1
    print(f'check_surface_keys: usages={len(theirs)} ours_mapped={sum(1 for u in ours if u)} findings={findings}')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
