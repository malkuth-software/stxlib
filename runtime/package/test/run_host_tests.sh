#!/bin/sh
# run_host_tests.sh -- build and run runtime/package's host tests on the M4
# with the host's C++ compiler (${CXX:-c++}, version printed), under address
# and undefined-behaviour sanitizers with no recovery: a finding fails the
# run. The pure files only (manifest, container, latest, program_slots,
# with ../net/url.cpp and the configuration parser for package paths).
# Output in <storage>/appliance/host_tests/package/. Exit status 0 only
# when every suite passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
package=$(cd "$here/.." && pwd)
runtime=$(cd "$package/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests/package
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_manifest" "$here/test_manifest.cpp" "$package/manifest.cpp" \
    "$package/container.cpp" "$runtime/supervisor/config/boot_config.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_latest" "$here/test_latest.cpp" "$package/latest.cpp" "$runtime/net/url.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_program_slots" "$here/test_program_slots.cpp" "$package/program_slots.cpp"
status=0
"$out/test_manifest" || status=1
"$out/test_latest" || status=1
"$out/test_program_slots" || status=1
echo "run_host_tests (package): status=$status"
exit $status
