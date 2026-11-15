#!/usr/bin/env python3
"""b1_records.py: B1's per-function distance records (B1.md section 5.5),
the summary table and the tool-gap list, generated from the census and the
runs; never edited by hand.

Inputs:
  --rows DIR       proofs/b1/rows (row.json per row: title, harnesses)
  --census DIR     census.py's --out/census directory (NN.json per row)
  --census-log F   instead of --census: census.py's summary lines as m4sbrun
                   logged them (per-function counts only; no lines, no
                   statuses beside the counted ones)
  --runs F         run_b1.py's runs.jsonl (phase B); repeatable (the M4 and
                   VM steps append to the same file, but a copy may be split)
  --closures F     optional JSON of per-function closing changes that
                   override the catalogue: {"<function>@<place>": {"<column>:
                   <construct prefix>": {"owner": ..., "change": ...}}}
  --out DIR        writes DIR/B1-distance/<row>.md (with --census),
                   DIR/B1-distance/summary.md and DIR/B1-distance/tool-gaps.md

Counting rules (B1.md 5.5): a cell is the number of entries with status
"distance"; entries permitted by house style, advisory entries and entries
recorded as not decidable from the AST are listed beside the cell and not
counted; cpp-verify's column is counted on the tool side. A row is
zero-annotation proved for a (tool, configuration) when its original is
proved at the headline size, every mutant is refuted, the probe is reached
(refuted) and the lifter counted no annotation.
"""
import argparse
import collections
import json
import os
import re
import sys

DISTANCE = 'distance'
COLUMNS = ('clight', 'strictc', 'house-style')
TOOLS = ('cpp-verify', 'esbmc', 'cbmc', 'genmc-vm')

# Closing change per construct (B1.md 5.5): owner code, tool or none, and
# the change. Proposed by sb3; a per-function entry in --closures wins.
CATALOGUE = {
    'C5a': ('code', 'hoist the assignment, increment or compound assignment into its own statement; '
                    'cost: one statement and, where the value is reused, one named local'),
    'C5b': ('code', 'bind the nested call\'s result to a named const local before the expression that '
                    'uses it; cost: one declaration per call'),
    'C8': ('code', 'report the failure as a result value (the house-style result type) instead of '
                   'throwing; a foreign exception (row 24) is owner none'),
    'C14 goto': ('code', 'structured loop or early return'),
    'C14 switch fall-through': ('code', 'an explicit break per case, shared code in a named function'),
    'C14 variadic': ('code', 'fixed arity or a span of arguments'),
    'C6 function pointer': ('code', 'a direct call, or a closed set of callees chosen by an enum; '
                                    'owner none where calling the caller\'s function is the job'),
    'C15 a local address escapes': ('code', 'return by value, or keep the owner alive for the view\'s '
                                            'lifetime'),
    'C15 a local address passed': ('none', 'recorded: escapes only if the callee keeps it; the callee\'s '
                                           'contract decides'),
    'C11 floating-point environment': ('code', 'restore the environment before returning (feholdexcept '
                                               'with feupdateenv in one scope)'),
    'C19 an input that crosses': ('code', 'take the input as an explicitly mutable type'),
    # cpp-verify 0.1.1 subset (tool side); the code alternative is stated too.
    'cpp-verify C8': ('tool', 'exception support in cpp-verify (fail-closed in 0.1.1); code: a result value'),
    'cpp-verify C11': ('tool', 'floating point in cpp-verify (fail-closed in 0.1.1); house style permits it'),
    'cpp-verify C18 member function': ('tool', 'member function contracts in cpp-verify; code: a free '
                                               'function taking the object'),
    'cpp-verify C18 struct return': ('tool', 'aggregate (struct) types in cpp-verify, as its message "unsupported '
                                             'C++ type in verification" names them'),
    'cpp-verify C18 lambda': ('tool', 'lambdas in cpp-verify; code: a named function'),
    'cpp-verify C9': ('tool', 'templates in cpp-verify; code: a non-template function at the proof '
                              'boundary'),
    'cpp-verify C6': ('tool', 'virtual dispatch in cpp-verify'),
    'cpp-verify C14 switch': ('tool', 'switch in cpp-verify; code: an if chain'),
    'cpp-verify C1': ('tool', 'range-for in cpp-verify; code: an index loop'),
    'cpp-verify C7': ('tool', 'standard-container models in cpp-verify'),
}


