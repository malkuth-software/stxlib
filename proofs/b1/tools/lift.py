#!/usr/bin/env python3
"""lift.py: build one harness translation unit from a harness file and the
pinned sources it lifts code from.

A harness names the code under test with directives in comment lines, so
the code is copied verbatim from its pinned source at run time and never
retyped (B1.md section 4.3):

  //@ lift source=S path=P lines=A-B|all [namespace=N] [anonymous] sha256=H
      Copies lines A..B of P under source S, checks their SHA-256 (of the
      lines joined by newlines, with a final newline), and wraps them in
      `namespace N {` and `namespace {` when asked. #line directives keep
      every diagnostic pointing at the original file and line.
  //@ pin source=S path=P sha256=H [installed=K:Q]
      Checks a whole file the harness includes rather than lifts (a header
      used as is); with `installed`, also checks that the installed copy Q
      under sources key K is byte-identical (for libc++, whose installed
      headers are what a harness includes).
  //@ subst source=S path=P line=L from="X" to="Y" reason="..."
      An environment model: replaces the single occurrence of X on lifted
      line L by Y in every variant. Recorded in the report.
  //@ contract source=S path=P line=L from="X" to="Y"
      Like subst, but attaches the row's property as a contract (cpp-verify
      files): the statement of the property, not an annotation.
  //@ annotate source=S path=P line=L from="X" to="Y"
      Like subst, but counted as an annotation (an invariant, assumption or
      contract a tool needs beyond the row's property and precondition):
      B1.md section 5.3, annotations beyond the contract.
  //@ annotation-begin / //@ annotation-end
      Harness lines between them are counted as annotation lines.
  //@ model-begin reason="..." / //@ model-end
      Harness lines between them are an environment model written in the
      harness (a host mutex over pthreads, a modelled allocator backend);
      counted and listed with their reason in the report.
  //@ mutant id=M source=S path=P line=L from="X" to="Y"
      Applied only when --mutant M is given (B1.md section 3.3).
  //@ abstract source=S path=P line=L from="X" to="Y" bound="..." reason="..."
      A bounded effect (owner direction of 2026-11-13, "bound effect"):
      replaces an arithmetic result X by Y, any value within its stated
      bound, so the harness verifies control flow with the arithmetic
      abstracted. Recorded in the report with its bound; each bound is a
      numeric obligation established separately, not by this run.

Sources are named in a JSON file (see ../sources.example.json):
  {"stx_auxlib": {"root": "...", "commit": "08c8132..."},
   "libcxx": {"root": ".../llvm-project-22.1.8.src/libcxx"},
   "libcxx_include": {"root": ".../include/c++/v1"}}
A source with "commit" is a git tree whose HEAD must start with it.

Usage:
  lift.py --sources S.json --harness H --out OUT.cpp [--mutant M] [--report R.json]
  lift.py --sources S.json --harness H --fill-hashes   (writes missing or
      TODO hashes into H; for the seat writing harnesses, never in a run)

Exit status: 0 on success; 2 on a usage or directive error; 3 on a hash or
pin mismatch (the pinned source is not the one the harness was written for).
"""

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

DIRECTIVE = re.compile(r'^\s*//@\s*([a-z-]+)\b(.*)$')


class LiftError(Exception):
    def __init__(self, message, status=2):
        super().__init__(message)
        self.status = status


def parse_fields(text):
    fields = {}
    for token in shlex.split(text):
        if '=' in token:
            key, value = token.split('=', 1)
            fields[key] = value
        else:
            fields[token] = True
    return fields


def sha256_of_lines(lines):
    joined = ''.join(line + '\n' for line in lines)
    return hashlib.sha256(joined.encode('utf-8', 'surrogateescape')).hexdigest()


def sha256_of_file(path):
    with open(path, 'rb') as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def read_lines(path):
    with open(path, 'r', encoding='utf-8', errors='surrogateescape', newline='') as handle:
        text = handle.read()
    lines = text.split('\n')
    if lines and lines[-1] == '':
        lines.pop()
    return [line[:-1] if line.endswith('\r') else line for line in lines]


