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
  I11 the gate file lists every declared component;
  I12 the deducer follows the sources soundly: a selector helper is read
      only when its body just picks predicates, a switch is fixed only by a
      literal, an output reset and a read-only check may precede a guard
      but a write may not, a unary star on an untouched value is a
      dereference and a product is not;
  I13 page_protocol is an invariant kept by every step: word identities are
      decided bit by bit (a partition the owner's release breaks without the
      block being held is not proved), and dropping a step, or a step's
      lemma for a word a cited write changes, leaves it open;
  I14 every obligation in proofs/ic is discharged or names what covers it
      (the engine's blocks or tests) and why the deducer does not reach it,
      and the obligations still open are exactly the five refusals that are
      the host's (File) and the two identities that are addresses;
  I15 the typestate reader (tools/ic_typestate.py) is sound on what it
      reads: a state's condition decides a guard (equality's laws: terms
      equal to distinct constants are unequal), a refusal that writes first
      is not discharged, a transition that does not write its state is not,
      a loop that writes a state member and an opaque call given a state
      member stop it, and the Mutex's requirements hold only in the build
      that checks them.
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
                                        # plan.cpp's transfer check names these through the
                                        # aliases plan64 and plan32.
                                        'plan64::admits_rfft_input', 'plan32::admits_irfft_output'),
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


def source_following(root: pathlib.Path) -> None:
    deducer = load_deducer(root)
    import json
    document = json.loads((root / 'proofs' / 'ic' / 'deductions.json').read_text(encoding='utf-8'))
    still_open = sorted(item['id'] for item in document['open'])
    expected = sorted(['stxlib.file.read.outside.writing', 'stxlib.file.read.outside.appending',
                       'stxlib.file.write_all.outside.reading', 'stxlib.file.read_at.outside.writing',
                       'stxlib.file.read_at.outside.appending', 'stxlib.memory.identity',
                       'stxlib.stx_minmallocpp.identity'])
    check(still_open == expected, 'I14 the obligations open are the host refusals and the address identities: '
          + ', '.join(still_open))
    every = []
    for path in sorted((root / 'proofs' / 'ic').glob('*.obligations.json')):
        every.extend(json.loads(path.read_text(encoding='utf-8'))['obligations'])
    for item in every:
        if item['status'] == 'discharged':
            continue
        check(bool(item.get('covered_by')) and bool(item.get('not_deduced')),
              'I14 ' + item['id'] + ' is open without what covers it and why it is not deduced')
    check(len(every) == len(document['deductions']) + len(document['open']),
          'I14 the deducer attempts every obligation (' + str(len(every)) + ' in proofs/ic)')
    lines = ['[[nodiscard]] bool pick(const u8 mode, const bool wide) noexcept {',
             '    const bool result = wide ? ::n::w::admits_a(mode)',
             '                             : ::n::s::admits_a(mode);',
             '    return result;',
             '}',
             '[[nodiscard]] bool bad(const u8 mode, const bool wide) noexcept {',
             '    counter = counter + 1;',
             '    return wide ? ::n::w::admits_a(mode) : ::n::s::admits_a(mode);',
             '}']
    selector = deducer.selector_of(lines, 0)
    check(selector is not None and selector.picks == {(False,): 'n::s::admits_a', (True,): 'n::w::admits_a'},
          'I12 a selector helper picks its predicates by its switch')
    check(deducer.selector_of(lines, 5) is None, 'I12 a helper that writes is not a selector')
    check(deducer.selector_value('precision == Precision::float64', [('Precision', 'precision')],
                                 ['Precision::float32']) is False, 'I12 a switch compared with an enum literal')
    check(deducer.selector_value('wide', [('bool', 'wide')], ['flag']) is None,
          'I12 a switch that is not a literal is not fixed')
    body = ['void check(const State& s) noexcept {', '    if (s.a != 0) return;', '    fail_fast(record);', '}',
            'void touch(State& s) noexcept {', '    s.a = 1;', '}',
            'OperationStatus Thing::go(const u32 x, Out& out) noexcept {', '    out = Out{};', '    check(state);',
            '    touch(state);', '    if (!admits_x(x)) return no;', '}']
    clean, offending, notes = deducer.precedes_cleanly(body, 11)
    check(not clean and offending == 'touch(state)', 'I12 a call that writes may not precede a guard')
    check(len(notes) == 2, 'I12 an output reset and a read-only check may precede a guard')
    check(deducer.unary_star('const u64 x = *value;', 'value'), 'I12 a unary star dereferences')
    check(not deducer.unary_star('const u64 x = a * value;', 'value'), 'I12 a product is not a dereference')


def page_invariant(tool, root: pathlib.Path, declarations: list) -> None:
    import dataclasses
    deducer = load_deducer(root)
    partition = ('H & F == 0', 'H & R == 0', 'F & R == 0', '(H | F | R) == M')
    proved, _ = deducer.lemma(partition + ('b & H == b', 'F_after == F | b', 'H_after == H & ~b', 'R_after == R'),
                              '(H_after | F_after | R_after) == M')
    check(proved, 'I13 the owner\'s release of a held block keeps the partition')
    proved, _ = deducer.lemma(partition + ('F_after == F | b', 'H_after == H & ~b', 'R_after == R'),
                              '(H_after | F_after | R_after) == M')
    check(not proved, 'I13 without the block held the partition is not proved')
    allocator = [item for item in declarations if item.path.endswith('allocator.ic.toml')][0]
    records, _ = deducer.deduce_declaration(root, allocator)
    discharged = {record.identifier: record for record in records}
    prefix = allocator.manifest_key().replace('::', '.').lower()
    stated = {prefix + '.' + item.identifier: item for item in allocator.obligations}
    protocol = [item for item in allocator.obligations if item.identifier == 'page_protocol'][0]
    held, _, _, _ = deducer.invariant_kept(root, prefix, protocol, discharged, stated)
    check(held, 'I13 page_protocol is kept by its steps')
    without_scan = dataclasses.replace(protocol, composes=tuple(entry for entry in protocol.composes
                                                                if not entry.startswith('scan')))
    held, _, _, _ = deducer.invariant_kept(root, prefix, without_scan, discharged, stated)
    check(not held, 'I13 a write no step cites leaves the invariant open')
    without_summary = dataclasses.replace(protocol, composes=tuple(entry.replace(', summary_take', '')
                                                                   for entry in protocol.composes))
    held, _, _, _ = deducer.invariant_kept(root, prefix, without_summary, discharged, stated)
    check(not held, 'I13 a step that changes a word its lemmas do not describe leaves the invariant open')


