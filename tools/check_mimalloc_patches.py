#!/usr/bin/env python3
"""Checks STXlib's patched mimalloc against its pinned release and patch series.

STXlib keeps its own patched copy of mimalloc (AGENTS.md; the owner,
2026-11-15). `third_party/mimalloc/` is a pinned upstream release with the
patches of `third_party/mimalloc-patches/` applied, in the order of that
directory's `series` file. PROVENANCE.md, section "mimalloc", holds three
tables:

- the release table: every file of the pinned release with its Git blob
  SHA-1 and SHA-256 (header "| File (under `third_party/mimalloc/`) |");
- the patch table: every patch with its SHA-256, the defect it fixes and
  its upstream status (header "| Patch (under `third_party/mimalloc-patches/`) |");
- the patched-file table: the SHA-256 of every file a patch changes, as it
  is in the tree (header "| Patched file (under `third_party/mimalloc/`) |").

The check fails unless all of these hold:
1. the series, the patch files on disk and the patch table name the same
   patches, and each patch's SHA-256 equals its table entry;
2. each patch has a header, before its first `--- ` line, with a `Fixes:`
   line (the audit ID) and an `Upstream:` line;
3. the tree holds exactly the files of the release table (patches change
   files; they neither add nor remove them);
4. each file a patch changes has the SHA-256 of the patched-file table, and
   every other file the SHA-256 of the release table; the patched-file
   table names exactly the files the patches change;
5. the series, reverse-applied in reverse order, gives back the release: every
   file's Git blob SHA-1 and SHA-256 equal the release table.

Patches are unified diffs with paths relative to `third_party/mimalloc/`
(`--- a/src/x.c`, `+++ b/src/x.c`). They are reverse-applied exactly: each
hunk must match at the line its header states, with no offset and no fuzz.

Usage:
  python3 -I tools/check_mimalloc_patches.py <repository root>
  python3 -I tools/check_mimalloc_patches.py <repository root> --print-tables

--print-tables prints the patch and patched-file table rows for the tree as
it is (for PROVENANCE.md), after checks 1-3 and 5, and does not check 4.

Exit status 0 when every check passes, 1 otherwise; each failure is printed.
"""

import hashlib
import pathlib
import re
import sys

