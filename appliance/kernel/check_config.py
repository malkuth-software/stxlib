#!/usr/bin/env python3
"""check_config.py <sys dir> <kernel config> [<extra include dir>]

Static pre-build check of a kernel configuration, for configurations cut
down from GENERIC. The architecture comes from the configuration's own
lines: a "machine" line, else a "cpu" line (ARM64: arm64, HAMMER: amd64),
else an ident ending in the architecture's name (STXLIB-AMD64); arm64 when
it names none. Its includes decide nothing (every architecture has a
GENERIC). It evaluates the configuration as
config(8) would (include, files, device, options, nodevice, nooptions;
DEFAULTS first), selects the source files sys/conf/files, files.<machine>
and any "files" lines would compile, and reports every
compiled C file that includes a generated "<name>_if.h" whose .m source is not
selected: the failure mode of a missing clknode_if.h. It does not find
undeclared functions or missing link symbols; a clean result is not a build.
Includes under #if are followed too, so it over-reports: compare a cut against
its baseline (STXLIB-GENERIC) and read only the lines the cut adds.
Python 3.9; reads only.
"""
# The architectures this check knows, and the cpu that names each in its
# GENERIC (sys/<arch>/conf/GENERIC at fb9033d2876d).
ARCHITECTURES = (('arm64', 'ARM64'), ('amd64', 'HAMMER'))
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


def config_directives(path, include_dirs, found=None):
    """The machine words, cpu names and files paths of a config and its
    includes; None when an include cannot be found under include_dirs."""
    found = {'machine': [], 'cpu': set(), 'files': []} if found is None else found
    for raw in open(path):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        words = line.split()
        word, rest = words[0], [w.strip('"') for w in words[1:]]
        if word == 'include' and rest:
            for d in include_dirs:
                candidate = os.path.join(d, rest[0])
                if os.path.exists(candidate):
                    if config_directives(candidate, include_dirs, found) is None:
                        return None
                    break
            else:
                return None
        elif word == 'machine' and rest and not found['machine']:
            found['machine'] = rest
        elif word == 'cpu' and rest:
            found['cpu'].add(rest[0])
        elif word == 'files' and rest:
            found['files'].append(rest[0])
    return found


def own_architecture(conf):
    """The architecture conf's own lines name (its includes are not read:
    every architecture has a GENERIC, so "include GENERIC" decides nothing):
    a machine line, else a cpu line, else an ident ending in an
    architecture's name (STXLIB-AMD64). None when it names none."""
    machine, cpus, ident = None, set(), None
    for raw in open(conf):
        line = raw.split('#', 1)[0].strip()
        words = line.split()
        if len(words) < 2:
            continue
        if words[0] == 'machine' and machine is None:
            machine = words[1].strip('"')
        elif words[0] == 'cpu':
            cpus.add(words[1])
        elif words[0] == 'ident':
            ident = words[1].strip('"').lower()
    if machine is not None:
        return machine
    for arch, cpu in ARCHITECTURES:
        if cpu in cpus:
            return arch
    for arch, _ in ARCHITECTURES:
        if ident is not None and re.search(r'(^|[^a-z0-9])' + arch + r'$', ident):
            return arch
    return None


def architecture(sysdir, conf, extra_dirs):
    """The architecture of conf (own_architecture; arm64, the first
    appliance kernels' and this check's first architecture, when the
    configuration names none), its conf directory, include directories and
    directives. Refuses when the expansion's cpu lines name another
    architecture's cpu."""
    arch = own_architecture(conf) or 'arm64'
    cpu = dict(ARCHITECTURES).get(arch)
    if cpu is None:
        raise SystemExit(f'check_config: architecture {arch} of {conf} is not one this check knows')
    confdir = os.path.join(sysdir, arch, 'conf')
    defaults = os.path.join(confdir, 'DEFAULTS')
    if not os.path.isfile(defaults):
        raise SystemExit(f'check_config: no {defaults}')
    dirs = extra_dirs + [os.path.dirname(os.path.abspath(conf)), confdir]
    found = config_directives(defaults, dirs)
    own = config_directives(conf, dirs)
    if found is None or own is None:
        raise SystemExit(f'check_config: an include of {conf} is not found for {arch}')
    others = {c for a, c in ARCHITECTURES if a != arch}
    if own['cpu'] & others:
        raise SystemExit(f'check_config: {conf} names cpu {sorted(own["cpu"] & others)}, not {arch}\'s {cpu}')
    found['files'] += own['files']
    if not found['machine']:
        found['machine'] = own['machine'] or [arch]
    return arch, confdir, dirs, found


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


def interface_includes(path, sysdir, arch, stack=()):
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
        machine = (os.path.join(sysdir, arch, 'include', name[8:])
                   if name.startswith('machine/') else None)
        for candidate in (os.path.join(os.path.dirname(path), name) if m.group(1) == '"' else None,
                          machine, os.path.join(sysdir, name)):
            if candidate and os.path.isfile(candidate):
                for header, via in interface_includes(candidate, sysdir, arch, stack + (path,)).items():
                    found.setdefault(header, via)
                break
    return found


def main():
    sysdir, conf = sys.argv[1], sys.argv[2]
    arch, confdir, dirs, directives = architecture(sysdir, conf, [d for d in sys.argv[3:]])
    names = config_names(os.path.join(confdir, 'DEFAULTS'), dirs)
    names = config_names(conf, dirs, names)
    # Generated from kern/vnode_if.src by the kernel Makefile, not from a .m.
    compiled, interfaces = [], {'vnode_if.h', 'vnode_if_newproto.h', 'vnode_if_typedef.h'}
    # config(8): files, files.<machine>, files.<machine arch> when the
    # machine line names one, then each "files" line (relative to the conf
    # directory).
    lists = [os.path.join(sysdir, 'conf', 'files')]
    lists += [os.path.join(sysdir, 'conf', 'files.' + word) for word in directives['machine'][:2]]
    lists += [os.path.join(confdir, path) for path in directives['files']]
    # x86's shared list, for amd64 even when its DEFAULTS does not name it
    # (a duplicate is read once).
    if arch == 'amd64':
        lists.append(os.path.join(sysdir, 'conf', 'files.x86'))
    lists = [os.path.normpath(os.path.abspath(path)) for path in lists]
    seen = set()
    for files in lists:
        if files in seen or not os.path.isfile(files):
            continue
        seen.add(files)
        for path, kind, words in file_entries(files):
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
        for header, via in sorted(interface_includes(full, sysdir, arch).items()):
            if header not in interfaces:
                print(f'{path}: needs {header} (via {via}), not generated')
                problems += 1
    print(f'check_config: arch {arch}, {len(names)} names, {len(compiled)} C files, '
          f'{len(interfaces)} interfaces, {problems} missing interface headers')
    return 1 if problems else 0


sys.exit(main())
