#!/usr/bin/env python3
"""Tests of tools/check_std_rules.py (R4, the governance check).

Usage: python3 -I tests/check_std_rules_tests.py <repository root>

Each case writes a small source tree to a temporary directory, runs the
check in a child process, and compares the rules reported (rule and line)
with the expected set. Properties:
  G1  every rule fires on a plain use (a call, a qualified name, an include);
  G2  members, longer names, other namespaces, comments, string, character
      and raw string literals do not fire;
  G3  host-only rules are silent under --host-path and fire elsewhere;
  G4  a suppression on the line or alone on the line above allows the named
      rules only; one without a reason or naming no rule is a finding;
  G5  STXlib's own rules fire under --profile stxlib only;
  G6  the lock-free rule: an atomic type without an assert anywhere in the
      tree fires once, an assert in another file covers it, and pointers
      share one assert;
  G7  the exit status: 0 clean, 1 findings, 2 for a usage error.
"""

import pathlib
import re
import subprocess
import sys
import tempfile

FINDING = re.compile(r'^(?P<path>[^:]+):(?P<line>\d+): \[(?P<rule>[\w-]+)\]')

failures: list[str] = []
cases: int = 0


def run_check(tool: pathlib.Path, files: dict[str, str], options: list[str]) -> tuple[int, set[tuple[str, int, str]]]:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8')
        completed = subprocess.run([sys.executable, '-I', str(tool), '--root', str(root), *options, str(root)],
                                   capture_output=True, text=True, check=False)
        found: set[tuple[str, int, str]] = set()
        for line in completed.stdout.splitlines():
            match = FINDING.match(line)
            if match is not None:
                relative = pathlib.Path(match.group('path')).as_posix()
                found.add((relative, int(match.group('line')), match.group('rule')))
        return completed.returncode, found


def expect(tool: pathlib.Path, name: str, files: dict[str, str], options: list[str],
           expected: set[tuple[str, int, str]]) -> None:
    global cases
    cases += 1
    status, found = run_check(tool, files, options)
    wanted_status = 1 if expected else 0
    if found != expected or status != wanted_status:
        failures.append(f'{name}: status {status} (wanted {wanted_status}); '
                        f'missing {sorted(expected - found)}; unexpected {sorted(found - expected)}')


# G1: one positive line per rule, numbered from 1.
POSITIVE_LINES: list[tuple[str, str]] = [
    ('regex', '#include <regex>'),
    ('regex', 'std::regex pattern;'),
    ('rand', 'int value = rand();'),
    ('rand', 'std::srand(1);'),
    ('rand', 'int top = RAND_MAX;'),
    ('strtok', 'char* token = strtok(line, " ");'),
    ('strerror', 'const char* text = std::strerror(errno);'),
    ('broken-down-time', 'tm* parts = localtime(&now);'),
    ('broken-down-time', 'tm* parts = gmtime(&now);'),
    ('clock', 'clock_t start = clock();'),
    ('setjmp', 'jmp_buf where;'),
    ('setjmp', '#include <csetjmp>'),
    ('qsort', 'qsort(items, count, sizeof *items, compare);'),
    ('qsort', 'void* hit = bsearch(&key, items, count, 4, compare);'),
    ('ato', 'int number = atoi(text);'),
    ('ato', 'double real = std::atof(text);'),
    ('tmpnam', 'char* name = tmpnam(nullptr);'),
    ('system', 'int status = system("ls");'),
    ('system', 'FILE* pipe = popen(command, "r");'),
    ('locale-change', 'setlocale(LC_ALL, "");'),
    ('locale-change', 'std::locale::global(std::locale(""));'),
    ('locale-change', 'stream.imbue(locale);'),
    ('locale-text', 'int order = strcoll(a, b);'),
    ('locale-text', 'size_t n = mbstowcs(wide, narrow, 8);'),
    ('fenv', '#include <cfenv>'),
    ('fenv', 'fesetround(FE_UPWARD);'),
    ('execution-policy', 'std::sort(std::execution::par, first, last);'),
    ('special-functions', 'double j = std::cyl_bessel_j(0.5, x);'),
    ('special-functions', 'double b = std::betaf(1.0f, 2.0f);'),
    ('valarray', 'std::valarray<double> values;'),
    ('unbounded-copy', 'strcpy(target, source);'),
    ('unbounded-copy', 'std::strncpy(target, source, 4);'),
    ('c-threads', 'thrd_create(&thread, entry, nullptr);'),
    ('pthread', 'pthread_mutex_lock(&lock);'),
    ('wide-text', 'wchar_t letter = 0;'),
    ('wide-text', '#include <cwchar>'),
]

