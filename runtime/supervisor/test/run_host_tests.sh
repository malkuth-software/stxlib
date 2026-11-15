#!/bin/sh
# run_host_tests.sh -- build and run the supervisor's own pure host tests
# (targets_plan, device_rules) on the M4 with the host's C++ compiler
# (${CXX:-c++}), under address and undefined-behaviour sanitizers with no
# recovery. Output in <storage>/appliance/host_tests/supervisor/. Exit
# status 0 only when every suite passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
supervisor=$(cd "$here/.." && pwd)
runtime=$(cd "$supervisor/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests/supervisor
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_targets_plan" "$here/test_targets_plan.cpp" "$supervisor/targets_plan.cpp" \
    "$supervisor/device_rules.cpp" "$supervisor/config/boot_config.cpp" "$supervisor/config/service_registry.cpp" \
    "$runtime/package/program_slots.cpp"
status=0
"$out/test_targets_plan" || status=1
echo "run_host_tests (supervisor): status=$status"
exit $status