TREE = pathlib.PurePosixPath('third_party/mimalloc')
PATCHES = pathlib.PurePosixPath('third_party/mimalloc-patches')
RELEASE_HEADER = '| File (under `third_party/mimalloc/`) |'
PATCH_HEADER = '| Patch (under `third_party/mimalloc-patches/`) |'
PATCHED_HEADER = '| Patched file (under `third_party/mimalloc/`) |'
HUNK = re.compile(rb'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


class CheckFailure(Exception):
    pass


def blob_sha1(data):
    header = b'blob ' + str(len(data)).encode('ascii') + b'\0'
    result = hashlib.sha1(header + data).hexdigest()
    return result


def sha256(data):
    result = hashlib.sha256(data).hexdigest()
    return result


def table_rows(lines, header):
    """Rows of the markdown table whose first line starts with `header`, as
    lists of cells with backquotes removed. None when the table is absent."""
    start = None
    for index, line in enumerate(lines):
        if line.startswith(header):
            start = index
            break
    if start is None:
        return None
    rows = []
    for line in lines[start + 2:]:
        if not line.startswith('|'):
            break
        cells = [cell.strip().strip('`') for cell in line.strip().strip('|').split('|')]
        rows.append(cells)
    return rows


def read_provenance(root):
    text = (root / 'PROVENANCE.md').read_text(encoding='utf-8')
    lines = text.splitlines()
    release_rows = table_rows(lines, RELEASE_HEADER)
    patch_rows = table_rows(lines, PATCH_HEADER)
    patched_rows = table_rows(lines, PATCHED_HEADER)
    if release_rows is None or patch_rows is None or patched_rows is None:
        raise CheckFailure('PROVENANCE.md: the mimalloc release, patch or patched-file table is missing')
    release = {}
    for cells in release_rows:
        if len(cells) != 3:
            raise CheckFailure('PROVENANCE.md: release table row does not have 3 cells: ' + ' | '.join(cells))
        release[cells[0]] = (cells[1], cells[2])
    patches = {}
    for cells in patch_rows:
        if len(cells) != 4:
            raise CheckFailure('PROVENANCE.md: patch table row does not have 4 cells: ' + ' | '.join(cells))
        patches[cells[0]] = cells[1]
    patched = {}
    for cells in patched_rows:
        if len(cells) != 2:
            raise CheckFailure('PROVENANCE.md: patched-file table row does not have 2 cells: ' + ' | '.join(cells))
        patched[cells[0]] = cells[1]
    return release, patches, patched


def read_series(root):
    series_path = root / PATCHES / 'series'
    if not series_path.is_file():
        raise CheckFailure(str(PATCHES / 'series') + ' is missing')
    names = []
    for line in series_path.read_text(encoding='utf-8').splitlines():
        name = line.split('#', 1)[0].strip()
        if name:
            names.append(name)
    if len(set(names)) != len(names):
        raise CheckFailure('series names a patch twice')
    return names


def read_tree(root):
    base = root / TREE
    files = {}
    for path in sorted(base.rglob('*')):
        if path.is_file():
            files[path.relative_to(base).as_posix()] = path.read_bytes()
    return files


class FilePatch:
    def __init__(self, path):
        self.path = path
        self.hunks = []  # (old_start, old_lines, new_start, new_lines)


def strip_prefix(raw, prefix):
    text = raw.decode('utf-8').split('\t', 1)[0].strip()
    if not text.startswith(prefix):
        raise CheckFailure('patch path does not start with ' + prefix + ': ' + text)
    return text[len(prefix):]


def parse_patch(name, data):
    lines = data.splitlines(keepends=True)
    first = None
    for index, line in enumerate(lines):
        if line.startswith(b'--- '):
            first = index
            break
    if first is None:
        raise CheckFailure(name + ': no file in the patch')
    header = b''.join(lines[:first]).decode('utf-8')
    if not re.search(r'^Fixes: \S', header, re.MULTILINE):
        raise CheckFailure(name + ': header has no "Fixes:" line')
    if not re.search(r'^Upstream: \S', header, re.MULTILINE):
        raise CheckFailure(name + ': header has no "Upstream:" line')
    file_patches = []
    index = first
    while index < len(lines):
        line = lines[index]
        if not line.startswith(b'--- '):
            index = index + 1
            continue
        if index + 1 >= len(lines) or not lines[index + 1].startswith(b'+++ '):
            raise CheckFailure(name + ': "--- " not followed by "+++ "')
        old_path = strip_prefix(line[4:], 'a/')
        new_path = strip_prefix(lines[index + 1][4:], 'b/')
        if old_path != new_path:
            raise CheckFailure(name + ': a patch may not rename, add or remove a file: ' + old_path)
        current = FilePatch(new_path)
        index = index + 2
        while index < len(lines) and lines[index].startswith(b'@@'):
            match = HUNK.match(lines[index])
            if match is None:
                raise CheckFailure(name + ': malformed hunk header in ' + new_path)
            old_count = int(match.group(2)) if match.group(2) is not None else 1
            new_count = int(match.group(4)) if match.group(4) is not None else 1
            old_lines = []
            new_lines = []
            index = index + 1
            while len(old_lines) < old_count or len(new_lines) < new_count:
                if index >= len(lines):
                    raise CheckFailure(name + ': hunk ends early in ' + new_path)
                body = lines[index]
                kind = body[:1]
                content = body[1:]
                if index + 1 < len(lines) and lines[index + 1].startswith(b'\\'):
                    content = content.rstrip(b'\n')
                if kind == b' ':
                    old_lines.append(content)
                    new_lines.append(content)
                elif kind == b'-':
                    old_lines.append(content)
                elif kind == b'+':
                    new_lines.append(content)
                else:
                    raise CheckFailure(name + ': unexpected line in a hunk of ' + new_path)
                index = index + 1
                if index < len(lines) and lines[index].startswith(b'\\'):
                    index = index + 1
            current.hunks.append((int(match.group(1)), old_lines, int(match.group(3)), new_lines))
        if not current.hunks:
            raise CheckFailure(name + ': no hunk for ' + new_path)
        file_patches.append(current)
    return file_patches


def reverse_apply(name, file_patch, data):
    lines = data.splitlines(keepends=True)
    for old_start, old_lines, new_start, new_lines in reversed(file_patch.hunks):
        begin = new_start - 1 if new_lines else new_start
        if lines[begin:begin + len(new_lines)] != new_lines:
            raise CheckFailure(name + ': hunk at +' + str(new_start) + ' does not match ' + file_patch.path)
        lines[begin:begin + len(new_lines)] = old_lines
    result = b''.join(lines)
    return result


def check(root, print_tables):
    failures = []
    release, patch_table, patched_table = read_provenance(root)
    series = read_series(root)
    on_disk = sorted(path.name for path in (root / PATCHES).glob('*.patch'))
    if sorted(series) != on_disk:
        failures.append('series and the .patch files differ: series ' + str(series) + ', files ' + str(on_disk))
    if not print_tables and sorted(series) != sorted(patch_table):
        failures.append('series and the PROVENANCE patch table differ')

    parsed = []
    patch_hashes = {}
    for name in series:
        path = root / PATCHES / name
        if not path.is_file():
            failures.append(name + ': listed in series, missing on disk')
            continue
        data = path.read_bytes()
        patch_hashes[name] = sha256(data)
        if not print_tables and patch_table.get(name) != patch_hashes[name]:
            failures.append(name + ': SHA-256 ' + patch_hashes[name] + ' differs from PROVENANCE')
        parsed.append((name, parse_patch(name, data)))

    tree = read_tree(root)
    if sorted(tree) != sorted(release):
        extra = sorted(set(tree) - set(release))
        missing = sorted(set(release) - set(tree))
        failures.append('tree and release table differ: extra ' + str(extra) + ', missing ' + str(missing))

    touched = sorted({file_patch.path for _, file_patches in parsed for file_patch in file_patches})
    for path in touched:
        if path not in release:
            failures.append('a patch changes ' + path + ', which is not in the release table')

    if print_tables:
        for name in series:
            print('| `' + name + '` | `' + patch_hashes.get(name, '?') + '` | (audit ID) | (upstream status) |')
        print()
        for path in touched:
            print('| `' + path + '` | `' + sha256(tree.get(path, b'')) + '` |')
    else:
        if sorted(patched_table) != touched:
            failures.append('the patched-file table names ' + str(sorted(patched_table)) + ', the patches change ' + str(touched))
        for path, data in tree.items():
            expected = patched_table.get(path) if path in touched else release.get(path, (None, None))[1]
            if sha256(data) != expected:
                failures.append(path + ': SHA-256 ' + sha256(data) + ' differs from PROVENANCE (' + str(expected) + ')')

    restored = dict(tree)
    for name, file_patches in reversed(parsed):
        for file_patch in reversed(file_patches):
            if file_patch.path in restored:
                restored[file_patch.path] = reverse_apply(name, file_patch, restored[file_patch.path])
    for path, data in sorted(restored.items()):
        if path not in release:
            continue
        expected_sha1, expected_sha256 = release[path]
        if blob_sha1(data) != expected_sha1 or sha256(data) != expected_sha256:
            failures.append(path + ': with the series reversed, it is not the pinned release')
    return failures


def main(argv):
    if len(argv) not in (2, 3) or (len(argv) == 3 and argv[2] != '--print-tables'):
        print(__doc__.strip().split('\n\n')[-2], file=sys.stderr)
        return 1
    root = pathlib.Path(argv[1])
    try:
        failures = check(root, len(argv) == 3)
    except CheckFailure as failure:
        failures = [str(failure)]
    for failure in failures:
        print(failure)
    if failures:
        return 1
    if len(argv) == 2:
        print('mimalloc: the tree is the pinned release with the series applied')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
