#!/usr/bin/env python3
"""The governance check (R4): STXlib's rules for the standard library as a tool.

Audit B (notes audits/B-std-capabilities.md, section 3.16) sorts the C and
C++ standard libraries into what STXlib owns, wraps, leaves alone and
governs. Governing means a rule: a facility whose result the standard
leaves to the host, the locale, a hidden static state or the library's own
policy is either not used, or used only under a condition. This tool checks
the rules that can be read from source text, over STXlib's own sources and
over any consumer's tree.

Usage:
  python3 -I tools/check_std_rules.py [options] PATH...

PATH is a file or a directory (searched for C and C++ sources). Options:
  --profile consumer   the rules every program built on STXlib is offered
                       (the default)
  --profile stxlib     those rules, plus the ones STXlib holds itself to
                       (no streams, no std::format, no assert, no locale at
                       all, no recursive or timed mutex, a lock-free check
                       for every atomic type)
  --root DIR           the directory that --host-path and --exclude
                       patterns are relative to (default: the current one)
  --host-path GLOB     host code: the rules marked "host code only" do not
                       apply there (repeatable; the stxlib profile adds
                       src/hosted/*)
  --exclude GLOB       a path not checked at all (repeatable; for vendored
                       code)
  --list-rules         print the rules and exit

A finding prints as `file:line: [rule] what, instead: replacement`. Exit
status 0 when there is none, 1 when there are findings, 2 on a usage error.

Allowing one use: a comment on the line, or alone on the line above,
  // std-rules: allow(rand) a reason, which is required
names the rules allowed there, separated by commas. A suppression without a
reason, or naming no known rule, is itself a finding.

How the text is read: comments, string literals and character literals are
blanked before the names are matched (line numbers are kept), so prose may
name anything. Includes are matched on the raw line. A name is matched as a
call or a qualified name: `rand(` and `std::rand` are findings, a member
`engine.rand(` or `widget->system(` and a longer name `operand(` are not.
The check reads text, not the program: a macro that expands to a forbidden
call, or a call through a function pointer, is not seen. It is a lint, not
a proof; what it reports is certain, what it passes is not.

What no text check can see, and how STXlib covers it instead:
  - `sort` only with a total order: the test builds define libc++'s
    _LIBCPP_DEBUG_RANDOMIZE_UNSPECIFIED_STABILITY (CMakeLists.txt,
    STXLIB_RANDOMIZE_UNSTABLE_ORDER), which shuffles before every unstable
    sort, nth_element and partial_sort, so reliance on an unspecified order
    fails a test;
  - a std::hash value never observed, and no observed iteration order of an
    unordered container: review;
  - the throwing accessors of std::string_view and the containers under no
    exceptions: review (the build's -fno-exceptions turns a throw into a
    process end, so the result is defined but abrupt).
"""

from __future__ import annotations

import argparse
import bisect
import dataclasses
import fnmatch
import pathlib
import re
import sys

SOURCE_SUFFIXES: frozenset[str] = frozenset(
    ('.c', '.cc', '.cpp', '.cxx', '.c++', '.h', '.hh', '.hpp', '.hxx', '.h++', '.inl', '.ipp', '.tpp', '.m', '.mm'))

# A call of a free function: not a member (`.`, `->`), not part of a longer
# name, and not qualified by anything but std:: (matched separately).
CALL_PREFIX: str = r'(?<![\w.>:])'


def calls(*names: str) -> str:
    """A pattern for a call of any of `names`, bare or through std::."""
    alternatives: str = '|'.join(names)
    return rf'(?:{CALL_PREFIX}|\bstd\s*::\s*)(?:{alternatives})\s*\('


def qualified(*names: str) -> str:
    """A pattern for std::<name> (a type or object)."""
    alternatives: str = '|'.join(names)
    return rf'\bstd\s*::\s*(?:{alternatives})\b'


def includes(*headers: str) -> str:
    """A pattern for an include of any of `headers` (angle brackets or quotes)."""
    alternatives: str = '|'.join(re.escape(header) for header in headers)
    return rf'^\s*#\s*include\s*[<"](?:{alternatives})[>"]'


