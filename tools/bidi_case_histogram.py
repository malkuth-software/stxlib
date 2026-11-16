#!/usr/bin/env python3
"""Measures how text falls into the bidi fast forms (src/algorithms/textlayout/).

Usage:
  python3 -I tools/bidi_case_histogram.py <DerivedBidiClass.txt> <corpus file or directory>...

Each corpus file is read as UTF-8. From a C or C++ source (.c, .cc, .cpp,
.h, .hpp), only the string literals are taken; other files are taken
whole. Text is split into paragraphs at Bidi_Class B (CR LF as one).

Reported:
  - paragraphs and code points by form: A (left to right only: no R, AL,
    AN and no explicit formatting character), B (implicit: no explicit
    formatting character), C (explicit embeddings, overrides or isolates);
  - the histogram of Bidi_Class over all code points;
  - for forms B and C: code points per paragraph, class runs per
    paragraph (consecutive code points of one class), and the mean run
    length, which is what the run-list engine saves over a per-character
    pass.
"""

import pathlib
import re
import sys

CLASSES = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
           'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI']
EXPLICIT = {'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI'}
RIGHT = {'R', 'AL', 'AN'}
SOURCE = {'.c', '.cc', '.cpp', '.h', '.hpp'}
LITERAL = re.compile(r'(?:u8|u|U|L)?"((?:[^"\\\n]|\\.)*)"')


def read_classes(path: str) -> list[str]:
    classes = ['L'] * 0x110000
    names = {'Left_To_Right': 'L', 'Right_To_Left': 'R', 'Arabic_Letter': 'AL',
             'European_Terminator': 'ET', 'Boundary_Neutral': 'BN'}
    text = pathlib.Path(path).read_text(encoding='utf-8')
    for raw in text.splitlines():
        if raw.startswith('# @missing:'):
            span, value = [piece.strip() for piece in raw[11:].split('#', 1)[0].split(';')]
            low, high = [int(piece, 16) for piece in span.split('..')]
            for code in range(low, high + 1):
                classes[code] = names.get(value, value)
    for raw in text.splitlines():
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        span, value = [piece.strip() for piece in line.split(';')]
        low, high = span.split('..') if '..' in span else (span, span)
        for code in range(int(low, 16), int(high, 16) + 1):
            classes[code] = value
    return classes


def texts_of(path: pathlib.Path):
    try:
        data = path.read_text(encoding='utf-8')
    except (UnicodeDecodeError, OSError):
        return
    if path.suffix in SOURCE:
        for match in LITERAL.finditer(data):
            body = match.group(1)
            # Escapes are kept as written except the common line breaks.
            yield body.replace('\\n', '\n').replace('\\r', '\r').replace('\\t', '\t')
    else:
        yield data


def paragraphs(text: str, classes: list[str]):
    start = 0
    index = 0
    while index < len(text):
        if classes[ord(text[index])] == 'B':
            end = index + 1
            if text[index] == '\r' and end < len(text) and text[end] == '\n':
                end += 1
            yield text[start:end]
            start = end
            index = end
            continue
        index += 1
    if start < len(text):
        yield text[start:]


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__.strip().split('\n\n')[1], file=sys.stderr)
        return 2
    classes = read_classes(argv[1])
    histogram = {name: 0 for name in CLASSES}
    forms = {'A': [0, 0], 'B': [0, 0], 'C': [0, 0]}
    runs = {'B': [0, 0], 'C': [0, 0]}
    files = []
    for argument in argv[2:]:
        root = pathlib.Path(argument)
        files.extend(sorted(p for p in root.rglob('*') if p.is_file()) if root.is_dir() else [root])
    for path in files:
        for text in texts_of(path):
            for paragraph in paragraphs(text, classes):
                kinds = [classes[ord(c)] for c in paragraph]
                for kind in kinds:
                    histogram[kind] += 1
                present = set(kinds)
                form = 'C' if present & EXPLICIT else ('B' if present & RIGHT else 'A')
                forms[form][0] += 1
                forms[form][1] += len(kinds)
                if form != 'A':
                    count = sum(1 for i, kind in enumerate(kinds) if i == 0 or kinds[i - 1] != kind)
                    runs[form][0] += count
                    runs[form][1] += len(kinds)
    total = sum(histogram.values()) or 1
    print(f'files {len(files)}, code points {total}')
    for form, (count, points) in forms.items():
        print(f'form {form}: {count} paragraphs, {points} code points ({100.0 * points / total:.2f}%)')
    for form, (count, points) in runs.items():
        if count:
            print(f'form {form}: {count} class runs, mean run length {points / count:.2f} code points')
    print('classes: ' + ', '.join(f'{name} {100.0 * value / total:.3f}%' for name, value in histogram.items() if value))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
