#!/usr/bin/env python3
"""Where the generated Windows API headers fall short of the SDK: a survey.

Usage:
  python3 -I tools/windows_api_headers/sdk_gaps.py OUT [REPO ...]

Compares every metadata file (one per SDK header) with the names the
committed headers in src/hosted/windows/api/include spell, by word, so it
checks the generator from outside its own model: a name counts when any
generated header spells it, as a declaration or a macro. Then it scans the
resource scripts and Windows-facing sources of each REPO (a working tree)
for metadata names no header spells. Prints a summary; the full lists go to
OUT. The metadata checkout is PIN.json's (STXWIN_METADATA overrides it).

generate.py's own windows.h check (SDK_WINDOWS_H, WINDOWS_H_UNCOVERED) is
the enforced part; this survey covers every other metadata file too and
finds names our programs use. The integrator wrote it for the 2026-11-24
coverage analysis (cp37, stx_sdk_gaps.py).
"""

import argparse
import collections
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
# Run with -I (isolated): the generator's modules are imported from this directory explicitly.
sys.path.insert(0, str(HERE))
import generate  # noqa: E402
import local_paths  # noqa: E402
import rdl  # noqa: E402

INCLUDE = HERE.parent.parent / 'src/hosted/windows/api/include'
SDK_INCLUDE = re.compile(r'#\s*include\s*[<"](windows|winuser|wingdi|winbase|winver|commctrl|shellapi|shlobj|objbase|'
                         r'ole2|dwmapi|uxtheme|d3d\w*|dxgi\w*|winsock2|ws2tcpip|mmsystem|commdlg)\.h[>"]', re.I)
WORD = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
SUFFIXES = ('.rc', '.rc.in', '.rc2', '.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.ipp', '.inc', '.mm')
SKIPPED_DIRECTORIES = ('.git', 'build', '.build', 'third_party', 'node_modules', 'vendor')


def describe(entry: tuple) -> str:
    stem, total, missing, kinds, dlls = entry
    kind_text = ', '.join(f'{kind} {count}' for kind, count in sorted(kinds.items()))
    dll_text = ', '.join(f'{dll} {count}' for dll, count in sorted(dlls.items()))
    return f'{stem}: {missing}/{total} missing ({kind_text})' + (f'; functions from {dll_text}' if dll_text else '')


def main() -> int:
    arguments = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    arguments.add_argument('out')
    arguments.add_argument('repos', nargs='*')
    options = arguments.parse_args()
    pin = json.loads((HERE / 'PIN.json').read_text(encoding='utf-8'))
    metadata = local_paths.metadata_checkout(pin) / 'metadata' / 'win32'

    emitted: set[str] = set()
    for header in INCLUDE.glob('*.h'):
        emitted.update(WORD.findall(header.read_text(encoding='utf-8', errors='replace')))
    headers = {path.stem.lower() for path in INCLUDE.glob('*.h')}

    by_file: dict[str, list] = collections.defaultdict(list)
    for declaration in rdl.load_directory(metadata):
        if rdl.arch_allows(declaration.attributes, pin['arch']):
            by_file[declaration.source].append(declaration)
    names = {}
    for stem, declarations in by_file.items():
        for declaration in declarations:
            names.setdefault(declaration.name, (stem, declaration))

    partial = []
    absent = []
    for stem in sorted(by_file):
        declarations = by_file[stem]
        missing = [item for item in declarations if item.name not in emitted]
        if not missing:
            continue
        kinds = collections.Counter(item.kind for item in missing)
        dlls = collections.Counter(generate.function_library(item) or 'import-only'
                                   for item in missing if item.kind == 'function')
        entry = (stem, len(declarations), len(missing), kinds, dlls)
        (partial if stem in headers else absent).append(entry)

    lines = ['== headers generated, some names missing']
    lines.extend(describe(entry) for entry in partial)
    lines.append('== headers not generated')
    lines.extend(describe(entry) for entry in absent)
    function_free = [entry for entry in absent if 'function' not in entry[3]]

    lines.append("== windows.h's default include set (generate.SDK_WINDOWS_H)")
    windows_lines = []
    for stem in generate.SDK_WINDOWS_H:
        if stem not in by_file:
            windows_lines.append(f'{stem}: no metadata file')
            continue
        match = [entry for entry in partial + absent if entry[0] == stem]
        state = 'complete' if not match else ('partial: ' if stem in headers else 'absent: ') + describe(match[0])
        windows_lines.append(f'{stem}: {state}')
    lines.extend(windows_lines)

    lines.append('== names the scanned files use that no header spells')
    use_hits: dict[str, set[str]] = collections.defaultdict(set)
    for repo in (pathlib.Path(item) for item in options.repos):
        for path in repo.rglob('*'):
            if not path.is_file() or not path.name.endswith(SUFFIXES):
                continue
            if any(part in SKIPPED_DIRECTORIES for part in path.parts):
                continue
            text = path.read_text(encoding='utf-8', errors='replace')
            # Only what a Windows compile reads: resource scripts, Windows
            # paths and files that include a Windows SDK header.
            relative = str(path.relative_to(repo)).lower()
            if not ('.rc' in path.name or 'windows' in relative or 'win32' in relative or SDK_INCLUDE.search(text)):
                continue
            for word in set(WORD.findall(text)):
                if word in names and word not in emitted:
                    use_hits[word].add(f'{repo.name}/{path.relative_to(repo)}')
    resource_hits = {word for word, files in use_hits.items() if any('.rc' in item for item in files)}
    for word in sorted(use_hits):
        stem, declaration = names[word]
        files = sorted(use_hits[word])
        lines.append(f'{word} [{declaration.kind}, {stem}.rdl]: {", ".join(files[:4])}' +
                     (f' (+{len(files) - 4})' if len(files) > 4 else ''))

    pathlib.Path(options.out).write_text('\n'.join(lines) + '\n', encoding='utf-8')
    print(f'metadata files {len(by_file)}; headers generated {len(headers)}')
    print(f'generated but partial: {len(partial)}; not generated: {len(absent)} ({len(function_free)} function-free)')
    print("windows.h's set, not complete: " + '; '.join(item for item in windows_lines if ': complete' not in item))
    print(f'names used by scanned files but not spelled: {len(use_hits)} ({len(resource_hits)} in resource scripts)')
    for word in sorted(resource_hits):
        print(f'  rc: {word} [{names[word][0]}]')
    print(f'full lists: {options.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
