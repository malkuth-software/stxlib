#!/usr/bin/env python3
"""Runs the MSVC C++ runtime conformance suite (tests/msvc_runtime) and
compares STXlib's runtime with Microsoft's vcruntime, case by case.

Each case was built twice from one source: in the oracle tree against
vcruntime and in the ours tree against STXlib's runtime, static
(case_<name>.exe) and shared (case_<name>_shared.exe). A case matches when
its stdout and exit code equal the oracle's. The import scan then checks
that no program or DLL of the ours tree imports vcruntime140*.dll or
msvcp140*.dll.

Prints one line per comparison and per scanned image, then a summary line;
each program's output is kept under --log-dir. Exits 0 only when every case
matches and the scan is clean.

    run_conformance.py --oracle-bin D:/.../conformance-oracle/bin \\
        --ours-bin D:/.../conformance-ours/bin --log-dir D:/.../logs/conformance \\
        --readobj D:/.../llvm-readobj.exe
"""

import argparse
import pathlib
import subprocess
import sys

CASES = [
    "basic",
    "hierarchy",
    "nested",
    "rethrow",
    "unwind",
    "destructor_throw",
    "noexcept_violation",
    "exception_pointer",
    "structured",
    "rtti",
    "threads",
    "copy_throw",
    "unhandled",
    "standard_exceptions",
    "objects",
    "states",
    "structured_and_jump",
]

# Cases built only against the shared runtime on the ours side.
SHARED_ONLY_CASES = ["cross_dll"]

FORBIDDEN_IMPORT_PREFIXES = ("vcruntime140", "msvcp140")

TIMEOUT_SECONDS = 120


def run_program(program: pathlib.Path, log_dir: pathlib.Path, label: str) -> tuple[str, int]:
    try:
        completed = subprocess.run(
            [str(program)],
            cwd=str(program.parent),
            stdin=subprocess.DEVNULL,
            capture_output=True,
            timeout=TIMEOUT_SECONDS,
        )
        output = completed.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        errors = completed.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
        code = completed.returncode
    except subprocess.TimeoutExpired:
        output = ""
        errors = "timed out\n"
        code = -999
    (log_dir / f"{label}.out").write_text(output, encoding="utf-8")
    (log_dir / f"{label}.err").write_text(errors + f"exit code {code}\n", encoding="utf-8")
    return output, code


def first_difference(expected: str, actual: str) -> str:
    expected_lines = expected.split("\n")
    actual_lines = actual.split("\n")
    count = max(len(expected_lines), len(actual_lines))
    for index in range(count):
        left = expected_lines[index] if index < len(expected_lines) else "<end>"
        right = actual_lines[index] if index < len(actual_lines) else "<end>"
        if left != right:
            return f"line {index + 1}: oracle {left!r} ours {right!r}"
    return "same output"


def compare(case: str, oracle: tuple[str, int], ours_bin: pathlib.Path, suffix: str, log_dir: pathlib.Path) -> bool:
    program = ours_bin / f"case_{case}{suffix}.exe"
    variant = "shared" if suffix else "static"
    if not program.exists():
        print(f"MISSING {case} {variant}: {program.name}")
        return False
    output, code = run_program(program, log_dir, f"ours_{case}_{variant}")
    if output == oracle[0] and code == oracle[1]:
        print(f"match   {case} {variant} (exit {code})")
        return True
    detail = first_difference(oracle[0], output)
    print(f"DIFFER  {case} {variant}: exit oracle {oracle[1]} ours {code}; {detail}")
    return False


def imports_of(readobj: pathlib.Path, image: pathlib.Path) -> list[str]:
    completed = subprocess.run([str(readobj), "--coff-imports", str(image)], capture_output=True, text=True)
    names = []
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("Name:") and stripped.lower().endswith(".dll"):
            names.append(stripped.split(":", 1)[1].strip())
    return sorted(set(names))


def scan(readobj: pathlib.Path, ours_bin: pathlib.Path, oracle_bin: pathlib.Path) -> bool:
    clean = True
    images = sorted(ours_bin.glob("*.exe")) + sorted(ours_bin.glob("*.dll"))
    for image in images:
        names = imports_of(readobj, image)
        forbidden = [name for name in names if name.lower().startswith(FORBIDDEN_IMPORT_PREFIXES)]
        if forbidden:
            clean = False
            print(f"IMPORTS {image.name}: {', '.join(forbidden)}")
    print(f"scanned {len(images)} ours images: {'no vcruntime140/msvcp140 import' if clean else 'forbidden imports found'}")
    sample = oracle_bin / "case_exception_pointer.exe"
    if sample.exists():
        print(f"oracle  {sample.name} imports {', '.join(imports_of(readobj, sample))}")
    return clean


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--oracle-bin", required=True, type=pathlib.Path)
    parser.add_argument("--ours-bin", required=True, type=pathlib.Path)
    parser.add_argument("--log-dir", required=True, type=pathlib.Path)
    parser.add_argument("--readobj", required=True, type=pathlib.Path)
    parser.add_argument("--case", action="append", help="run only these cases")
    arguments = parser.parse_args()
    arguments.log_dir.mkdir(parents=True, exist_ok=True)
    selected = arguments.case
    total = 0
    matched = 0
    for case in CASES + SHARED_ONLY_CASES:
        if selected and case not in selected:
            continue
        oracle_program = arguments.oracle_bin / f"case_{case}.exe"
        if not oracle_program.exists():
            print(f"MISSING {case} oracle: {oracle_program.name}")
            total = total + 1
            continue
        oracle = run_program(oracle_program, arguments.log_dir, f"oracle_{case}")
        suffixes = ["_shared"] if case in SHARED_ONLY_CASES else ["", "_shared"]
        for suffix in suffixes:
            total = total + 1
            if compare(case, oracle, arguments.ours_bin, suffix, arguments.log_dir):
                matched = matched + 1
    clean = scan(arguments.readobj, arguments.ours_bin, arguments.oracle_bin)
    print(f"summary: {matched}/{total} comparisons match; import scan {'clean' if clean else 'FAILED'}")
    return 0 if matched == total and clean else 1


if __name__ == "__main__":
    sys.exit(main())