@dataclasses.dataclass(frozen=True)
class Rule:
    identifier: str
    profile: str  # 'consumer' (also checked for STXlib) or 'stxlib'
    what: str
    instead: str
    names: tuple[str, ...] = ()  # patterns over the stripped text
    headers: tuple[str, ...] = ()  # patterns over the raw lines
    host_only: bool = False  # allowed in host code


RULES: tuple[Rule, ...] = (
    Rule('regex', 'consumer',
         'std::regex: matching time and stack depth are the library\'s and can be exponential; the classes follow '
         'the global locale; it allocates and throws',
         'TextView search (R3), or a glob for file-name patterns',
         names=(qualified('regex', 'wregex', 'basic_regex', r'regex_\w+'),),
         headers=(includes('regex'),)),
    Rule('rand', 'consumer',
         'rand and srand: a hidden global state, and RAND_MAX and the generator differ by C library',
         'stxlib/random.hpp (and stxlib/entropy.hpp for a seed)',
         names=(calls('rand', 'srand', 'rand_r'), r'\bRAND_MAX\b')),
    Rule('strtok', 'consumer',
         'strtok keeps its position in a hidden static',
         'TextScanner (stxlib/numeric_text.hpp) or TextView search',
         names=(calls('strtok', 'wcstok'),)),
    Rule('strerror', 'consumer',
         'strerror returns a static buffer, and strerror_r has two incompatible signatures (GNU and XSI)',
         'ErrorRecord and ForeignCode (stxlib/error.hpp)',
         names=(calls('strerror'),)),
    Rule('broken-down-time', 'consumer',
         'localtime and gmtime return a static buffer and read TZ',
         'stxlib/local_time.hpp and stxlib/calendar.hpp',
         names=(calls('localtime', 'gmtime'),)),
    Rule('clock', 'consumer',
         'clock() is CPU time on POSIX and wall time on Windows',
         'stxlib/clocks.hpp (interval_now, or a process CPU clock where one is offered)',
         names=(rf'(?:{CALL_PREFIX}|\bstd\s*::\s*)clock\s*\(\s*\)', r'\bCLOCKS_PER_SEC\b')),
    Rule('setjmp', 'consumer',
         'setjmp and longjmp skip destructors, and a jump would cross the gate',
         'a Result returned up the stack',
         names=(r'\b(?:setjmp|longjmp|sigsetjmp|siglongjmp|_setjmp|_longjmp|jmp_buf)\b',),
         headers=(includes('csetjmp', 'setjmp.h'),)),
    Rule('qsort', 'consumer',
         'qsort and bsearch: no type safety, and the order of equal elements is unspecified',
         'std::stable_sort, or std::sort with a total order; std::lower_bound',
         names=(calls('qsort', 'qsort_r', 'qsort_s', 'bsearch', 'bsearch_s'),)),
    Rule('ato', 'consumer',
         'atoi, atol, atoll and atof: undefined behaviour on overflow and no failure report',
         'parse_* in stxlib/numeric_text.hpp',
         names=(calls('atoi', 'atol', 'atoll', 'atof', '_wtoi', '_wtol', '_wtof'),)),
    Rule('tmpnam', 'consumer',
         'tmpnam, tempnam and mktemp name a file that another process can create first',
         'a file created with exclusive creation in a directory the program owns (R10)',
         names=(calls('tmpnam', 'tmpnam_r', 'tmpnam_s', 'tempnam', '_tempnam', 'mktemp', '_mktemp', '_mktemp_s'),)),
    Rule('system', 'consumer',
         'system and popen run a shell, whose parsing differs by host',
         'stxlib/children.hpp (an argument list, no shell)',
         names=(calls('system', '_wsystem', 'popen', '_popen', '_wpopen'),)),
    Rule('locale-change', 'consumer',
         'changing the locale changes number text, character classes and collation for the whole process',
         'the classic "C" locale for machine text; STXlib is locale-free (D9 a)',
         names=(calls('setlocale', '_wsetlocale', 'uselocale', '_configthreadlocale'),
                r'\blocale\s*::\s*global\s*\(', r'(?:\.|->)\s*imbue\s*\(')),
    Rule('locale-text', 'consumer',
         'strcoll, strxfrm and the multibyte conversions follow the locale',
         'byte comparison of UTF-8, and STXlib\'s transcoding (R15)',
         names=(calls('strcoll', 'strxfrm', 'wcscoll', 'wcsxfrm', 'mblen', 'mbtowc', 'wctomb', 'mbstowcs',
                      'wcstombs', 'mbrlen', 'mbrtowc', 'wcrtomb', 'mbsrtowcs', 'wcsrtombs', 'btowc', 'wctob'),)),
    Rule('fenv', 'consumer',
         'the floating-point environment is per thread, ignored by compilers without FENV_ACCESS, and traps '
         'are host-specific; STXlib\'s numerics assume round to nearest and no traps',
         'the default environment, never changed',
         names=(r'\b(?:fesetround|fegetround|fesetenv|fegetenv|feholdexcept|feupdateenv|feclearexcept|'
                r'feraiseexcept|fetestexcept|fesetexceptflag|fegetexceptflag|feenableexcept|fedisableexcept|'
                r'_controlfp|_controlfp_s|_control87|__control87_2)\b', r'\bFENV_ACCESS\b'),
         headers=(includes('cfenv', 'fenv.h'),)),
    Rule('execution-policy', 'consumer',
         'parallel algorithms: the backend and thread count are the library\'s, and reductions regroup '
         'floating-point sums',
         'the sequential algorithm, or AtomicPool (R8)',
         names=(r'\bstd\s*::\s*execution\s*::',),
         headers=(includes('execution'),)),
    Rule('special-functions', 'consumer',
         'the special math functions: quality varies widely by library, and libc++ implements few',
         'a function STXlib certifies, on need',
         names=(qualified(r'(?:assoc_laguerre|assoc_legendre|beta|comp_ellint_[123]|cyl_bessel_[ijk]|cyl_neumann|'
                          r'ellint_[123]|expint|hermite|laguerre|legendre|riemann_zeta|sph_bessel|sph_legendre|'
                          r'sph_neumann)[fl]?'),)),
    Rule('valarray', 'consumer',
         'valarray: aliasing and expression-template freedoms are the library\'s',
         'a plain array and a loop',
         names=(qualified('valarray', 'slice_array', 'gslice_array', 'mask_array', 'indirect_array', 'gslice',
                          'slice'),),
         headers=(includes('valarray'),)),
    Rule('unbounded-copy', 'consumer',
         'strcpy, strcat and gets write without a bound, and strncpy can leave no terminator',
         'a View copy with its count, or TextWriter',
         names=(calls('strcpy', 'strcat', 'strncpy', 'wcscpy', 'wcscat', 'wcsncpy', 'gets'),)),
    Rule('c-threads', 'consumer',
         'C11 threads.h: absent on Apple',
         'stxlib/threads.hpp',
         names=(r'\b(?:thrd_\w+|mtx_(?:init|lock|timedlock|trylock|unlock|destroy)|cnd_\w+|tss_\w+)\s*\(',),
         headers=(includes('threads.h'),)),
    Rule('pthread', 'consumer',
         'pthread outside the host layer: absent on Windows',
         'stxlib/threads.hpp',
         names=(rf'{CALL_PREFIX}pthread_\w+\s*\(',),
         headers=(includes('pthread.h'),),
         host_only=True),
    Rule('wide-text', 'consumer',
         'wchar_t is 16 bits on Windows and 32 elsewhere, and the wide functions follow the locale',
         'UTF-8 TextView; host code converts at the edge',
         names=(r'\bwchar_t\b', qualified('wstring', 'wstring_view', r'w(?:i|o|io)?stream', 'wcout', 'wcerr'),),
         headers=(includes('cwchar', 'wchar.h', 'cwctype', 'wctype.h', 'cuchar', 'uchar.h'),),
         host_only=True),
    # STXlib's own rules.
    Rule('streams', 'stxlib',
         'no streams inside STXlib: the global locale, imbue and stream state',
         'stxlib/console.hpp, stxlib/files.hpp, TextWriter and TextScanner',
         names=(qualified('cout', 'cerr', 'clog', 'cin', r'(?:i|o)?fstream', r'(?:i|o)?stringstream',
                          r'(?:i|o)?spanstream', 'osyncstream', 'istream', 'ostream', 'iostream'),),
         headers=(includes('iostream', 'fstream', 'sstream', 'iomanip', 'syncstream', 'spanstream', 'istream',
                           'ostream'),)),
    Rule('format', 'stxlib',
         'std::format and std::print are not used in STXlib (a runtime format string throws, format allocates, '
         'and the L flag reads the locale)',
         'format_* and Field in stxlib/numeric_text.hpp',
         names=(qualified('format', 'vformat', 'format_to', 'format_to_n', 'vformat_to', 'print', 'println',
                          'vprint_unicode', 'vprint_nonunicode'),),
         headers=(includes('format', 'print'),)),
    Rule('assert', 'stxlib',
         'assert depends on NDEBUG, prints a host format and calls abort',
         'STXLIB_REQUIRE (stxlib/require.hpp)',
         names=(rf'{CALL_PREFIX}assert\s*\(',),
         headers=(includes('cassert', 'assert.h'),)),
    Rule('locale-use', 'stxlib',
         'STXlib is locale-free (D9 a): no locale is read either',
         'the classic "C" behaviour, written out',
         names=(qualified('locale', 'use_facet', 'has_facet', 'numpunct', 'ctype'), calls('localeconv', 'newlocale')),
         headers=(includes('locale', 'clocale', 'locale.h', 'xlocale.h'),)),
    Rule('recursive-mutex', 'stxlib',
         'recursive and timed mutexes are not used in STXlib (recursion is a design smell the house rules avoid)',
         'stxlib/threads.hpp\'s Mutex',
         names=(qualified('recursive_mutex', 'recursive_timed_mutex', 'timed_mutex', 'shared_timed_mutex'),)),
)

