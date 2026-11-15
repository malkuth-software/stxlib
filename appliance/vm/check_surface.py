#!/usr/bin/env python3
"""check_surface.py: hold the run-time surface (what surface.sh printed on
the console) to what the configuration allows (C2, appliance/C2_DESIGN.md,
"Surface inventory"). Every STXSURFACE line is checked; none is sampled.

  devices    every devfs entry matches a pattern the boot ruleset shows:
             runtime/supervisor/device_rules.h's base_patterns (read from
             that file, so the two cannot drift) or a foreground target's
             --device pattern, with fnmatch(3) FNM_PATHNAME semantics as
             devfs applies them ('*' and '?' never match '/');
  listeners  every listening socket is one the configuration allows: sshd
             on TCP port 22, a target's declared --port (proto/port, any
             program), or one of the base programs' own sockets named in
             ALLOWED_PROGRAMS.

Usage: check_surface.py <console-log> [--device PATTERN]... [--port tcp/N]...
Prints one line per finding and a summary; exit 1 on any finding, or when
the log has no "STXSURFACE done" (the listing did not finish).
"""

import argparse
import os
import re
import sys
from typing import List, Optional, Sequence, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
DEVICE_RULES_H = os.path.join(HERE, '..', '..', 'runtime', 'supervisor', 'device_rules.h')

# Base programs whose sockets are part of the appliance by design: dhclient
# (its sending socket; the receive side is bpf) and stxsntp (one query at
# boot, while it runs).
ALLOWED_PROGRAMS: Tuple[str, ...] = ('dhclient', 'stxsntp')


def base_patterns(path: str) -> List[str]:
    text = open(path, encoding='utf-8').read()
    match = re.search(r'base_patterns\[\]\s*=\s*\{(.*?)\};', text, re.S)
    if match is None:
        raise ValueError(f'no base_patterns in {path}')
    return re.findall(r'"([^"]*)"', match.group(1))


def pattern_regex(pattern: str) -> 're.Pattern':
    """fnmatch(3) with FNM_PATHNAME: '*' and '?' stop at '/'; [..] classes."""
    out = []
    index = 0
    while index < len(pattern):
        char = pattern[index]
        if char == '*':
            out.append('[^/]*')
        elif char == '?':
            out.append('[^/]')
        elif char == '[':
            end = pattern.find(']', index + 2)
            if end < 0:
                out.append(re.escape(char))
            else:
                body = pattern[index + 1:end]
                if body.startswith('!'):
                    body = '^' + body[1:]
                out.append('(?!/)[' + body.replace('\\', '\\\\') + ']')
                index = end
        else:
            out.append(re.escape(char))
        index += 1
    return re.compile(''.join(out) + r'\Z')


def check_devices(entries: Sequence[str], patterns: Sequence[str]) -> List[str]:
    compiled = [pattern_regex(pattern) for pattern in patterns]
    return [entry for entry in entries if not any(regex.match(entry) for regex in compiled)]


def parse_listener(line: str) -> Optional[Tuple[str, str, str, str]]:
    """sockstat -l's row: USER COMMAND PID FD PROTO LOCAL FOREIGN."""
    fields = line.split()
    if len(fields) < 6 or fields[0] == 'USER':
        return None
    return fields[1], fields[4], fields[5], fields[0]


def listener_allowed(command: str, proto: str, local: str, ports: Sequence[str]) -> bool:
    port = local.rsplit(':', 1)[-1] if ':' in local else ''
    family = 'tcp' if proto.startswith('tcp') else 'udp' if proto.startswith('udp') else proto
    if command == 'sshd' and family == 'tcp' and port == '22':
        return True
    if f'{family}/{port}' in ports:
        return True
    return command in ALLOWED_PROGRAMS


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('log')
    parser.add_argument('--device', action='append', default=[])
    parser.add_argument('--port', action='append', default=[])
    arguments = parser.parse_args(argv)
    devices: List[str] = []
    deeper: List[str] = []
    listeners: List[str] = []
    done = False
    with open(arguments.log, 'r', encoding='utf-8', errors='replace') as stream:
        for raw in stream:
            line = raw.replace('\r', '').rstrip('\n')
            at = line.find('STXSURFACE ')
            if at < 0:
                continue
            body = line[at + len('STXSURFACE '):]
            if body.startswith('dev '):
                devices.append(body[4:])
            elif body.startswith('deeper '):
                deeper.append(body[7:])
            elif body.startswith('listen '):
                listeners.append(body[7:])
            elif body == 'done':
                done = True
    patterns = base_patterns(DEVICE_RULES_H) + list(arguments.device)
    findings = 0
    for entry in check_devices(devices, patterns):
        print(f'check_surface: device not allowed: /dev/{entry}')
        findings += 1
    for entry in deeper:
        print(f'check_surface: devfs entry below three levels, not listed: /dev/{entry}')
        findings += 1
    sockets = 0
    for line in listeners:
        parsed = parse_listener(line)
        if parsed is None:
            continue
        sockets += 1
        command, proto, local, _user = parsed
        if not listener_allowed(command, proto, local, arguments.port):
            print(f'check_surface: listener not allowed: {line}')
            findings += 1
    if not done:
        print('check_surface: the listing did not finish (no "STXSURFACE done")')
        findings += 1
    print(f'check_surface: devices={len(devices)} listeners={sockets} patterns={len(patterns)} findings={findings}')
    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
