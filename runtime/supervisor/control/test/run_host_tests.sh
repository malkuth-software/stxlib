#!/bin/sh
# run_host_tests.sh -- build and run the control channel's and the turn
# files' host tests on the M4 with the host's C++ compiler (${CXX:-c++},
# version printed), under address and undefined-behaviour sanitizers with
# no recovery: a finding fails the run. Pure files only (and the turn
# files' system half, which is POSIX and runs in a scratch directory under
# the output). Output in <storage>/appliance/host_tests/. Exit status 0 only
# when every suite passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
supervisor=$(cd "$here/../.." && pwd)
store=$(cd "$supervisor/../store" && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_turn_file" "$store/test/test_turn_file.cpp" "$store/turn_file.cpp" \
    "$store/turn_file_system.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_control_protocol" "$here/test_control_protocol.cpp" \
    "$supervisor/control/control_protocol.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_control_server" "$here/test_control_server.cpp" \
    "$supervisor/control/control_protocol.cpp" "$supervisor/control/control_server.cpp" \
    "$supervisor/config/boot_config.cpp" "$supervisor/config/service_registry.cpp" \
    "$supervisor/config/user_config.cpp" "$supervisor/config/target_ids.cpp" "$supervisor/boot_slots.cpp"
scratch=$out/turn_scratch
rm -rf "$scratch"
mkdir -p "$scratch"
status=0
"$out/test_turn_file" "$scratch" || status=1
"$out/test_control_protocol" || status=1
"$out/test_control_server" || status=1
echo "run_host_tests: status=$status"
exit $status