def closing(column, construct, function_key, closures, census=None):
    override = (closures.get(function_key) or {})
    for prefix, value in override.items():
        if f'{column}:{construct}'.startswith(prefix):
            return value['owner'], value['change']
    if construct.startswith('C8') and column != 'cpp-verify' and census and not census['C8']['throw']:
        return 'code', ('use a primitive that reports failure without throwing: the function throws nothing '
                        'itself and catches what the standard library throws')
    key = construct if column != 'cpp-verify' else f'cpp-verify {construct}'
    best = max((candidate for candidate in CATALOGUE if key.startswith(candidate)), key=len, default=None)
    if best is None:
        return 'open', 'no catalogue entry: to be written'
    return CATALOGUE[best]


def load_json(path):
    with open(path, 'r', encoding='utf-8') as handle:
        return json.load(handle)


def load_rows(directory):
    rows = {}
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name, 'row.json')
        if os.path.isfile(path):
            row = load_json(path)
            rows[row['row']] = row
    return rows


def load_runs(paths):
    runs = []
    for path in paths or []:
        with open(path, 'r', encoding='utf-8') as handle:
            runs.extend(json.loads(line) for line in handle if line.strip())
    return runs


def from_census(directory, rows):
    """Per row: functions with their census and gap lists."""
    result = {}
    for row in rows:
        path = os.path.join(directory, f'{row}.json')
        if os.path.isfile(path):
            result[row] = load_json(path)
    return result


SUMMARY = re.compile(r'(.+?)@(\S+) \[distance: clight (\d+), strictc (\d+), house-style (\d+); '
                     r'cpp-verify tool gaps (\d+)\]')


def from_census_log(path):
    """Per row: functions with counts only (census.py's summary lines)."""
    result = {}
    with open(path, 'r', encoding='utf-8') as handle:
        for text in handle:
            match = re.match(r'^(\d\d)  (.*)$', text.rstrip('\n'))
            if not match:
                continue
            functions = {}
            for part in re.split(r'; (?=stx)', match.group(2)):
                entry = SUMMARY.fullmatch(part.strip())
                if not entry:
                    sys.exit(f'unparsed census line in row {match.group(1)}: {part[:100]}')
                name, place, clight, strictc, house, gaps = entry.groups()
                functions[f'{name}@{place}'] = {'counts': {'clight': int(clight), 'strictc': int(strictc),
                                                           'house-style': int(house),
                                                           'cpp-verify': int(gaps)}}
            result[match.group(1)] = {'row': match.group(1), 'functions': functions}
    return result


def counts(entry):
    if 'counts' in entry:
        return entry['counts'], None
    outside = entry['outside']
    counted = {column: sum(1 for gap in outside[column] if gap['status'] == DISTANCE) for column in COLUMNS}
    counted['cpp-verify'] = len(outside['cpp-verify'])
    beside = collections.Counter(gap['status'] for column in COLUMNS for gap in outside[column]
                                 if gap['status'] != DISTANCE)
    return counted, beside




def void_reason(run, voids):
    """A run is void when a rule names its row and its lift lacks the span
    the rule requires (row 21 before kernel_constants was lifted)."""
    for rule in voids:
        if run['row'] != rule['row']:
            continue
        spans = (run.get('lift') or {}).get('spans', [])
        if not any(span.get('path') == rule['unless_span'] for span in spans):
            return rule['reason']
    return None