class Sources:
    def __init__(self, path):
        with open(path, 'r', encoding='utf-8') as handle:
            self.table = json.load(handle)
        self.checked = set()

    def root(self, name):
        if name not in self.table:
            raise LiftError(f'source "{name}" is not in the sources file')
        entry = self.table[name]
        root = entry['root']
        if name not in self.checked and 'commit' in entry:
            head = subprocess.run(['git', '-C', root, 'rev-parse', 'HEAD'], capture_output=True,
                                  text=True, check=False).stdout.strip()
            if not head.startswith(entry['commit']):
                raise LiftError(f'source "{name}" is at {head or "?"}, not {entry["commit"]}', 3)
        self.checked.add(name)
        return root

    def file(self, name, relative):
        return os.path.join(self.root(name), relative)


def parse_range(text, count):
    if text == 'all':
        return 1, count
    first, last = text.split('-', 1)
    first_line, last_line = int(first), int(last)
    if first_line < 1 or last_line < first_line or last_line > count:
        raise LiftError(f'line range {text} is outside 1-{count}')
    return first_line, last_line


def lift(sources, harness_path, mutant=None, fill_hashes=False, apply_edits=True):
    harness = read_lines(harness_path)
    directives = []
    for number, line in enumerate(harness, start=1):
        match = DIRECTIVE.match(line)
        if match:
            directives.append((number, match.group(1), parse_fields(match.group(2))))

    spans = {}          # harness line -> span record
    edits = []          # (kind, fields, harness line)
    pins = []
    annotation_lines = 0
    filled = {}
    for number, kind, fields in directives:
        if kind == 'lift':
            path = sources.file(fields['source'], fields['path'])
            lines = read_lines(path)
            first, last = parse_range(fields['lines'], len(lines))
            digest = sha256_of_lines(lines[first - 1:last])
            expected = fields.get('sha256', 'TODO')
            if fill_hashes and expected in ('TODO', True):
                filled[number] = digest
            elif expected != digest:
                raise LiftError(f'{harness_path}:{number}: {fields["source"]}:{fields["path"]}:'
                                f'{fields["lines"]} hashes to {digest}, the harness expects {expected}', 3)
            spans[number] = {'source': fields['source'], 'path': fields['path'], 'first': first,
                             'last': last, 'lines': list(lines[first - 1:last]), 'sha256': digest,
                             'namespace': fields.get('namespace'), 'anonymous': 'anonymous' in fields}
        elif kind == 'pin':
            path = sources.file(fields['source'], fields['path'])
            digest = sha256_of_file(path)
            expected = fields.get('sha256', 'TODO')
            if fill_hashes and expected in ('TODO', True):
                filled[number] = digest
            elif expected != digest:
                raise LiftError(f'{harness_path}:{number}: pinned file {fields["path"]} hashes to {digest}', 3)
            if 'installed' in fields:
                key, relative = fields['installed'].split(':', 1)
                if key in sources.table:
                    installed = sources.file(key, relative)
                    if sha256_of_file(installed) != digest:
                        raise LiftError(f'{harness_path}:{number}: installed {installed} differs from the pinned source', 3)
            pins.append({'source': fields['source'], 'path': fields['path'], 'sha256': digest})
        elif kind in ('subst', 'annotate', 'mutant', 'contract', 'abstract'):
            edits.append((kind, fields, number))
        elif kind in ('annotation-begin', 'annotation-end', 'model-begin', 'model-end'):
            pass
        else:
            raise LiftError(f'{harness_path}:{number}: unknown directive "{kind}"')
    inside = False
    models = []
    for line in harness:
        match = DIRECTIVE.match(line)
        if match and match.group(1) == 'annotation-begin':
            inside = True
            continue
        if match and match.group(1) == 'annotation-end':
            inside = False
            continue
        if match and match.group(1) == 'model-begin':
            models.append({'reason': parse_fields(match.group(2)).get('reason', ''), 'lines': 0})
            continue
        if match and match.group(1) == 'model-end':
            models.append(None)
            continue
        if inside and line.strip():
            annotation_lines += 1
        if models and models[-1] is not None and line.strip():
            models[-1]['lines'] += 1
    models = [model for model in models if model is not None]

    if fill_hashes:
        if filled:
            out = list(harness)
            for number, digest in filled.items():
                text = out[number - 1]
                if 'sha256=' in text:
                    out[number - 1] = re.sub(r'sha256=\S+', f'sha256={digest}', text)
                else:
                    out[number - 1] = text + f' sha256={digest}'
            with open(harness_path, 'w', encoding='utf-8', newline='\n') as handle:
                handle.write(''.join(line + '\n' for line in out))
        return None, {'filled': len(filled)}

    applied = []
    mutant_found = mutant is None
    for kind, fields, number in edits:
        if kind == 'mutant' and fields.get('id') != mutant:
            continue
        if not apply_edits:
            continue
        target = int(fields['line'])
        span = None
        for record in spans.values():
            if (record['source'] == fields['source'] and record['path'] == fields['path']
                    and record['first'] <= target <= record['last']):
                span = record
                break
        if span is None:
            raise LiftError(f'{harness_path}:{number}: line {target} of {fields["path"]} is not lifted')
        index = target - span['first']
        text = span['lines'][index]
        if text.count(fields['from']) != 1:
            raise LiftError(f'{harness_path}:{number}: "{fields["from"]}" occurs '
                            f'{text.count(fields["from"])} times on {fields["path"]}:{target}')
        span['lines'][index] = text.replace(fields['from'], fields['to'])
        applied.append({'kind': kind, 'path': fields['path'], 'line': target, 'from': fields['from'],
                        'to': fields['to'], 'reason': fields.get('reason', '')})
        if kind == 'abstract':
            applied[-1]['bound'] = fields.get('bound', '')
        if kind == 'mutant':
            mutant_found = True
    if not mutant_found:
        raise LiftError(f'{harness_path}: no mutant "{mutant}"')

    label = os.path.relpath(harness_path)
    output = [f'#line 1 "{label}"']
    for number, line in enumerate(harness, start=1):
        if number not in spans:
            output.append(line)
            continue
        record = spans[number]
        output.append(f'// lifted {record["source"]}:{record["path"]}:{record["first"]}-{record["last"]} '
                      f'sha256={record["sha256"]}')
        closers = 0
        if record['namespace']:
            output.append(f'namespace {record["namespace"]} {{')
            closers += 1
        if record['anonymous']:
            output.append('namespace {')
            closers += 1
        output.append(f'#line {record["first"]} "{record["source"]}/{record["path"]}"')
        output.extend(record['lines'])
        output.extend(['}'] * closers)
        output.append(f'#line {number + 1} "{label}"')
    report = {
        'harness': label,
        'mutant': mutant,
        'spans': [{key: record[key] for key in ('source', 'path', 'first', 'last', 'sha256')}
                  for record in spans.values()],
        'pins': pins,
        'edits': applied,
        'annotations': sum(1 for item in applied if item['kind'] == 'annotate') + annotation_lines,
        'contracts': sum(1 for item in applied if item['kind'] == 'contract'),
        'environment_models': [item for item in applied if item['kind'] == 'subst'],
        'abstractions': [item for item in applied if item['kind'] == 'abstract'],
        'harness_models': models,
    }
    return '\n'.join(output) + '\n', report


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--sources', required=True)
    parser.add_argument('--harness', required=True)
    parser.add_argument('--out')
    parser.add_argument('--mutant')
    parser.add_argument('--report')
    parser.add_argument('--fill-hashes', action='store_true')
    arguments = parser.parse_args()
    try:
        sources = Sources(arguments.sources)
        text, report = lift(sources, arguments.harness, arguments.mutant, arguments.fill_hashes)
    except LiftError as error:
        print(f'lift.py: {error}', file=sys.stderr)
        return error.status
    except (OSError, KeyError, ValueError) as error:
        print(f'lift.py: {error}', file=sys.stderr)
        return 2
    if arguments.fill_hashes:
        print(f'lift.py: {report["filled"]} hashes written to {arguments.harness}')
        return 0
    if not arguments.out:
        print('lift.py: --out is required', file=sys.stderr)
        return 2
    with open(arguments.out, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(text)
    if arguments.report:
        with open(arguments.report, 'w', encoding='utf-8') as handle:
            json.dump(report, handle, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
