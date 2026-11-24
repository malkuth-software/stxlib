#!/usr/bin/env python3
"""Finds the Windows API names that our sources use.

Usage:
  python3 -I tools/windows_api_headers/scan.py [--out roots.txt]

Reads the repositories named in `sources.json` at their pinned revisions
(through `git ls-tree` and `git cat-file`, never the working tree), plus
pinned plain directories (libc++ sources, the fetched miniaudio tree), and
writes `roots.txt`: the Windows header names the sources include and every
identifier in a Windows-facing file that names a declaration in the
metadata (`PIN.json`), resolved as described in `resolve_token`.

A file is Windows-facing when its path has a `windows` or `win32` directory
or a `_windows`/`_win`/`_win32` stem suffix, or when it includes a header
named after a metadata file (or windows.h and the other umbrella names in
`UMBRELLAS`). Comments and string literals are removed before the
identifiers are taken. Preprocessor conditions are not evaluated: a name in
an inactive branch still counts. Over-inclusion costs a declaration; it
never removes one.

The output is deterministic: sorted, no timestamps.
"""

import argparse
import json
import pathlib
import posixpath
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
# Run with -I (isolated): the reader is imported from this directory explicitly.
sys.path.insert(0, str(HERE))
import local_paths  # noqa: E402
import rdl  # noqa: E402

