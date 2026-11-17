#!/bin/sh
# Compile probes for the generated Windows API headers (no build, no link).
# Usage: probe.sh <clang bin dir> <log dir> [extra include dir...]
#   STXWIN_LIBCXX=<libc++ include/c++/v1> adds libc++ to the C++ probes and a
#   probe of every header together with libc++ headers.
#   Each header alone, in C++20 and C17, with and without UNICODE and
#   WIN32_LEAN_AND_MEAN; check/layout_check.cpp; lib/uuid.c; windows.h under
#   RC_INVOKED must preprocess to no declarations. Flags follow the no-MinGW
#   include contract: --target=x86_64-pc-windows-msvc -nostdinc, clang's
#   resource headers, then the extra include folders (the crt stream's), then
#   api/include. Prints one line per check and the failing commands; full
#   output goes to <log dir>.
set -u
BIN=${1:?clang bin dir}; LOGS=${2:?log dir}; shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
API="$HERE/../../src/hosted/windows/api"
RESOURCE=$("$BIN/clang" -print-resource-dir)
EXTRA=""
for dir in "$@"; do EXTRA="$EXTRA -isystem $dir"; done
COMMON="--target=x86_64-pc-windows-msvc -nostdinc -isystem $RESOURCE/include $EXTRA -isystem $API/include -fsyntax-only -Wall -Wextra -Werror -Wno-unused-function"
CXXINC=""
if [ -n "${STXWIN_LIBCXX:-}" ]; then CXXINC="-isystem $STXWIN_LIBCXX"; fi
mkdir -p "$LOGS"
failures=0
probe() { # name, command...
    name=$1; shift
    if "$@" > "$LOGS/$name.log" 2>&1; then :; else
        failures=$((failures + 1))
        if [ $failures -le 12 ]; then echo "FAIL $name: $(grep -m1 'error:' "$LOGS/$name.log")"; fi
    fi
}
count=0
for header in "$API"/include/*.h; do
    base=$(basename "$header" .h)
    for mode in cxx c; do
        for variant in plain unicode lean; do
            case $variant in plain) defs="";; unicode) defs="-DUNICODE -D_UNICODE";; lean) defs="-DUNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX";; esac
            if [ $mode = cxx ]; then
                probe "$base.$mode.$variant" "$BIN/clang++" $CXXINC $COMMON -std=c++20 $defs -x c++ "$header"
            else
                probe "$base.$mode.$variant" "$BIN/clang" $COMMON -std=c17 $defs -x c "$header"
            fi
            count=$((count + 1))
        done
    done
done
echo "headers: $count probes"
# Every header in one translation unit, before and after C and C++ library headers.
ALL="$LOGS/all_headers.cpp"
{
    echo '#include <stdio.h>'; echo '#include <wchar.h>'
    for header in "$API"/include/*.h; do echo "#include \"$(basename "$header")\""; done
    if [ -n "$CXXINC" ]; then
        echo '#ifdef __cplusplus'
        for name in atomic chrono cwchar filesystem mutex string thread; do echo "#include <$name>"; done
        echo '#endif'
    fi
} > "$ALL"
probe all_headers.cxx "$BIN/clang++" $CXXINC $COMMON -std=c++20 -DUNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -x c++ "$ALL"
probe all_headers.c "$BIN/clang" $COMMON -std=c17 -x c "$ALL"
probe layout_check "$BIN/clang++" $CXXINC $COMMON -std=c++20 -DUNICODE "$API/check/layout_check.cpp"
probe uuid "$BIN/clang" $COMMON -std=c17 "$API/lib/uuid.c"
# A resource script sees only macros.
if "$BIN/clang" --target=x86_64-pc-windows-msvc -nostdinc -isystem "$API/include" -E -P -DRC_INVOKED -x c "$API/include/windows.h" > "$LOGS/rc.i" 2> "$LOGS/rc.log"; then
    if grep -q '[^[:space:]]' "$LOGS/rc.i"; then failures=$((failures + 1)); echo "FAIL rc: declarations survive RC_INVOKED"; head -5 "$LOGS/rc.i"; fi
else
    failures=$((failures + 1)); echo "FAIL rc: preprocessing failed"; head -5 "$LOGS/rc.log"
fi
echo "probe: $failures failures"
[ $failures -eq 0 ]
