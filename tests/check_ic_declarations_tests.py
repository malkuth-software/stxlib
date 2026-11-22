#!/usr/bin/env python3
"""Tests of STXlib's IC declarations (tools/ic/project.toml) and its use of tools/ic_declarations.py.

Usage: python3 -I tests/check_ic_declarations_tests.py <repository root>

The tool is template_harness's harness/ic_declarations.py, kept identical
here; its own suite is harness/test_ic_declarations.py there (the clause
language, the declaration, the outputs, the exit codes). These cases check
STXlib's declarations, phase IC1 of the IC study (notes, gui_forms branch,
planning/stages/IC-integrity-constraints/STUDY.md): the allocator, the
containers and the files.
Properties:
  I1  every declaration loads, every output it generates is current, and
      every generated predicate is called (the tool's check, exit 0);
  I2  the components IC1 covers are declared: stx_minmallocpp and memory
      (the allocator), Vector, Buffer, FrozenMap and FrozenTextMap (the
      containers), File and the whole-file operations (the files);
  I3  each requirement a declaration states is checked through its generated
      predicate: every STXLIB_REQUIRE in vector.hpp and buffer.hpp, and the
      positional requirements of frozen_map.cpp, call detail::ic::;
  I4  each requirement's message is spelled once, in its generated checks
      header: no source the declaration names repeats the literal, so the
      message and the condition cannot drift apart;
  I5  every refusal of the files component that is a clause (positioned
      offsets, set_size) is the generated predicate, and the old
      hand-written limit is gone from files.cpp;
  I6  the contract lines in the headers are the generator's: each declared
      call with something to declare has `// ic:` lines directly above its
      first declaration line (or above its template line), in its class;
  I7  the IC-full components are declared (AtomicPool, Thread, Mutex,
      GlyphStore, Plan64, Plan32) and their refusals that are clauses are
      the generated predicates in their sources;
  I8  every declaration has an [engine] table (IC2), every one drives at
      least one call, and each call it does not drive says why;
  I9  the deductions (IC4, tools/ic_deduce.py) are current, the allocator's
      four page-count lemmas are discharged, and a discharged obligation is
      recorded as such in its obligations file;
  I10 the deducer refutes what does not follow: a page-count lemma with a
      wrong conclusion, a subtraction no conjunct guards;
  I11 the gate file lists every declared component.
"""

import importlib.util
import io
import pathlib
import re
import sys
from contextlib import redirect_stdout

failures: list[str] = []
cases: int = 0


def check(condition: bool, label: str) -> None:
    global cases
    cases = cases + 1
    if not condition:
        failures.append(label)