EXTENSIONS = ('.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.ipp', '.inc')
UUIDOF = re.compile(r'__uuidof\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)')
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([A-Za-z0-9_./]+)\.h[>"]', re.MULTILINE)
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
# Umbrella names our sources may include that have no metadata file of their own.
UMBRELLAS = ('windows', 'uiautomation', 'versionhelpers')


def strip_comments_and_strings(text: str) -> str:
    """Blanks comments, string and character literals (keeps line structure)."""
    result: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        character = text[index]
        if text.startswith('//', index):
            end = text.find('\n', index)
            index = length if end < 0 else end
            continue
        if text.startswith('/*', index):
            end = text.find('*/', index + 2)
            index = length if end < 0 else end + 2
            result.append(' ')
            continue
        if character == '"' or character == "'":
            end = index + 1
            while end < length and text[end] != character and text[end] != '\n':
                end += 2 if text[end] == '\\' else 1
            index = end + 1
            result.append(' ')
            continue
        result.append(character)
        index += 1
    return ''.join(result)


def windows_facing_path(path: str) -> bool:
    parts = path.lower().split('/')
    for directory in parts[:-1]:
        if directory in ('windows', 'win32', 'windows_msvc_startup', 'msvc_runtime'):
            return True
    stem = parts[-1].rsplit('.', 1)[0]
    return stem.endswith('_windows') or stem.endswith('_win') or stem.endswith('_win32')


def excluded_path(path: str, exclusions: list[str]) -> bool:
    for prefix in exclusions:
        if path.startswith(prefix):
            return True
    return False


def git_files(repository: str, revision: str, roots: list[str], exclusions: list[str]) -> dict[str, str]:
    """Reads every source file under roots at revision: {path: text}."""
    listing = subprocess.run(['git', '-C', repository, 'ls-tree', '-r', '--name-only', revision, '--'] + roots,
                             check=True, capture_output=True, text=True).stdout.splitlines()
    wanted: list[str] = []
    for path in listing:
        if path.endswith(EXTENSIONS) and not excluded_path(path, exclusions):
            wanted.append(path)
    request = ''.join(f'{revision}:{path}\n' for path in wanted).encode('utf-8')
    output = subprocess.run(['git', '-C', repository, 'cat-file', '--batch'], input=request,
                            check=True, capture_output=True).stdout
    files: dict[str, str] = {}
    offset = 0
    for path in wanted:
        header_end = output.index(b'\n', offset)
        header = output[offset:header_end].split()
        if len(header) != 3:
            raise SystemExit(f'git cat-file failed for {revision}:{path}')
        size = int(header[2])
        body = output[header_end + 1:header_end + 1 + size]
        offset = header_end + 1 + size + 1
        files[path] = body.decode('utf-8', errors='replace')
    return files


def directory_files(directory: str, roots: list[str], exclusions: list[str]) -> dict[str, str]:
    base = pathlib.Path(directory)
    files: dict[str, str] = {}
    for root in roots:
        for path in sorted((base / root).rglob('*')):
            relative = path.relative_to(base).as_posix()
            if path.is_file() and relative.endswith(EXTENSIONS) and not excluded_path(relative, exclusions):
                files[relative] = path.read_text(encoding='utf-8', errors='replace')
    return files


ANY_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([A-Za-z0-9_./-]+)[>"]', re.MULTILINE)


def facing_files(files: dict[str, str], header_names: set[str]) -> set[str]:
    """Windows-facing files: by path, by a Windows header include, or by
    including (directly or through other first-party files) a facing file.
    An include names a file of the same source when the file's path ends
    with it."""
    by_basename: dict[str, list[str]] = {}
    for path in files:
        by_basename.setdefault(path.rsplit('/', 1)[-1], []).append(path)
    facing: set[str] = set()
    targets: dict[str, list[str]] = {}
    for path in sorted(files):
        text = files[path]
        resolved: list[str] = []
        direct = windows_facing_path(path)
        for match in ANY_INCLUDE.finditer(text):
            name = match.group(1)
            stem = name[:-2].lower() if name.endswith('.h') else ''
            if stem in header_names:
                direct = True
                continue
            # A relative include ("../src/x.hpp") names the file next to the
            # includer; any other names a file whose path ends with it.
            relative = posixpath.normpath(posixpath.join(posixpath.dirname(path), name))
            for candidate in by_basename.get(name.rsplit('/', 1)[-1], []):
                if candidate == name or candidate == relative or candidate.endswith('/' + name):
                    resolved.append(candidate)
        targets[path] = resolved
        if direct:
            facing.add(path)
    changed = True
    while changed:
        changed = False
        for path in sorted(files):
            if path in facing:
                continue
            for target in targets[path]:
                if target in facing:
                    facing.add(path)
                    changed = True
                    break
    return facing


class Index:
    """Metadata names for one architecture."""

    def __init__(self, declarations: list[rdl.Decl], arch: str) -> None:
        self.declarations: dict[str, rdl.Decl] = {}
        self.enumerators: dict[str, str] = {}
        for declaration in declarations:
            if not rdl.arch_allows(declaration.attributes, arch):
                continue
            self.declarations[declaration.name] = declaration
            if declaration.kind == 'enum':
                for member, _ in declaration.members:
                    self.enumerators.setdefault(member, declaration.name)


def is_class_guid(declaration: rdl.Decl) -> bool:
    """A GUID constant with no prefix: the metadata's spelling of a coclass."""
    if declaration.kind != 'const' or declaration.value_type is None:
        return False
    value_type = declaration.value_type
    return value_type.kind == 'named' and value_type.name == 'GUID' and '_' not in declaration.name


def resolve_token(token: str, index: Index, roots: set[str], excluded_stems: set[str]) -> None:
    """Adds the roots a source identifier asks for.

    - a declaration name roots itself; when it also has an ANSI/wide pair
      (NAMEA, NAMEW) it is the neutral name and roots NAMEW as well;
    - a name absent from the metadata whose NAMEW is present is a neutral
      name (UNICODE is assumed): roots NAMEW and the alias;
    - an enumerator roots its enum;
    - IID_<interface> and CLSID_<class GUID> root the GUID constant.
    """
    declarations = index.declarations
    candidate = declarations.get(token) or declarations.get(token + 'W')
    if candidate is not None and candidate.source in excluded_stems:
        return
    if token in declarations and is_class_guid(declarations[token]):
        # A class GUID is named CLSID_<class> in our sources; its bare name
        # (Shell, Path, Component) is usually one of ours.
        return
    if token in declarations:
        roots.add(token)
        if token + 'W' in declarations and token + 'A' in declarations:
            roots.add(token + 'W')
        return
    if token + 'W' in declarations:
        roots.add(token)
        roots.add(token + 'W')
        return
    if token in index.enumerators:
        if declarations[index.enumerators[token]].source in excluded_stems:
            return
        roots.add(index.enumerators[token])
        return
    if token.startswith('IID_') and token[4:] in declarations and declarations[token[4:]].kind == 'interface':
        roots.add(token)
        return
    if token.startswith('CLSID_') and token[6:] in declarations and declarations[token[6:]].kind == 'const':
        roots.add(token)
        return


def main() -> int:
    arguments = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    arguments.add_argument('--out', default=str(HERE / 'roots.txt'))
    options = arguments.parse_args()
    pin = json.loads((HERE / 'PIN.json').read_text(encoding='utf-8'))
    sources = json.loads((HERE / 'sources.json').read_text(encoding='utf-8'))
    metadata = local_paths.metadata_checkout(pin) / 'metadata' / 'win32'
    index = Index(rdl.load_directory(metadata), pin['arch'])
    header_names: set[str] = set(UMBRELLAS)
    for path in metadata.glob('*.rdl'):
        header_names.add(path.stem.lower())
    excluded_tokens = set(sources['excluded_tokens'])
    excluded_stems = set(sources['excluded_stems'])
    for stem in excluded_stems:
        header_names.discard(stem)

    roots: set[str] = set()
    includes: dict[str, int] = {}
    facing_count = 0
    for source in sources['sources']:
        if 'revision' in source:
            files = git_files(local_paths.source_path(source), source['revision'], source['roots'], source['exclude'])
        else:
            files = directory_files(local_paths.source_path(source), source['roots'], source['exclude'])
        facing = facing_files(files, header_names)
        for path in sorted(facing):
            text = files[path]
            facing_count += 1
            for match in INCLUDE.finditer(text):
                name = match.group(1).lower()
                if name in header_names:
                    includes[name] = includes.get(name, 0) + 1
            code = strip_comments_and_strings(text)
            for token in set(IDENTIFIER.findall(code)):
                if token not in excluded_tokens:
                    resolve_token(token, index, roots, excluded_stems)
            # __uuidof(Name) on a coclass: root CLASS:Name, which declares the
            # class with its uuid (the bare name alone is skipped above).
            for name in set(UUIDOF.findall(code)):
                declaration = index.declarations.get(name)
                if declaration is not None and is_class_guid(declaration) and declaration.source not in excluded_stems:
                    roots.add('CLASS:' + name)
    for name in sources['extra_roots']:
        resolve_token(name, index, roots, excluded_stems)
        if name not in roots and name not in sources['supplement_names']:
            print(f'scan: extra root {name} is not in the metadata', file=sys.stderr)

    lines = ['# Generated by tools/windows_api_headers/scan.py from sources.json; do not edit.',
             f'# Windows-facing files scanned: {facing_count}', '[includes]']
    for name in sorted(includes):
        lines.append(f'{name} {includes[name]}')
    lines.append('[roots]')
    for name in sorted(roots):
        lines.append(name)
    pathlib.Path(options.out).write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print(f'scan: {facing_count} files, {len(includes)} header names, {len(roots)} roots')
    return 0


if __name__ == '__main__':
    sys.exit(main())