def row_outcomes(runs, rows, voids=()):
    """Per (row, tool, config, configuration): B1.md 5.4 measures 1, 5, 6."""
    table = collections.defaultdict(lambda: {'original': {}, 'mutants': {}, 'probe': None, 'unbounded': None,
                                             'annotations': 0, 'time_s': 0.0, 'peak_mib': 0,
                                             'unsupported': [], 'harness': None, 'abstractions': []})
    for run in runs:
        reason = void_reason(run, voids)
        config = run['config'] + (' (void)' if reason else '')
        key = (run['row'], run['tool'], config, run.get('configuration', 'default'))
        cell = table[key]
        cell['void'] = reason
        result = run.get('result')
        if run.get('mode') == 'unbounded':
            cell['unbounded'] = result
        elif run['variant'] == 'original':
            cell['original'][run.get('size')] = result
        elif run['variant'] == 'mutant':
            cell['mutants'][run.get('mutant')] = result
        elif run['variant'] == 'probe':
            cell['probe'] = result
        cell['annotations'] = max(cell['annotations'], (run.get('lift') or {}).get('annotations', 0))
        cell['harness'] = os.path.basename(run.get('harness') or '') or cell['harness']
        if not cell['abstractions']:
            cell['abstractions'] = (run.get('lift') or {}).get('abstractions', [])
        cell['time_s'] += run.get('time_s') or 0.0
        cell['peak_mib'] = max(cell['peak_mib'], run.get('peak_memory_mib') or 0)
        if result == 'unsupported':
            cell['unsupported'].append(first_error(run))
    for (row, tool, config, configuration), cell in table.items():
        harness = next((h for h in rows[row]['harnesses'] if h['file'] == cell['harness']), None) or \
            next((h for h in rows[row]['harnesses'] if tool in h['tools']), {})
        cell['properties'] = harness.get('properties')
        sizes = harness.get('sizes')
        headline = harness.get('headline')
        if sizes and headline is None:
            headline = max(size for size in sizes if size <= 16) if min(sizes) <= 16 else min(sizes)
        cell['headline'] = headline
        cell['at_headline'] = cell['original'].get(headline)
        proved = [size for size, result in cell['original'].items() if result == 'proved' and size is not None]
        cell['largest_proved'] = max(proved) if proved else None
        cell['mutants_refuted'] = bool(cell['mutants']) and all(r == 'refuted' for r in cell['mutants'].values())
        if not harness.get('mutants') and harness:
            cell['mutants_refuted'] = None  # no mutant: no control-flow property beyond P0 (bound-effect rows)
        cell['probe_reached'] = cell['probe'] == 'refuted'
        cell['zero_annotation'] = (not cell.get('void') and cell['at_headline'] == 'proved' and cell['mutants_refuted']
                                   and cell['probe_reached'] and cell['annotations'] == 0)
    return table


def first_error(run):
    for line in (run.get('output_head') or []) + (run.get('output_tail') or []):
        match = re.search(r'(error: .*|parse error.*|unsupported.*)', line)
        if match:
            text = re.sub(r'^.*?(?=error:|parse error|unsupported)', '', match.group(1)).strip()
            return re.sub(r'\s+', ' ', text)[:160]
    return 'no error line in the recorded output'


def yes(flag):
    return 'n/a' if flag is None else 'yes' if flag else 'no'


