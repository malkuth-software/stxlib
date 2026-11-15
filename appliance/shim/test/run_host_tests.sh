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
status=0
for name in terminate register; do
	# shellcheck disable=SC2086
	if "$compiler" $flags -o "$out/test_stxshim_$name" "$here/test_stxshim_$name.c"; then
		"$out/test_stxshim_$name" || status=1
	else
		echo "test_stxshim_$name: build failed"
		status=1
	fi
done
echo "run_host_tests (shim): status=$status"
exit $status
