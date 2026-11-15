#!/bin/bash
# enumerate.sh SOURCES OUT [CLANG]: lifts and runs row 21's exhaustive
# enumeration (rows/21-sincos-float/enumerate.cpp), the numeric obligations
# of its bound-effect configuration. Natively compiled with the pinned clang
# (default /opt/homebrew/opt/llvm@22/bin/clang++) and the macOS SDK; writes
# only under OUT (outside the repository). Exit status: the program's.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
b1="$(dirname "$here")"
sources="$1"; out="$2"; clang="${3:-/opt/homebrew/opt/llvm@22/bin/clang++}"
mkdir -p "$out"
auxlib="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["stx_auxlib"]["root"])' "$sources")"
python3 -B "$here/lift.py" --sources "$sources" --harness "$b1/rows/21-sincos-float/enumerate.cpp" \
    --out "$out/enumerate21.cpp" --report "$out/enumerate21.lift.json"
sdk="$(xcrun --show-sdk-path)"
"$clang" -std=c++20 -O2 -ffp-contract=off -fno-fast-math -fno-math-errno -isysroot "$sdk" \
    -DSTX_AUXLIB_MATH_VARIANT=native -DSTX_AUXLIB_MATH_DEKKER=1 \
    -I "$auxlib/include" -I "$auxlib/src/math" -I "$b1/common" \
    "$out/enumerate21.cpp" -o "$out/enumerate21"
"$clang" --version | head -1
/usr/bin/time -l "$out/enumerate21"
