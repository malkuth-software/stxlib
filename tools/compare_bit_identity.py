#!/usr/bin/env python3
"""Compares stxlib_bit_identity outputs with the frozen hashes.

The cross-host bit-identity check (planning/stages/A1-float-discipline.md,
section 3). Each output line is `<item> <hash> <values hashed>`; lines
starting with '#' say what ran and are echoed. tests/bit_identity_expected.txt
holds one line per item: the frozen `<item> <hash> <values hashed>`, or
`<item> unfrozen` before the first freeze.

Usage:
  compare_bit_identity.py --expected FILE --run [EMULATOR...] PROGRAM
      runs PROGRAM (with this process's environment, so STX_SIMD_LEVEL
      reaches it) and compares its output; the ctest form.
  compare_bit_identity.py --expected FILE OUTPUT...
      compares saved outputs, which must also agree with each other.
  compare_bit_identity.py --freeze OUTPUT...
      prints an expected file built from the outputs, refusing unless they
      all agree item for item.

Fails (exit 1) when the program fails, an output line is malformed, two
outputs disagree, an output differs from a frozen line, or a frozen item is
missing. An unfrozen item, or one the expected file does not list, is
reported (as a GitHub Actions warning under CI) and does not fail.
"""

import os
import subprocess
import sys

HEADER = """# Frozen output hashes of stxlib_bit_identity (tests/bit_identity.cpp), one
# per item: `<item> <hash> <values hashed>`, or `<item> unfrozen` until the
# first run. Every host, SIMD level and product form must print the frozen
# line (planning/stages/A1-float-discipline.md, section 3). A frozen line
# changes only with a deliberate change of results, named in PROVENANCE.md.
#
# Freezing: the integrator runs the program on the M4 (all levels it has,
# and the Dekker build) and then
#   python3 -I tools/compare_bit_identity.py --freeze OUTPUT... > tests/bit_identity_expected.txt
# which refuses unless every output agrees; CI's hosts then check the same
# values.
"""


def fail(message):
    print("bit identity: FAILED: " + message)
    return 1


def warn(message):
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print("::warning::bit identity: " + message)
    else:
        print("bit identity: warning: " + message)


def parse_output(name, text):
    """Returns ({item: (hash, count)}, [item order], [comments]) or raises."""
    items = {}
    order = []
    comments = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            comments.append(line)
            continue
        fields = line.split()
        if len(fields) != 3 or len(fields[1]) != 16 or not fields[2].isdigit():
            raise ValueError("%s line %d is malformed: %r" % (name, number, line))
        int(fields[1], 16)
        if fields[0] in items:
            raise ValueError("%s line %d repeats item %s" % (name, number, fields[0]))
        items[fields[0]] = (fields[1], fields[2])
        order.append(fields[0])
    return items, order, comments


def parse_expected(path):
    """Returns {item: (hash, count) or None for unfrozen}."""
    expected = {}
    with open(path, encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) == 2 and fields[1] == "unfrozen":
                expected[fields[0]] = None
            elif len(fields) == 3 and len(fields[1]) == 16 and fields[2].isdigit():
                expected[fields[0]] = (fields[1], fields[2])
            else:
                raise ValueError("%s line %d is malformed: %r" % (path, number, line))
    return expected


def agree(outputs):
    """Checks that every output has the same items and values. Returns an
    error message or None."""
    first_name, (first_items, _, _) = outputs[0]
    for name, (items, _, _) in outputs[1:]:
        for item in sorted(set(first_items) | set(items)):
            left = first_items.get(item)
            right = items.get(item)
            if left != right:
                return "%s: %s gives %s, %s gives %s" % (item, first_name, left, name, right)
    return None


def compare(expected, outputs):
    status = 0
    message = agree(outputs)
    if message is not None:
        status = fail("the outputs disagree: " + message)
    _, (items, order, _) = outputs[0]
    matched = 0
    unfrozen = []
    for item in order:
        if item not in expected:
            unfrozen.append(item)
            continue
        frozen = expected[item]
        if frozen is None:
            unfrozen.append(item)
            continue
        if items[item] != frozen:
            status = fail("%s: frozen %s %s, this host %s %s" % (item, frozen[0], frozen[1], items[item][0],
                                                                 items[item][1]))
        else:
            matched += 1
    for item, frozen in sorted(expected.items()):
        if frozen is not None and item not in items:
            status = fail("%s is frozen but was not printed" % item)
    if unfrozen:
        warn("%d items not frozen yet: %s" % (len(unfrozen), " ".join(unfrozen)))
        for item in unfrozen:
            print("  %s %s %s" % (item, items[item][0], items[item][1]))
    print("bit identity: %d frozen items match, %d unfrozen, %d outputs compared" % (matched, len(unfrozen),
                                                                                   len(outputs)))
    return status


def main(arguments):
    if len(arguments) >= 2 and arguments[0] == "--freeze":
        outputs = []
        for path in arguments[1:]:
            with open(path, encoding="utf-8") as handle:
                outputs.append((path, parse_output(path, handle.read())))
        message = agree(outputs)
        if message is not None:
            print("bit identity: refusing to freeze: " + message, file=sys.stderr)
            return 1
        _, (items, order, _) = outputs[0]
        sys.stdout.write(HEADER)
        for item in order:
            sys.stdout.write("%s %s %s\n" % (item, items[item][0], items[item][1]))
        return 0
    if len(arguments) >= 3 and arguments[0] == "--expected":
        expected = parse_expected(arguments[1])
        outputs = []
        if arguments[2] == "--run" and len(arguments) >= 4:
            # The program is the last argument; any before it are the command
            # that runs it (Emscripten: Node).
            program = arguments[-1]
            completed = subprocess.run(arguments[3:], stdout=subprocess.PIPE, universal_newlines=True)
            for line in completed.stdout.splitlines():
                if line.startswith("#"):
                    print(line)
            if completed.returncode != 0:
                return fail("%s exited with %d" % (program, completed.returncode))
            outputs.append((program, parse_output(program, completed.stdout)))
        else:
            for path in arguments[2:]:
                with open(path, encoding="utf-8") as handle:
                    outputs.append((path, parse_output(path, handle.read())))
        return compare(expected, outputs)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except (OSError, ValueError) as error:
        sys.exit(fail(str(error)))