def write_row(directory, row, row_census, outcomes, closures):
    lines = [f'# B1 distance record: row {row["row"]}, {row["title"]}', '',
             'Generated by `b1_records.py` from census.py and run_b1.py output; never edited by hand.', '']
    if 'error' in row_census:
        lines += [f'Census error: {row_census["error"]}', '']
    errors = row_census.get('clang_errors')
    if errors:
        lines += [f'Clang errors: {errors["harness_lines"]} on harness lines; on lifted lines: '
                  f'{", ".join(errors["lifted_lines"]) or "none"}.', '']
    for name, entry in row_census.get('functions', {}).items():
        if name == '<namespace scope>' and not any(entry['outside'].values()):
            continue
        lines += [f'## `{name}`', '', '### Census (B1.md 5.3; non-zero entries)', '',
                  '| # | Counts | Lines |', '|---|---|---|']
        for construct, fields in entry['census'].items():
            shown = {k: v for k, v in fields.items() if v and not k.startswith('lines') and k != 'escaping_lines'
                     and k != 'environment_lines' and k != 'read_only_lines'}
            places = sorted({str(line) for k, v in fields.items() if k.endswith('lines') or k.startswith('lines')
                             for line in (v or [])})
            if construct == 'C4' and not (fields['alias_pairs'] or fields['out_parameters']):
                shown = {}
            if shown:
                rendered = '; '.join(f'{k} {json.dumps(v) if not isinstance(v, int) else v}' for k, v in shown.items())
                lines.append(f'| {construct} | {rendered} | {", ".join(places) or "-"} |')
        lines += ['', '### Outside each column', '', '| Column | Construct | Status | Owner | Closing change |',
                  '|---|---|---|---|---|']
        for column in COLUMNS + ('cpp-verify',):
            for gap in entry['outside'][column]:
                if gap['status'] in (DISTANCE, 'tool gap') or gap['status'].startswith('recorded'):
                    owner, change = closing(column, gap['construct'], name, closures, entry['census'])
                else:
                    owner, change = '-', 'none needed (not counted)'
                lines.append(f'| {column} | {gap["construct"]} | {gap["status"]} | {owner} | {change} |')
        lines.append('')
    lines += ['## Tool side (B1.md 5.4)', '']
    cells = {key: cell for key, cell in outcomes.items() if key[0] == row['row']}
    if not cells:
        lines += ['No phase B runs yet.', '']
    else:
        lines += ['| Tool | Config | Configuration | At headline | Largest N proved | Unbounded | Mutants refuted '
                  '| Probe reached | Annotations | Zero-annotation proved | Time (s) | Peak (MiB) |',
                  '|---|---|---|---|---|---|---|---|---|---|---|---|']
        for (_, tool, config, configuration), cell in sorted(cells.items()):
            headline_text = f'{cell["void"].split(" (")[0]} (was {cell["at_headline"]})' if cell.get('void') \
                else cell['at_headline']
            lines.append(f'| {tool} | {config} | {configuration} | {headline_text} | '
                         f'{cell["largest_proved"] or "-"} | {cell["unbounded"] or "-"} | '
                         f'{yes(cell["mutants_refuted"])} | {yes(cell["probe_reached"])} | {cell["annotations"]} | '
                         f'{yes(cell["zero_annotation"])} | {cell["time_s"]:.1f} | {cell["peak_mib"]} |')
        effects = [cell for key, cell in sorted(cells.items()) if key[2] == 'bound-effect']
        if effects:
            cell = effects[0]
            lines += ['', f'Bound-effect configuration (owner direction of 2026-11-13): properties '
                          f'{", ".join(cell["properties"] or ["-"])}; each arithmetic result below is abstracted to '
                          f'its bound, which is a numeric obligation established outside the run:', '',
                      '| Line | Abstracted | Bound |', '|---|---|---|']
            lines += [f'| {item["path"]}:{item["line"]} | `{item["from"]}` | {item.get("bound", "")} |'
                      for item in cell['abstractions']]
        voided = sorted({cell['void'] for cell in cells.values() if cell.get('void')})
        if voided:
            lines += [''] + [f'Void runs: {reason}.' for reason in voided]
        rejected = sorted({(key[1], text) for key, cell in cells.items() for text in cell['unsupported']
                           if not cell.get('void')})
        if rejected:
            lines += ['', 'Rejected, as the tool names it:', '']
            lines += [f'- {tool}: `{text}`' for tool, text in rejected]
        lines.append('')
    with open(os.path.join(directory, f'{row["row"]}.md'), 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(lines))


def write_summary(directory, rows, census, outcomes, source):
    seen = {}
    for row_name, row_census in sorted(census.items()):
        for name, entry in row_census.get('functions', {}).items():
            if name.startswith('<namespace'):
                continue
            counted, beside = counts(entry)
            record = seen.setdefault(name, {'rows': [], 'counts': counted, 'beside': beside})
            record['rows'].append(row_name)
    functions = len(seen)
    totals = {column: (sum(1 for r in seen.values() if r['counts'][column]),
                       sum(r['counts'][column] for r in seen.values())) for column in COLUMNS + ('cpp-verify',)}
    clean = sum(1 for r in seen.values() if not any(r['counts'][c] for c in COLUMNS))
    lines = ['# B1 distance summary', '',
             f'Generated by `b1_records.py` from {source}; never edited by hand. Each cell is the number of '
             'entries with status "distance"; cpp-verify\'s column counts tool gaps (tool side).', '',
             f'Distinct functions: {functions}. With no source-side distance: {clean}.', '',
             '| Column | Functions | Entries |', '|---|---|---|']
    lines += [f'| {column} | {f} | {e} |' for column, (f, e) in totals.items()]
    lines += ['', '## Per function', '', '| Function | Rows | Clight | StrictC | House style | Beside (not counted) '
              '| cpp-verify gaps |', '|---|---|---|---|---|---|---|']
    for name, record in sorted(seen.items(), key=lambda item: (item[1]['rows'][0], item[0])):
        beside = ', '.join(f'{k} {v}' for k, v in sorted(record['beside'].items())) if record['beside'] else \
            ('-' if record['beside'] is not None else 'needs census.json')
        c = record['counts']
        lines.append(f'| `{name}` | {" ".join(record["rows"])} | {c["clight"]} | {c["strictc"]} | '
                     f'{c["house-style"]} | {beside} | {c["cpp-verify"]} |')
    lines += ['', '## Zero-annotation proved, per tool (B1.md 5.4 measure 5)', '']
    if not outcomes:
        lines += ['No phase B runs yet.']
    else:
        per_tool = collections.defaultdict(dict)
        for (row, tool, config, configuration), cell in outcomes.items():
            if config.startswith('bound-effect') or cell.get('void'):
                continue  # a different property set, or void: not counted
            previous = per_tool[tool].get(row, False)
            per_tool[tool][row] = previous or cell['zero_annotation']
        lines += ['| Tool | Rows run | Zero-annotation proved | Of the 24 rows |', '|---|---|---|---|']
        for tool in TOOLS:
            if tool in per_tool:
                proved = sum(per_tool[tool].values())
                lines.append(f'| {tool} | {len(per_tool[tool])} | {proved} | {proved}/24 |')
        lines += ['', 'A row counts for a tool when any of its configurations meets the rule in the header.']
    with open(os.path.join(directory, 'summary.md'), 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(lines) + '\n')


