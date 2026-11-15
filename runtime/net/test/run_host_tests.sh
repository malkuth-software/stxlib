#!/bin/sh
# run_host_tests.sh -- build and run runtime/net's host tests on the M4 with
# the host's C++ compiler (${CXX:-c++}, version printed), under address and
# undefined-behaviour sanitizers with no recovery: a finding fails the run.
# The pure files only (url, http_head, sntp). test_sntp's two 2^32 loops
# take the longest (seconds to a minute under the sanitizers).
# Output in <storage>/appliance/host_tests/net/. Exit status 0 only when
# every suite passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
net=$(cd "$here/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests/net
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_url" "$here/test_url.cpp" "$net/url.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_http_head" "$here/test_http_head.cpp" "$net/http_head.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_sntp" "$here/test_sntp.cpp" "$net/sntp.cpp"
status=0
"$out/test_url" || status=1
"$out/test_http_head" || status=1
"$out/test_sntp" || status=1
echo "run_host_tests (net): status=$status"
exit $status
