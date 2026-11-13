#!/usr/bin/env python3
"""Checks STXlib's public headers against AGENTS.md's hard boundary.

Every header under include/stxlib/ must name no std:: entity and include no
header in angle brackets (no standard, platform or kernel header): public
types have a layout STXlib defines. Comments, string literals and character
literals are removed before the check, so prose may mention std:: freely.

Usage: python3 -I tools/check_public_headers.py <repository root>
Exit status 0 when every header passes, 1 otherwise (each finding printed
as file:line: reason).
"""

import pathlib
import re
import sys

STD_NAME = re.compile(r'\bstd\s*::')
ANGLE_INCLUDE = re.compile(r'^\s*#\s*include\s*<')


def strip_comments_and_literals(text: str) -> str:
    """Blanks comments and literals, keeping line breaks so line numbers hold."""
    result: list[str] = []
    index: int = 0
    length: int = len(text)
    while index < length:
        character: str = text[index]
        following: str = text[index + 1] if index + 1 < length else ''
        if character == '/' and following == '/':
            while index < length and text[index] != '\n':
                index += 1
            continue
        if character == '/' and following == '*':
            index += 2
            while index < length and not (text[index] == '*' and index + 1 < length and text[index + 1] == '/'):
                if text[index] == '\n':
                    result.append('\n')
                index += 1
            index += 2
            continue
        if character in ('"', "'"):
            quote: str = character
            result.append(' ')
            index += 1
            while index < length and text[index] != quote:
                if text[index] == '\\':
                    index += 1
                elif text[index] == '\n':
                    break
                index += 1
            index += 1
            continue
        result.append(character)
        index += 1
    return ''.join(result)


def check(root: pathlib.Path) -> list[str]:
    findings: list[str] = []
    headers: list[pathlib.Path] = sorted((root / 'include' / 'stxlib').rglob('*.hpp'))
    if not headers:
        findings.append(f'{root}: no public headers found under include/stxlib')
    for header in headers:
        original: str = header.read_text(encoding='utf-8')
        # Includes are checked on the raw lines: the path of an angle include
        # would be blanked as a literal only if quoted, which it is not.
        line_number: int
        line: str
        for line_number, line in enumerate(original.splitlines(), start=1):
            if ANGLE_INCLUDE.match(line):
                findings.append(f'{header}:{line_number}: includes a header in angle brackets')
        stripped: str = strip_comments_and_literals(original)
        for line_number, line in enumerate(stripped.splitlines(), start=1):
            if STD_NAME.search(line):
                findings.append(f'{header}:{line_number}: names std::')
    return findings


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_public_headers.py <repository root>', file=sys.stderr)
        return 2
    findings: list[str] = check(pathlib.Path(sys.argv[1]))
    finding: str
    for finding in findings:
        print(finding)
    if findings:
        return 1
    print('public headers: no std:: name and no angle-bracket include')
    return 0


if __name__ == '__main__':
    sys.exit(main())