def write_tool_gaps(directory, census, outcomes):
    lines = ['# B1 tool-gap list', '', 'Generated by `b1_records.py`; never edited by hand.', '',
             '## cpp-verify 0.1.1 subset, from the census (constructs outside it)', '']
    by_construct = collections.defaultdict(set)
    detailed = False
    for row_name, row_census in census.items():
        for name, entry in row_census.get('functions', {}).items():
            if 'outside' in entry:
                detailed = True
                for gap in entry['outside']['cpp-verify']:
                    by_construct[gap['construct']].add(name)
    if not detailed:
        lines += ['Needs census.json (the summary lines give counts only; see summary.md).']
    else:
        lines += ['| Construct | Functions | Owner | Closing change |', '|---|---|---|---|']
        for construct, names in sorted(by_construct.items(), key=lambda item: -len(item[1])):
            owner, change = closing('cpp-verify', construct, '', {})
            lines.append(f'| {construct} | {len(names)} | {owner} | {change} |')
    lines += ['', '## Rejections in the runs, as each tool names them', '']
    rejected = collections.defaultdict(lambda: collections.defaultdict(set))
    for (row, tool, config, configuration), cell in outcomes.items():
        if cell.get('void'):
            continue
        for text in cell['unsupported']:
            rejected[tool][text].add(row)
    if not rejected:
        lines += ['No rejections.' if outcomes else 'No phase B runs yet.']
    for tool in sorted(rejected):
        lines += [f'### {tool}', '', '| Message | Rows |', '|---|---|']
        for text, rows in sorted(rejected[tool].items(), key=lambda item: sorted(item[1])):
            lines.append(f'| `{text}` | {" ".join(sorted(rows))} |')
        lines.append('')
    with open(os.path.join(directory, 'tool-gaps.md'), 'w', encoding='utf-8') as handle:
        handle.write('\n'.join(lines) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--rows', required=True)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--census')
    group.add_argument('--census-log')
    parser.add_argument('--runs', action='append')
    parser.add_argument('--closures')
    parser.add_argument('--out', required=True)
    arguments = parser.parse_args()
    rows = load_rows(arguments.rows)
    if arguments.census:
        census, source = from_census(arguments.census, rows), 'census.json per row'
    else:
        census, source = from_census_log(arguments.census_log), 'census.py\'s summary lines (counts only)'
    closures = load_json(arguments.closures) if arguments.closures else {}
    outcomes = row_outcomes(load_runs(arguments.runs), rows, closures.get('_voids', []))
    directory = os.path.join(arguments.out, 'B1-distance')
    os.makedirs(directory, exist_ok=True)
    if arguments.census:
        for name, row in rows.items():
            write_row(directory, row, census.get(name, {'error': 'no census record'}), outcomes, closures)
    write_summary(directory, rows, census, outcomes, source)
    write_tool_gaps(directory, census, outcomes)
    return 0


if __name__ == '__main__':
    sys.exit(main())
