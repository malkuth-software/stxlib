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
      first declaration line (or above its template line).
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
        for call in declaration.calls:
            wanted = call.declarations[0].strip()
            found = [index for index, line in enumerate(lines) if line.strip().startswith(wanted)]
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
    for label in failures:
        print('FAILED: ' + label)
    print(f'IC declaration tests: {cases - len(failures)} of {cases} passed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