RULES_BY_IDENTIFIER: dict[str, Rule] = {rule.identifier: rule for rule in RULES}

# The lock-free rule (stxlib profile): every atomic type named must have
# is_always_lock_free asserted somewhere in the checked tree.
LOCK_FREE_RULE: str = 'atomic-lock-free'
ATOMIC_OPEN: re.Pattern[str] = re.compile(r'\bstd\s*::\s*(atomic_ref|atomic)\s*<')
ATOMIC_ASSERT_TAIL: re.Pattern[str] = re.compile(r'\s*::\s*is_always_lock_free\b')
TYPE_ALIASES: dict[str, str] = {
    'u8': 'uint8', 'std::uint8_t': 'uint8', 'uint8_t': 'uint8', 'unsignedchar': 'uint8',
    'u16': 'uint16', 'std::uint16_t': 'uint16', 'uint16_t': 'uint16', 'unsignedshort': 'uint16',
    'u32': 'uint32', 'std::uint32_t': 'uint32', 'uint32_t': 'uint32', 'unsigned': 'uint32', 'unsignedint': 'uint32',
    'u64': 'uint64', 'std::uint64_t': 'uint64', 'uint64_t': 'uint64',
    'i8': 'int8', 'std::int8_t': 'int8', 'int8_t': 'int8', 'signedchar': 'int8',
    'i16': 'int16', 'std::int16_t': 'int16', 'int16_t': 'int16', 'short': 'int16',
    'i32': 'int32', 'std::int32_t': 'int32', 'int32_t': 'int32', 'int': 'int32',
    'i64': 'int64', 'std::int64_t': 'int64', 'int64_t': 'int64',
    'usize': 'size', 'std::size_t': 'size', 'size_t': 'size',
    'uptr': 'uintptr', 'std::uintptr_t': 'uintptr', 'uintptr_t': 'uintptr',
}