def load_tool(root: pathlib.Path):
    path = root / 'tools' / 'ic_declarations.py'
    spec = importlib.util.spec_from_file_location('ic_declarations', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['ic_declarations'] = module
    spec.loader.exec_module(module)
    return module


def whole_check(tool, root: pathlib.Path) -> None:
    output = io.StringIO()
    with redirect_stdout(output):
        status = tool.run('check', root / 'tools' / 'ic' / 'project.toml', root)
    check(status == 0, 'I1 the declarations and their outputs are current: ' + output.getvalue().strip())


def declarations_of(tool, root: pathlib.Path) -> list:
    project = tool.load_project(root / 'tools' / 'ic' / 'project.toml', root)
    loaded = [tool.load_declaration(root, relative) for relative in tool.declaration_paths(project)]
    return loaded


def coverage(declarations: list) -> None:
    keys = {declaration.manifest_key() for declaration in declarations}
    wanted = ('stxlib::stx_minmallocpp', 'stxlib::memory', 'stxlib::Vector', 'stxlib::Buffer', 'stxlib::FrozenMap',
              'stxlib::FrozenTextMap', 'stxlib::File')
    for key in wanted:
        check(key in keys, 'I2 ' + key + ' is declared')
    files = next(declaration for declaration in declarations if declaration.manifest_key() == 'stxlib::File')
    free = {call.name for call in files.calls if call.kind == 'free'}
    check({'read_file', 'write_file', 'list_directory'} <= free, 'I2 the whole-file operations are declared')
    check(files.states == ('closed', 'reading', 'writing', 'appending'), 'I2 File declares its four states')


def requirements_checked(root: pathlib.Path) -> None:
    for relative in ('include/stxlib/vector.hpp', 'include/stxlib/buffer.hpp'):
        text = (root / relative).read_text(encoding='utf-8')
        sites = [line for line in text.split('\n') if 'STXLIB_REQUIRE(' in line]
        check(bool(sites), 'I3 ' + relative + ' has requirements')
        for line in sites:
            check('detail::ic::' in line, 'I3 ' + relative + ' checks through its declaration: ' + line.strip())
    frozen = (root / 'src' / 'algorithms' / 'frozen_map.cpp').read_text(encoding='utf-8')
    for name in ('FrozenMap::key_at', 'FrozenMap::value_at', 'FrozenTextMap::key_at', 'FrozenTextMap::value_at'):
        check(name + ': index out of range"' not in frozen, 'I3 ' + name + ' checks through its declaration')


def messages_once(tool, root: pathlib.Path, declarations: list) -> None:
    for declaration in declarations:
        if declaration.checks is None:
            continue
        sources = set(declaration.checks.used_in) | {declaration.header}
        texts = {path: (root / path).read_text(encoding='utf-8') for path in sources}
        checks_text = (root / declaration.checks.path).read_text(encoding='utf-8')
        for call in declaration.calls:
            for requirement in call.requires:
                if not requirement.message:
                    continue
                literal = '"' + requirement.message + '"'
                check(literal in checks_text, 'I4 ' + requirement.check + "'s message is in its checks header")
                for path, text in texts.items():
                    code = '\n'.join(line for line in text.split('\n')
                                     if not line.lstrip().startswith(tool.COMMENT_PREFIX))
                    check(literal not in code, 'I4 ' + path + ' does not repeat ' + literal)


def files_refusals(root: pathlib.Path) -> None:
    text = (root / 'src' / 'core' / 'files.cpp').read_text(encoding='utf-8')
    for name in ('admits_read_at_offset', 'admits_write_at_extent', 'admits_set_size_bytes'):
        check(re.search(r'detail::ic::files::' + name + r'\(', text) is not None, 'I5 files.cpp calls ' + name)
    check('largest_offset' not in text, 'I5 the hand-written largest_offset is gone from files.cpp')


def contract_lines(tool, root: pathlib.Path, declarations: list) -> None:
    for declaration in declarations:
        lines = (root / declaration.header).read_text(encoding='utf-8').split('\n')
        anchors = [index for index, line in enumerate(lines)
                   if declaration.anchor and line.strip().startswith(declaration.anchor)]
        start = anchors[0] if anchors else 0
        for call in declaration.calls:
            wanted = call.declarations[0].strip()
            found = [index for index, line in enumerate(lines) if index >= start and line.strip().startswith(wanted)]
            check(len(found) >= 1, 'I6 ' + declaration.header + ' declares ' + call.name)
            if not found:
                continue
            index = found[0]
            above = index - 1
            if above >= 0 and tool.TEMPLATE_LINE.match(lines[above]):
                above = above - 1
            # A call with nothing to declare gets no lines (the generator
            # writes none when call_sentences is empty).
            if not tool.call_sentences(declaration, call):
                continue
            check(above >= 0 and lines[above].lstrip().startswith(tool.COMMENT_PREFIX),
                  'I6 ' + declaration.manifest_key() + '::' + call.name + ' has its contract lines')


def full_components(declarations: list, root: pathlib.Path) -> None:
    keys = {declaration.manifest_key() for declaration in declarations}
    for key in ('stxlib::AtomicPool', 'stxlib::Thread', 'stxlib::Mutex', 'stxlib::glyphs::GlyphStore',
                'stxlib::fft::Plan64', 'stxlib::fft::Plan32'):
        check(key in keys, 'I7 ' + key + ' is declared')
    sites = {
        'src/core/thread_pool.cpp': ('admits_create_some_threads', 'admits_create_thread_limit',
                                     'admits_batch_task_limit', 'admits_begin_some_tasks'),
        'src/core/threads.cpp': ('admits_start_entry', 'admits_start_stack'),
        'src/algorithms/glyphs/glyph_store.cpp': ('admits_glyph_index',),
        'src/algorithms/fft/plan.cpp': ('fft_plan64::admits_make_mode', 'fft_plan32::admits_make_float32_mode',
                                        'fft_plan64::admits_rfft_input', 'fft_plan32::admits_irfft_output'),
    }
    for path, names in sites.items():
        text = (root / path).read_text(encoding='utf-8')
        for name in names:
            check(name + '(' in text, 'I7 ' + path + ' calls ' + name)
    plan = (root / 'src' / 'algorithms' / 'fft' / 'plan.cpp').read_text(encoding='utf-8')
    check('known_mode' not in plan and 'minimum_size' not in plan, 'I7 fft make states its clauses once')
    for name in ('glyph_face.cpp', 'glyph_shape.cpp'):
        text = (root / 'src' / 'algorithms' / 'glyphs' / name).read_text(encoding='utf-8')
        check('<< 24' not in text, 'I7 ' + name + ' takes the glyph size bound from font_reader.hpp')


def engine_tables(tool, root: pathlib.Path, declarations: list) -> None:
    project = tool.load_project(root / 'tools' / 'ic' / 'project.toml', root)
    check(project.engine is not None, 'I8 the project names the engine')
    if project.engine is None:
        return
    _, summaries = tool.engine_outputs(project, declarations)
    for summary in summaries:
        check(summary.legal_blocks > 0, 'I8 ' + summary.component + ' has engine blocks')
        check(summary.driven > 0, 'I8 ' + summary.component + ' drives a call')
        for name, reason in summary.skipped:
            check(bool(reason.strip()), 'I8 ' + summary.component + ' ' + name + ' says why it is not driven')


def load_deducer(root: pathlib.Path):
    path = root / 'tools' / 'ic_deduce.py'
    spec = importlib.util.spec_from_file_location('ic_deduce', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['ic_deduce'] = module
    spec.loader.exec_module(module)
    return module


def deductions(tool, root: pathlib.Path) -> None:
    deducer = load_deducer(root)
    output = io.StringIO()
    with redirect_stdout(output):
        status = deducer.run('check', root / 'tools' / 'ic' / 'project.toml', root)
    check(status == 0, 'I9 the deductions are current: ' + output.getvalue().strip())
    import json
    document = json.loads((root / 'proofs' / 'ic' / 'deductions.json').read_text(encoding='utf-8'))
    discharged = {item['id'] for item in document['deductions']}
    for name in ('page_count_take', 'page_count_release', 'page_count_collect', 'page_count_made'):
        check('stxlib.stx_minmallocpp.' + name in discharged, 'I9 the allocator lemma ' + name + ' is discharged')
    allocator = json.loads((root / 'proofs' / 'ic' / 'allocator.obligations.json').read_text(encoding='utf-8'))
    statuses = {item['id']: item['status'] for item in allocator['obligations']}
    check(statuses.get('stxlib.stx_minmallocpp.page_count_take') == 'discharged',
          'I9 a discharged lemma is recorded in its obligations file')
    proved, _ = deducer.lemma(('sum + live == capacity', 'word != 0',
                               'sum_after == sum - pop(word) + pop(word & (word - 1))', 'live_after == live + 1'),
                              'sum_after + live_after == capacity + 1')
    check(not proved, 'I10 a wrong page-count conclusion is not proved')
    proved, _ = deducer.lemma(('sum + live == capacity', 'sum_after == sum - pop(known) + pop(known | bit)',
                               'live_after == live - 1', 'single_bit(bit)'), 'sum_after + live_after == capacity')
    check(not proved, 'I10 pop(a | b) is not split without the words being disjoint')
    clause = tool.parse_clause('length <= count - first')
    failures = deducer.exact_failures(clause, {'length': 'usize', 'count': 'usize', 'first': 'usize'}, {})
    check(bool(failures), 'I10 an unguarded subtraction is not exact')
    clause = tool.parse_clause('first <= count and length <= count - first')
    failures = deducer.exact_failures(clause, {'length': 'usize', 'count': 'usize', 'first': 'usize'}, {})
    check(not failures, 'I10 a subtraction its conjunct guards is exact')


def gate_file(root: pathlib.Path, declarations: list) -> None:
    text = (root / 'proofs' / 'ic' / 'GATE.md').read_text(encoding='utf-8')
    for declaration in declarations:
        check('| ' + declaration.manifest_key() + ' |' in text, 'I11 the gate lists ' + declaration.manifest_key())


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_ic_declarations_tests.py <repository root>')
        return 2
    root = pathlib.Path(sys.argv[1]).resolve()
    tool = load_tool(root)
    whole_check(tool, root)
    declarations = declarations_of(tool, root)
    coverage(declarations)
    requirements_checked(root)
    messages_once(tool, root, declarations)
    files_refusals(root)
    contract_lines(tool, root, declarations)
    full_components(declarations, root)
    engine_tables(tool, root, declarations)
    deductions(tool, root)
    gate_file(root, declarations)
    for label in failures:
        print('FAILED: ' + label)
    print(f'IC declaration tests: {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