# G2: lines that name nothing governed.
NEGATIVE_TEXT = '''\
int operand(int);
int value = engine.rand();
int other = generator->rand();
int third = mine::rand();
int status = shell.system("x");
double clocked = timer.clock();
auto reading = my_clock();
// rand() strtok() localtime() in a comment
/* system("rm") and
   setlocale(LC_ALL, "") in a block comment */
const char* text = "rand() and qsort(x) in a string";
const char letter = '"'; int after = 1'000'000;
const char* raw = R"x(atoi("1") strcpy(a, b))x";
static_assert(true, "assert(x)");
int strtok_like = strtok_r(line, " ", &state) != nullptr;
std::sort(first, last);
'''


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: check_std_rules_tests.py <repository root>', file=sys.stderr)
        return 2
    tool = pathlib.Path(sys.argv[1]) / 'tools' / 'check_std_rules.py'

    positive = '\n'.join(line for _, line in POSITIVE_LINES) + '\n'
    expect(tool, 'G1 every rule fires', {'a.cpp': positive}, [],
           {('a.cpp', index + 1, rule) for index, (rule, _) in enumerate(POSITIVE_LINES)})
    expect(tool, 'G2 nothing governed', {'b.cpp': NEGATIVE_TEXT}, [], set())

    host = 'pthread_create(&thread, nullptr, entry, nullptr);\nwchar_t name[8];\n'
    expect(tool, 'G3 host code', {'hosted/posix.cpp': host, 'app/main.cpp': host}, ['--host-path', 'hosted/*'],
           {('app/main.cpp', 1, 'pthread'), ('app/main.cpp', 2, 'wide-text')})

    suppressed = ('int a = rand(); // std-rules: allow(rand) a test of the suppression\n'
                  '// std-rules: allow(rand, strtok) the next line, for a reason\n'
                  'int b = rand() + (strtok(s, " ") != nullptr);\n'
                  'int c = atoi(s); // std-rules: allow(rand) the wrong rule\n'
                  'int d = rand(); // std-rules: allow(rand)\n'
                  'int e = 0; // std-rules: allow(no-such-rule) a reason\n'
                  'int f = rand(); // std-rules: allowed rand\n')
    expect(tool, 'G4 suppressions', {'c.cpp': suppressed}, [],
           {('c.cpp', 4, 'ato'), ('c.cpp', 5, 'rand'), ('c.cpp', 5, 'suppression'), ('c.cpp', 6, 'suppression'),
            ('c.cpp', 7, 'rand'), ('c.cpp', 7, 'suppression')})

    own = '#include <iostream>\nstd::cout << 1;\nassert(x);\nstd::format("{}", 1);\nstd::recursive_mutex m;\n' \
          'lconv* c = localeconv();\n'
    expect(tool, 'G5 consumer profile', {'d.cpp': own}, [], set())
    expect(tool, 'G5 stxlib profile', {'d.cpp': own}, ['--profile', 'stxlib'],
           {('d.cpp', 1, 'streams'), ('d.cpp', 2, 'streams'), ('d.cpp', 3, 'assert'), ('d.cpp', 4, 'format'),
            ('d.cpp', 5, 'recursive-mutex'), ('d.cpp', 6, 'locale-use')})

    uses = 'std::atomic<u32> count;\nstd::atomic<std::uint32_t> other;\nstd::atomic<Node*> head;\n' \
           'std::atomic_ref<std::uint16_t> field(x);\nstd::atomic<bool> flag;\n'
    asserts = 'static_assert(std::atomic<std::uint32_t>::is_always_lock_free);\n' \
              'static_assert(std::atomic<void*>::is_always_lock_free);\n'
    expect(tool, 'G6 lock-free', {'e.cpp': uses, 'f.cpp': asserts}, ['--profile', 'stxlib'],
           {('e.cpp', 4, 'atomic-lock-free'), ('e.cpp', 5, 'atomic-lock-free')})
    expect(tool, 'G6 consumer profile has no lock-free rule', {'e.cpp': uses}, [], set())

    global cases
    cases += 1
    usage = subprocess.run([sys.executable, '-I', str(tool)], capture_output=True, text=True, check=False)
    if usage.returncode != 2:
        failures.append(f'G7 usage error: status {usage.returncode}')
    cases += 1
    listing = subprocess.run([sys.executable, '-I', str(tool), '--list-rules'], capture_output=True, text=True,
                             check=False)
    if listing.returncode != 0 or 'atomic-lock-free' not in listing.stdout:
        failures.append('G7 --list-rules')

    for failure in failures:
        print(failure)
    print(f'check_std_rules_tests: {cases} cases, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