SUPPRESSION: re.Pattern[str] = re.compile(r'std-rules:\s*allow\(([^)]*)\)\s*(.*)$')
SUPPRESSION_MARK: re.Pattern[str] = re.compile(r'std-rules:')


# One left-to-right pass over the text (re.sub), each alternative tried at a
# position in this order, exactly the cases of the character loop it replaced:
# a line comment (removed up to its line break), a block comment (its line
# breaks kept; an unterminated one runs to the end), a raw string literal not
# preceded by a letter, digit or underscore (one space and its line breaks; an
# unterminated one runs to the end), a digit separator (a quote between two
# alphanumerics: one space), and a quoted literal (one space; a backslash
# takes the next character, line break included, and an unescaped line break
# ends the literal and is consumed with it). Anything else is kept.
STRIPPED_LEXEMES: re.Pattern[str] = re.compile(
    r'(?P<line>//[^\n]*)'
    r'|(?P<block>/\*(?s:.*?)(?:\*/|\Z))'
    r'|(?P<raw>(?<!\w)R"(?P<delimiter>[^()\\ \n]{0,16})\((?s:.*?)(?:\)(?P=delimiter)"|\Z))'
    r"|(?P<separator>(?<=[^\W_])'(?=[^\W_]))"
    r'|(?P<literal>"(?:[^"\\\n]|\\(?s:.)|\\\Z)*(?:"|\n|\Z)'
    r"|'(?:[^'\\\n]|\\(?s:.)|\\\Z)*(?:'|\n|\Z))")


