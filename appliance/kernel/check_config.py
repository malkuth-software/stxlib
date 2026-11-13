#!/usr/bin/env python3
"""check_config.py <sys dir> <kernel config> [<extra include dir>]

Static pre-build check of an arm64 kernel configuration, for configurations
cut down from GENERIC. It evaluates the configuration as config(8) would
(include, device, options, nodevice, nooptions; DEFAULTS first), selects the
source files sys/conf/files and files.arm64 would compile, and reports every
compiled C file that includes a generated "<name>_if.h" whose .m source is not
selected: the failure mode of a missing clknode_if.h. It does not find
undeclared functions or missing link symbols; a clean result is not a build.
Includes under #if are followed too, so it over-reports: compare a cut against
its baseline (STXLIB-GENERIC) and read only the lines the cut adds.
Python 3.9; reads only.
"""
import os
import re
import sys

KEYWORDS = {'compile-with', 'dependency', 'clean', 'no-obj', 'no-implicit-rule',
            'before-depend', 'local', 'warning', 'obj-prefix', 'no-ctfconvert',
            'no-depend'}


def config_names(path, include_dirs, names=None):
    """Return the set of lower-case device and option names of a config."""
    names = set() if names is None else names
    for raw in open(path):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        words = line.split()
        word, rest = words[0], [w.strip('"') for w in words[1:]]
        if word == 'include':
            for d in include_dirs:
                candidate = os.path.join(d, rest[0])
                if os.path.exists(candidate):
                    config_names(candidate, include_dirs, names)
                    break
            else:
                raise SystemExit(f'include not found: {rest[0]}')
        elif word in ('device', 'devices', 'options', 'option'):
            for item in ' '.join(rest).split(','):
                names.add(item.strip().split('=')[0].lower())
        elif word in ('nodevice', 'nodevices', 'nooptions', 'nooption'):
            for item in ' '.join(rest).split(','):
                names.discard(item.strip().lower())
    return names


def file_entries(path):
    text = re.sub(r'\\\n', ' ', open(path).read())
    for raw in text.splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if len(words) < 2:
            continue
        yield words[0], words[1], words[2:]


def selected(kind, words, names):
    if kind == 'standard':
        return True
    if kind != 'optional':
        return False
    expr = []
    for w in words:
        if w in KEYWORDS:
            break
        expr.append(w)
    alternative, ok = [], False
    for w in expr + ['|']:
        if w == '|':
            if alternative and all((n[1:] not in names) if n.startswith('!') else (n in names)
                                   for n in alternative):
                ok = True
            alternative = []
        else:
            alternative.append(w.lower())
    return ok


INCLUDE = re.compile(r'^\s*#\s*include\s+([<"])([^>"]+)[>"]')
_memo = {}


def interface_includes(path, sysdir, stack=()):
    """Map each generated *_if.h reachable from path (ignoring #if; an
    over-approximation) to the first file that includes it."""
    if path in _memo:
        return _memo[path]
    if path in stack:
        return {}
    found = {}
    _memo[path] = found
    for line in open(path, errors='replace'):
        m = INCLUDE.match(line)
        if not m:
            continue
        name = m.group(2)
        if re.fullmatch(r'[A-Za-z0-9_]+_if\.h', name):
            found.setdefault(name, os.path.relpath(path, sysdir))
            continue
        machine = (os.path.join(sysdir, 'arm64', 'include', name[8:])
                   if name.startswith('machine/') else None)
        for candidate in (os.path.join(os.path.dirname(path), name) if m.group(1) == '"' else None,
                          machine, os.path.join(sysdir, name)):
            if candidate and os.path.isfile(candidate):
                for header, via in interface_includes(candidate, sysdir, stack + (path,)).items():
                    found.setdefault(header, via)
                break
    return found


def main():
    sysdir, conf = sys.argv[1], sys.argv[2]
    confdir = os.path.join(sysdir, 'arm64', 'conf')
    dirs = [d for d in sys.argv[3:]] + [os.path.dirname(os.path.abspath(conf)), confdir]
    names = config_names(os.path.join(confdir, 'DEFAULTS'), dirs)
    names = config_names(conf, dirs, names)
    # Generated from kern/vnode_if.src by the kernel Makefile, not from a .m.
    compiled, interfaces = [], {'vnode_if.h', 'vnode_if_newproto.h', 'vnode_if_typedef.h'}
    for files in ('conf/files', 'conf/files.arm64'):
        for path, kind, words in file_entries(os.path.join(sysdir, files)):
            if not selected(kind, words, names):
                continue
            if path.endswith('.m'):
                interfaces.add(os.path.basename(path)[:-2] + '.h')
            elif path.endswith('.c'):
                compiled.append(path)
    problems = 0
    for path in compiled:
        full = os.path.join(sysdir, path)
        if not os.path.exists(full):
            continue
        for header, via in sorted(interface_includes(full, sysdir).items()):
            if header not in interfaces:
                print(f'{path}: needs {header} (via {via}), not generated')
                problems += 1
    print(f'check_config: {len(names)} names, {len(compiled)} C files, '
          f'{len(interfaces)} interfaces, {problems} missing interface headers')
    return 1 if problems else 0


sys.exit(main())
