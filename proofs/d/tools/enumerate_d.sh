#!/bin/bash
# enumerate_d.sh SOURCES OUT [CLANG]: lifts, builds and runs phase D's
# exhaustive enumerations (row.json harnesses whose tool is "enumeration"),
# each as the original and as each of its mutants. The original must exit 0
# (every check holds) and each mutant must exit nonzero (the enumeration
# catches the planted change), as B1.md section 3.3 asks of every proof.
# Natively compiled with the pinned clang (default
# /opt/homebrew/opt/llvm@22/bin/clang++) under STXlib's float discipline
# (-ffp-contract=off, no fast math); writes only under OUT, outside the
# repository. Prints one line per run; exit status 0 only if every original
# passed and every mutant was caught.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
d="$(dirname "$here")"
b1tools="$(dirname "$d")/b1/tools"
sources="$1"; out="$2"; clang="${3:-/opt/homebrew/opt/llvm@22/bin/clang++}"
mkdir -p "$out"
stxlib="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["stxlib"]["root"])' "$sources")"
sdk=""
if command -v xcrun >/dev/null 2>&1; then sdk="-isysroot $(xcrun --show-sdk-path)"; fi
"$clang" --version | head -1
status=0
while IFS=$'\t' read -r row harness mutants; do
    name="$(basename "$harness" .cpp)"
    for variant in original $mutants; do
        tag="$row-$name-$variant"
        mutant_flag=()
        if [ "$variant" != original ]; then mutant_flag=(--mutant "$variant"); fi
        if ! python3 -B "$b1tools/lift.py" --sources "$sources" --harness "$harness" ${mutant_flag[@]+"${mutant_flag[@]}"} \
                --out "$out/$tag.cpp" --report "$out/$tag.lift.json"; then
            echo "$tag: lift-error"; status=1; continue
        fi
        # shellcheck disable=SC2086
        if ! "$clang" -std=c++20 -O2 -ffp-contract=off -fno-fast-math $sdk -I "$stxlib/include" -I "$stxlib/src" \
                "$out/$tag.cpp" -o "$out/$tag" 2> "$out/$tag.build.txt"; then
            echo "$tag: build-error (see $tag.build.txt)"; status=1; continue
        fi
        start=$(date +%s)
        "$out/$tag" > "$out/$tag.txt" 2>&1
        code=$?
        elapsed=$(( $(date +%s) - start ))
        if [ "$variant" = original ]; then
            if [ $code -eq 0 ]; then result=proved; else result="failed (exit $code)"; status=1; fi
        else
            if [ $code -ne 0 ]; then result="refuted (caught, exit $code)"; else result="not caught"; status=1; fi
        fi
        echo "$tag: $result (${elapsed} s) $(tail -1 "$out/$tag.txt")"
    done
done < <(python3 - "$d/rows" <<'PY'
import json, os, sys
rows = sys.argv[1]
for name in sorted(os.listdir(rows)):
    path = os.path.join(rows, name, 'row.json')
    if not os.path.isfile(path):
        continue
    row = json.load(open(path))
    for harness in row['harnesses']:
        if 'enumeration' in harness['tools']:
            print('\t'.join([row['row'], os.path.join(rows, name, harness['file']), ' '.join(harness.get('mutants', []))]))
PY
)
exit $status