def stripped_lexeme(match: re.Match[str]) -> str:
    kind: str | None = match.lastgroup
    if kind == 'line':
        return ''
    if kind == 'block':
        return '\n' * match.group(0).count('\n')
    if kind == 'raw':
        return ' ' + '\n' * match.group(0).count('\n')
    # A digit separator or a quoted literal.
    return ' '


def strip_comments_and_literals(text: str) -> str:
    """Blanks comments and literals, keeping line breaks so line numbers hold.

    The same reading as tools/check_public_headers.py, plus raw string
    literals (R"delimiter( ... )delimiter"). STRIPPED_LEXEMES lists the cases.
    """
    return STRIPPED_LEXEMES.sub(stripped_lexeme, text)


# Whole-text matching. A rule's pattern is written for one line at a time; it
# is searched once over the file's lines joined with '\n' (re.MULTILINE, so ^
# and $ still mean a line's ends) when it can be rewritten so that no match or
# lookaround can cross or consume a line break: \s becomes [^\S\n], \W and \D
# become [^\w\n] and [^\d\n], and a negated class also refuses '\n'. A '\n'
# then reads like a line's ends to everything else (\b, lookbehinds and
# lookaheads over the rewritten classes), so the lines holding a match are
# exactly the lines a search of each line finds. A pattern using anything this
# rewriting does not cover (\A, \Z, \B, escapes naming a character or group by
# number, inline flags other than i, a class holding \s, \W or \D outside a
# negation) is searched line by line as before.
LINE_SAFE_FLAGS: frozenset[str] = frozenset('iaLu')


def line_safe(pattern: str) -> str | None:
    """The pattern rewritten for whole-text matching, or None when it must stay per line."""
    result: list[str] = []
    index: int = 0
    length: int = len(pattern)
    in_class: bool = False
    negated: bool = False
    class_start: int = 0
    while index < length:
        character: str = pattern[index]
        if character == '\\':
            if index + 1 >= length:
                return None
            escaped: str = pattern[index + 1]
            if escaped.isdigit() or escaped in 'AZBxuUNnrfvtag0':
                return None
            if in_class:
                if escaped in 'sWD' and not negated:
                    return None
                result.append(character + escaped)
            elif escaped == 's':
                result.append('[^\\S\\n]')
            elif escaped == 'W':
                result.append('[^\\w\\n]')
            elif escaped == 'D':
                result.append('[^\\d\\n]')
            else:
                result.append(character + escaped)
            index += 2
            continue
        if in_class:
            if character == ']' and index > class_start:
                if negated:
                    result.append('\\n')
                result.append(']')
                in_class = False
                index += 1
                continue
            if character == '\n':
                return None
            result.append(character)
            index += 1
            continue
        if character == '[':
            in_class = True
            negated = index + 1 < length and pattern[index + 1] == '^'
            result.append('[^' if negated else '[')
            index += 2 if negated else 1
            # A ']' first in a class is a member, not its end.
            class_start = index
            continue
        if character == '(' and pattern.startswith('(?', index):
            following: str = pattern[index + 2:index + 3]
            if following.isalpha() or following == '-':
                flags: str = ''
                cursor: int = index + 2
                while cursor < length and (pattern[cursor].isalpha() or pattern[cursor] == '-'):
                    flags += pattern[cursor]
                    cursor += 1
                if not set(flags.replace('-', '')) <= LINE_SAFE_FLAGS:
                    return None
        if character == '\n':
            return None
        result.append(character)
        index += 1
    if in_class:
        return None
    return ''.join(result)


