#!/bin/sh
# run_host_tests.sh -- build and run the boot configuration's host tests on
# the M4 with the host's C++ compiler (${CXX:-c++}, version printed), under
# address and undefined-behaviour sanitizers with no recovery: a finding
# fails the run. Only the pure files (boot_config, init_executor,
# service_registry, user_config, access_keys, config_merge, ../boot_slots)
# take part;
# system_actions.cpp is FreeBSD's and builds in build_programs.sh.
# Output in <storage>/appliance/host_tests/. Exit status 0 only when every
# suite passes.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
config=$(cd "$here/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
out=$storage/appliance/host_tests
compiler=${CXX:-c++}
mkdir -p "$out"
"$compiler" --version | head -1
flags="-std=c++20 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
       -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_boot_config" "$here/test_boot_config.cpp" "$config/boot_config.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_init_executor" "$here/test_init_executor.cpp" "$config/boot_config.cpp" \
    "$config/init_executor.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_service_registry" "$here/test_service_registry.cpp" "$config/boot_config.cpp" \
    "$config/service_registry.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_boot_slots" "$here/test_boot_slots.cpp" "$config/../boot_slots.cpp"
# shellcheck disable=SC2086
"$compiler" $flags -o "$out/test_config_merge" "$here/test_config_merge.cpp" "$config/boot_config.cpp" \
    "$config/user_config.cpp" "$config/access_keys.cpp" "$config/config_merge.cpp"
status=0
"$out/test_boot_slots" || status=1
"$out/test_boot_config" || status=1
"$out/test_init_executor" || status=1
"$out/test_service_registry" || status=1
"$out/test_config_merge" || status=1
echo "run_host_tests: status=$status"
exit $status
