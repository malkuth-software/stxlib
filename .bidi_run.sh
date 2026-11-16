#!/bin/zsh
# sb1 bidi run: release build (tests + bench vs SheenBidi), then ASan+UBSan+integer build of the bidi tests.
set -u
S=$PWD
B=/Volumes/Storage/stxlib_workers/sb1-bidi
D=/Volumes/Storage/stxlib_workers/unicode-18.0.0
SB=$HOME/Developer/Projects/stxlib_workers/stx_map/stxlib/third_party/sheenbidi
python3 -I tools/gen_bidi_tables.py --check third_party/unicode-18.0.0 src/algorithms/bidi/bidi_tables.inc && echo "PASS tables"
cmake -S . -B $B/rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTXLIB_UNICODE_TEST_DATA=$D -DSTXLIB_SHEENBIDI_DIR=$SB >/dev/null || { echo "FAIL configure rel"; exit 1; }
ninja -C $B/rel stxlib_bidi_tests stxlib_bidi_bench 2>&1 | grep -E 'error|warning' | head -40
$B/rel/bin/stxlib_bidi_tests third_party/unicode-18.0.0 $D --exhaustive 5 2>&1 | tail -45
$B/rel/bin/stxlib_bidi_bench $D/BidiCharacterTest.txt 2>&1 | tail -30
SAN="-fsanitize=address,undefined,integer -fno-sanitize-recover=all -fno-omit-frame-pointer"
cmake -S . -B $B/san -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSTXLIB_UNICODE_TEST_DATA=$D "-DCMAKE_CXX_FLAGS=$SAN" "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined,integer" >/dev/null || { echo "FAIL configure san"; exit 1; }
ninja -C $B/san stxlib_bidi_tests 2>&1 | grep -E 'error' | head -20
UBSAN_OPTIONS=print_stacktrace=1 $B/san/bin/stxlib_bidi_tests third_party/unicode-18.0.0 $D --random 20000 2>&1 | tail -25
echo "-> exit done"