@dataclasses.dataclass(frozen=True)
class JoinedLines:
    """A file's lines (str.splitlines) joined with '\n', and where each line starts."""
    text: str
    starts: list[int]


def joined_lines(lines: list[str]) -> JoinedLines:
    starts: list[int] = []
    offset: int = 0
    line: str
    for line in lines:
        starts.append(offset)
        offset += len(line) + 1
    return JoinedLines('\n'.join(lines), starts)


@dataclasses.dataclass(frozen=True)
class LinePattern:
    """A rule's pattern, with its whole-text form when it has one."""
    per_line: re.Pattern[str]
    whole: re.Pattern[str] | None


def line_pattern(pattern: str) -> LinePattern:
    safe: str | None = line_safe(pattern)
    whole: re.Pattern[str] | None = None if safe is None else re.compile(safe, re.MULTILINE)
    return LinePattern(re.compile(pattern), whole)


def matching_lines(pattern: LinePattern, lines: list[str], joined: JoinedLines) -> list[int]:
    """The 1-based numbers of the lines a search of each line finds, in order, each once."""
    numbers: list[int] = []
    if not lines:
        return numbers
    if pattern.whole is None:
        line_number: int
        line: str
        for line_number, line in enumerate(lines, start=1):
            if pattern.per_line.search(line):
                numbers.append(line_number)
        return numbers
    match: re.Match[str]
    for match in pattern.whole.finditer(joined.text):
        number: int = bisect.bisect_right(joined.starts, match.start())
        if not numbers or numbers[-1] != number:
            numbers.append(number)
    return numbers


@dataclasses.dataclass
class Finding:
    path: str
    line: int
    rule: str
    message: str

    def text(self) -> str:
        return f'{self.path}:{self.line}: [{self.rule}] {self.message}'


@dataclasses.dataclass
class Suppressions:
    by_line: dict[int, frozenset[str]]
    findings: list[Finding]


def read_suppressions(path_text: str, lines: list[str]) -> Suppressions:
    """The rules allowed on each line, and the malformed suppressions."""
    by_line: dict[int, frozenset[str]] = {}
    findings: list[Finding] = []
    line_number: int
    line: str
    for line_number, line in enumerate(lines, start=1):
        if SUPPRESSION_MARK.search(line) is None:
            continue
        match: re.Match[str] | None = SUPPRESSION.search(line)
        if match is None:
            findings.append(Finding(path_text, line_number, 'suppression', 'write std-rules: allow(<rule>) <reason>'))
            continue
        names: list[str] = [name.strip() for name in match.group(1).split(',') if name.strip()]
        reason: str = match.group(2).strip().rstrip('*/').strip()
        known: list[str] = [name for name in names if name in RULES_BY_IDENTIFIER or name == LOCK_FREE_RULE]
        unknown: list[str] = [name for name in names if name not in known]
        if unknown or not names:
            findings.append(Finding(path_text, line_number, 'suppression',
                                    f'no such rule: {", ".join(unknown) if unknown else "(none named)"}'))
        if not reason:
            findings.append(Finding(path_text, line_number, 'suppression', 'a suppression needs a reason'))
            continue
        allowed: frozenset[str] = frozenset(known)
        by_line[line_number] = by_line.get(line_number, frozenset()) | allowed
        # A comment alone on its line covers the next line too.
        code_before: str = line[:match.start()].strip()
        if code_before in ('//', '/*', ''):
            by_line[line_number + 1] = by_line.get(line_number + 1, frozenset()) | allowed
    return Suppressions(by_line, findings)


