#!/bin/sh
# run_host_tests.sh -- build and run the shim's host test on the M4 with the
# host's C compiler (${CC:-cc}, version printed), under address and
# undefined-behaviour sanitizers with no recovery. Only the pure part takes
# part (stxshim_terminate.h, the code the kernel runs); stxshim.c is the
# kernel's and builds with the kernel (appliance/kernel/build_kernel.sh).
# Output in <storage>/appliance/host_tests/. Exit status 0 only on a pass.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests
compiler=${CC:-cc}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_stxshim_terminate" "$here/test_stxshim_terminate.c"
status=0
"$out/test_stxshim_terminate" || status=1
echo "run_host_tests (shim): status=$status"
exit $status