def typestate_reading(tool, root: pathlib.Path, declarations: list) -> None:
    import dataclasses
    deducer = load_deducer(root)
    states = sys.modules['ic_typestate']
    reading = states.parse('handle_ != closed && mode_ == FileMode::read')
    check(states.decide(states.parse('handle_ == closed'), [reading]) == 'false',
          'I15 a state decides a guard on it')
    check(states.decide(states.parse('mode_ == FileMode::append'), [reading]) == 'false',
          'I15 a member equal to one enumerator is not another')
    check(states.decide(states.parse('font_bytes_ == font.bytes.data()'),
                        [states.parse('font_bytes_ == nullptr'), states.parse('font.bytes.data() != nullptr')])
          == 'false', 'I15 equality joins terms and splits none taken unequal')
    check(states.decide(states.parse('a_ == 1'), [states.parse('b_ == 1')]) == 'unknown',
          'I15 what the givens do not fix is unknown')
    source = '\n'.join([
        'class Box final {', 'public:', '    bool open() const noexcept { return held_ != nullptr; }',
        '    u8* held_{nullptr};', '};',
        'OperationStatus Box::take() noexcept {', '    count_ = count_ + 1;',
        '    if (!open()) return OperationStatus::failure(ErrorRecord::make(ErrorKind::disposed, op));',
        '    return OperationStatus::success();', '}',
        'OperationStatus Box::give() noexcept {',
        '    if (!open()) return OperationStatus::failure(ErrorRecord::make(ErrorKind::disposed, op));',
        '    return OperationStatus::success();', '}',
        'OperationStatus Box::shut() noexcept {', '    if (!open()) return OperationStatus::success();',
        '    return OperationStatus::success();', '}',
        'void Box::spin() noexcept {', '    for (u32 i = 0; i < 4; ++i) held_ = nullptr;', '}',
        'void Box::hand() noexcept {', '    backend::forget(held_);', '}',
        'u8* Box::drop() noexcept {', '    u8* const result = held_;', '    held_ = nullptr;', '    return result;',
        '}'])
    view = states.View('Box', {'closed': 'held_ == nullptr', 'open': 'held_ != nullptr'}, [], {}, [],
                       {'box.cpp': states.preprocessed(source, ())}, ())
    refusal = tool.refusal_of('failure:disposed', 'test')
    outcome = states.typestate_refusal(view, 'give', 'closed', refusal, 'stxlib::HostCode', deducer.case_mapping)
    check(outcome.held, 'I15 a guard on the state that returns the declared failure is a typestate refusal')
    outcome = states.typestate_refusal(view, 'take', 'closed', refusal, 'stxlib::HostCode', deducer.case_mapping)
    check(not outcome.held, 'I15 a refusal after a write is not discharged')
    outcome = states.typestate_refusal(view, 'give', 'open', refusal, 'stxlib::HostCode', deducer.case_mapping)
    check(not outcome.held, 'I15 a call that succeeds in a state does not refuse there')
    outcome = states.transition(view, 'shut', ('open',), 'closed')
    check(not outcome.held, 'I15 a transition that does not write its state is not discharged')
    outcome = states.transition(view, 'spin', ('open',), 'closed')
    check(not outcome.held and 'loop' in outcome.why, 'I15 a loop that writes a state member stops the reader')
    outcome = states.transition(view, 'hand', ('open',), 'closed')
    check(not outcome.held and 'touches' in outcome.why, 'I15 an opaque call given a state member stops it')
    outcome = states.transition(view, 'drop', ('open',), 'closed')
    check(outcome.held, 'I15 a member that returns a pointer is read: ' + outcome.why)
    mutex = [item for item in declarations if item.path.endswith('/mutex.ic.toml')][0]
    checked = deducer.typestate_view(root, mutex)
    held = tool.refusal_of('requirement', 'test')
    outcome = states.typestate_refusal(checked, 'lock', 'held', held, 'stxlib::HostCode', deducer.case_mapping)
    check(outcome.held, 'I15 lock in held ends the process in the build that checks locks')
    unchecked = dataclasses.replace(checked, texts={path: states.preprocessed((root / path).read_text(
        encoding='utf-8'), ()) for path in checked.texts}, build=())
    outcome = states.typestate_refusal(unchecked, 'lock', 'held', held, 'stxlib::HostCode', deducer.case_mapping)
    check(not outcome.held, 'I15 without the lock checks lock in held is not a refusal')


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
    source_following(root)
    page_invariant(tool, root, declarations)
    typestate_reading(tool, root, declarations)
    gate_file(root, declarations)
    for label in failures:
        print('FAILED: ' + label)
    print(f'IC declaration tests: {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