def line_of(text: str, offset: int) -> int:
    return text.count('\n', 0, offset) + 1


def matching_angle(text: str, start: int) -> int:
    """The index of the '>' that closes the '<' just before `start`, or -1."""
    depth: int = 1
    index: int = start
    while index < len(text):
        character: str = text[index]
        if character == '<':
            depth += 1
        elif character == '>':
            depth -= 1
            if depth == 0:
                return index
        elif character in ';{}':
            return -1
        index += 1
    return -1


def normal_type(argument: str) -> str:
    compact: str = re.sub(r'\bconst\b|\bvolatile\b', '', argument)
    compact = re.sub(r'\s+', '', compact)
    if compact.endswith('*'):
        # Every object pointer is one size; one assert covers them all.
        return 'pointer'
    return TYPE_ALIASES.get(compact, compact)


@dataclasses.dataclass
class AtomicUse:
    path: str
    line: int
    kind: str  # 'atomic' or 'atomic_ref'
    type_name: str


def atomic_uses(path_text: str, stripped: str) -> tuple[list[AtomicUse], set[tuple[str, str]]]:
    """The atomic types named in a file, and those with is_always_lock_free asserted."""
    uses: list[AtomicUse] = []
    asserted: set[tuple[str, str]] = set()
    match: re.Match[str]
    for match in ATOMIC_OPEN.finditer(stripped):
        close: int = matching_angle(stripped, match.end())
        if close < 0:
            continue
        type_name: str = normal_type(stripped[match.end():close])
        kind: str = match.group(1)
        if ATOMIC_ASSERT_TAIL.match(stripped, close + 1):
            asserted.add((kind, type_name))
        else:
            uses.append(AtomicUse(path_text, line_of(stripped, match.start()), kind, type_name))
    return uses, asserted


@dataclasses.dataclass(frozen=True)
class CompiledRule:
    rule: Rule
    names: tuple[LinePattern, ...]
    headers: tuple[LinePattern, ...]


def compile_rules(profile: str) -> list[CompiledRule]:
    chosen: list[CompiledRule] = []
    rule: Rule
    for rule in RULES:
        if rule.profile == 'stxlib' and profile != 'stxlib':
            continue
        chosen.append(CompiledRule(rule, tuple(line_pattern(pattern) for pattern in rule.names),
                                   tuple(line_pattern(pattern) for pattern in rule.headers)))
    return chosen


def matches_any(relative: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(relative, pattern) for pattern in patterns)


def check_file(path: pathlib.Path, path_text: str, rules: list[CompiledRule], host_code: bool,
               profile: str) -> tuple[list[Finding], list[AtomicUse], set[tuple[str, str]], dict[int, frozenset[str]]]:
    original: str = path.read_text(encoding='utf-8', errors='replace')
    lines: list[str] = original.splitlines()
    suppressions: Suppressions = read_suppressions(path_text, lines)
    findings: list[Finding] = list(suppressions.findings)
    stripped: str = strip_comments_and_literals(original)
    stripped_lines: list[str] = stripped.splitlines()
    joined: JoinedLines = joined_lines(lines)
    joined_stripped: JoinedLines = joined_lines(stripped_lines)
    seen: set[tuple[int, str]] = set()
    compiled: CompiledRule
    for compiled in rules:
        rule: Rule = compiled.rule
        if rule.host_only and host_code:
            continue
        hits: list[int] = []
        pattern: LinePattern
        for pattern in compiled.headers:
            hits.extend(matching_lines(pattern, lines, joined))
        for pattern in compiled.names:
            hits.extend(matching_lines(pattern, stripped_lines, joined_stripped))
        line_number: int
        for line_number in hits:
            if (line_number, rule.identifier) in seen:
                continue
            seen.add((line_number, rule.identifier))
            if rule.identifier in suppressions.by_line.get(line_number, frozenset()):
                continue
            where: str = ' (host code only)' if rule.host_only else ''
            findings.append(Finding(path_text, line_number, rule.identifier,
                                    f'{rule.what}{where}; instead: {rule.instead}'))
    uses: list[AtomicUse] = []
    asserted: set[tuple[str, str]] = set()
    if profile == 'stxlib':
        uses, asserted = atomic_uses(path_text, stripped)
    return findings, uses, asserted, suppressions.by_line


def gather(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    path: pathlib.Path
    for path in paths:
        if path.is_dir():
            files.extend(sorted(candidate for candidate in path.rglob('*')
                                if candidate.is_file() and candidate.suffix.lower() in SOURCE_SUFFIXES))
        elif path.is_file():
            files.append(path)
    return files


def relative_text(path: pathlib.Path, root: pathlib.Path) -> str:
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def run(arguments: argparse.Namespace) -> int:
    root: pathlib.Path = pathlib.Path(arguments.root)
    host_paths: list[str] = list(arguments.host_path)
    if arguments.profile == 'stxlib':
        host_paths.append('src/hosted/*')
    rules: list[CompiledRule] = compile_rules(arguments.profile)
    findings: list[Finding] = []
    all_uses: list[AtomicUse] = []
    all_asserted: set[tuple[str, str]] = set()
    allowed_lines: dict[str, dict[int, frozenset[str]]] = {}
    files: list[pathlib.Path] = gather([pathlib.Path(path) for path in arguments.paths])
    if not files:
        print('check_std_rules: no C or C++ sources under the paths given', file=sys.stderr)
        return 2
    checked: int = 0
    path: pathlib.Path
    for path in files:
        relative: str = relative_text(path, root)
        if matches_any(relative, arguments.exclude):
            continue
        checked += 1
        host_code: bool = matches_any(relative, host_paths)
        file_findings, uses, asserted, by_line = check_file(path, relative, rules, host_code, arguments.profile)
        findings.extend(file_findings)
        all_uses.extend(uses)
        all_asserted |= asserted
        allowed_lines[relative] = by_line
    reported: set[tuple[str, str]] = set()
    use: AtomicUse
    for use in all_uses:
        key: tuple[str, str] = (use.kind, use.type_name)
        if key in all_asserted or key in reported:
            continue
        if LOCK_FREE_RULE in allowed_lines.get(use.path, {}).get(use.line, frozenset()):
            continue
        reported.add(key)
        findings.append(Finding(use.path, use.line, LOCK_FREE_RULE,
                                f'std::{use.kind}<{use.type_name}> has no static_assert of is_always_lock_free in '
                                'the checked tree (a type that is not lock-free falls back to a lock in a separate '
                                'library); instead: assert it beside the others in src/core/atomic_rules.cpp'))
    finding: Finding
    for finding in findings:
        print(finding.text())
    if findings:
        print(f'check_std_rules: {len(findings)} finding(s) in {checked} file(s), profile {arguments.profile}',
              file=sys.stderr)
        return 1
    print(f'check_std_rules: {checked} file(s), profile {arguments.profile}, no finding')
    return 0


def list_rules() -> int:
    rule: Rule
    for rule in RULES:
        scope: str = 'STXlib only' if rule.profile == 'stxlib' else 'everyone'
        host: str = ', allowed in host code' if rule.host_only else ''
        print(f'{rule.identifier} ({scope}{host}): {rule.what}; instead: {rule.instead}')
    print(f'{LOCK_FREE_RULE} (STXlib only): every std::atomic and std::atomic_ref type named has '
          'is_always_lock_free asserted somewhere in the checked tree')
    return 0


def main() -> int:
    parser: argparse.ArgumentParser = argparse.ArgumentParser(
        description='The governance check (R4): STXlib\'s rules for the standard library.')
    parser.add_argument('--profile', choices=('consumer', 'stxlib'), default='consumer')
    parser.add_argument('--root', default='.')
    parser.add_argument('--host-path', action='append', default=[])
    parser.add_argument('--exclude', action='append', default=[])
    parser.add_argument('--list-rules', action='store_true')
    parser.add_argument('paths', nargs='*')
    arguments: argparse.Namespace = parser.parse_args()
    if arguments.list_rules:
        return list_rules()
    if not arguments.paths:
        parser.print_usage(sys.stderr)
        return 2
    return run(arguments)


if __name__ == '__main__':
    sys.exit(main())
