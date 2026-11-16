#!/bin/sh
# build_programs.sh -- cross-compile the appliance's own programs for
# FreeBSD on the M4, with the pinned toolchain (appliance/toolchain.env,
# versions checked) against a sysroot: arm64 (appliance/vm/c1_sysroot.sh)
# or amd64 (appliance/vm/c2_amd64_sysroot.sh), chosen by STXLIB_ARCH:
#
#   stxinit                 runtime/init/stxinit.cpp (PID 1)
#   stxsupervisor           runtime/supervisor/*.cpp, runtime/supervisor/config/*.cpp,
#                           runtime/package/program_slots.cpp, manifest.cpp, store_layout.cpp,
#                           the control channel (runtime/supervisor/control/*.cpp,
#                           control_channels.cpp) and runtime/store/*.cpp
#   stxpkg                  runtime/package/*.cpp and runtime/net/{url,http_head,fetch}.cpp,
#                           with the base system's libssl, libcrypto and libufs (static)
#   stxnet                  runtime/net/stxnet.cpp with the configuration parser,
#                           the user rules, the merge and the turn files
#   stxsntp                 runtime/net/stxsntp.cpp, sntp.cpp
#   stxsupervisor-crash     runtime/supervisor/test/crash_supervisor.cpp (test VMs only)
#   stxweb                  runtime/web/*.cpp with the supervisor's pure
#                           configuration, protocol and store files, linked
#                           with the sysroot's base OpenSSL (-lssl -lcrypto)
#
# Static (no run-time linker for PID 1), stripped (-s), unused sections
# dropped (--gc-sections); no exceptions, no RTTI. The C++ library is linked
# statically only for what the programs use (std::optional, std::variant
# headers; libc++.a supplies anything they reference). Output in
# <storage>/appliance/programs/: each program, <program>.sha256,
# <program>.size, and build.log. Stops at the first failure.
#
# Environment: STXLIB_STORAGE, STXLIB_ARCH (aarch64, the default, or
# amd64), STXLIB_SYSROOT (default <storage>/appliance/sysroot/15.2-BETA1-<arch>).
# Output in <storage>/appliance/programs/ (aarch64) or programs-amd64/.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
arch=${STXLIB_ARCH:-aarch64}
case "$arch" in
    aarch64) triple=aarch64-unknown-freebsd15.2; out=$storage/appliance/programs ;;
    amd64) triple=x86_64-unknown-freebsd15.2; out=$storage/appliance/programs-amd64 ;;
    *) echo "build_programs: STXLIB_ARCH must be aarch64 or amd64" >&2; exit 2 ;;
esac
sysroot=${STXLIB_SYSROOT:-$storage/appliance/sysroot/15.2-BETA1-$arch}
[ -f "$sysroot/usr/lib/libc.a" ] || { echo "build_programs: no sysroot at $sysroot (run c1_sysroot.sh)" >&2; exit 2; }
. "$repo/appliance/toolchain.env"
reported=$("$STXLIB_LLVM_BINDIR/clang" --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -1)
lld_reported=$("$STXLIB_LLD_BINDIR/ld.lld" --version | sed -n 's/.*LLD \([0-9.]*\).*/\1/p' | head -1)
[ "$reported" = "$STXLIB_LLVM_VERSION" ] && [ "$lld_reported" = "$STXLIB_LLVM_VERSION" ] || {
    echo "build_programs: clang $reported / ld.lld $lld_reported, toolchain.env pins $STXLIB_LLVM_VERSION" >&2; exit 2; }
mkdir -p "$out"
log=$out/build.log
: > "$log"

# build NAME SOURCE... [-- LIBRARY...] (sources relative to the repository;
# libraries as linker flags, after the sources)
build() {
    name=$1
    shift
    sources=
    libraries=
    in_libraries=0
    for source in "$@"; do
        if [ "$source" = "--" ]; then
            in_libraries=1
        elif [ "$in_libraries" -eq 1 ]; then
            libraries="$libraries $source"
        else
            sources="$sources $repo/$source"
        fi
    done
    status=0
    echo "=== $name:$sources$libraries" >> "$log"
    # shellcheck disable=SC2086
    "$STXLIB_LLVM_BINDIR/clang++" --target="$triple" --sysroot="$sysroot" \
        -fuse-ld=lld --ld-path="$STXLIB_LLD_BINDIR/ld.lld" \
        -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror \
        -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -static -s -Wl,--gc-sections \
        -o "$out/$name.new" $sources $libraries >>"$log" 2>&1 || status=$?
    if [ "$status" -ne 0 ]; then
        echo "build_programs: $name FAILED (exit $status)"
        grep -E 'error|warning|undefined' "$log" | head -40
        exit "$status"
    fi
    mv "$out/$name.new" "$out/$name"
    ( cd "$out" && shasum -a 256 "$name" > "$name.sha256" )
    {
        echo "file_bytes $(wc -c < "$out/$name" | tr -d ' ')"
        "$STXLIB_LLVM_BINDIR/llvm-size" "$out/$name" | tail -1
    } > "$out/$name.size"
    echo "build_programs: $name OK: $(tr '\n' ' ' < "$out/$name.size")"
}

build stxinit runtime/init/stxinit.cpp
build stxsupervisor runtime/supervisor/stxsupervisor.cpp runtime/supervisor/config/boot_config.cpp \
    runtime/supervisor/config/init_executor.cpp runtime/supervisor/config/service_registry.cpp \
    runtime/supervisor/config/system_actions.cpp runtime/supervisor/config/sysctl_value.cpp \
    runtime/supervisor/shim_client.cpp runtime/supervisor/boot_slots.cpp runtime/supervisor/boot_slots_system.cpp \
    runtime/supervisor/device_rules.cpp runtime/supervisor/device_rules_system.cpp \
    runtime/supervisor/targets.cpp runtime/supervisor/targets_plan.cpp \
    runtime/package/program_slots.cpp runtime/package/manifest.cpp runtime/package/store_layout.cpp \
    runtime/supervisor/config/user_config.cpp runtime/supervisor/config/access_keys.cpp \
    runtime/supervisor/config/config_merge.cpp runtime/supervisor/target_log.cpp runtime/supervisor/log_ring.cpp runtime/supervisor/console_echo.cpp \
    runtime/supervisor/program_answers.cpp runtime/supervisor/config/target_ids.cpp \
    runtime/supervisor/target_id_files.cpp \
    runtime/supervisor/control/control_protocol.cpp runtime/supervisor/control/control_server.cpp \
    runtime/supervisor/control_channels.cpp runtime/store/turn_file.cpp runtime/store/turn_file_system.cpp
build stxpkg runtime/package/stxpkg.cpp runtime/package/manifest.cpp runtime/package/container.cpp \
    runtime/package/latest.cpp runtime/package/program_slots.cpp runtime/package/store_layout.cpp \
    runtime/net/url.cpp runtime/net/http_head.cpp runtime/net/fetch.cpp \
    runtime/supervisor/config/boot_config.cpp runtime/supervisor/config/service_registry.cpp \
    -- -lufs -lssl -lcrypto -lpthread
build stxnet runtime/net/stxnet.cpp runtime/supervisor/config/boot_config.cpp \
    runtime/supervisor/config/user_config.cpp runtime/supervisor/config/config_merge.cpp \
    runtime/store/turn_file.cpp runtime/store/turn_file_system.cpp
build stxsntp runtime/net/stxsntp.cpp runtime/net/sntp.cpp
build stxsupervisor-crash runtime/supervisor/test/crash_supervisor.cpp
build stxweb runtime/web/stxweb.cpp runtime/web/tls_server.cpp runtime/web/system_backend.cpp \
    runtime/web/crypto_system.cpp runtime/web/control_client.cpp runtime/web/api.cpp runtime/web/api_pages.cpp \
    runtime/web/auth.cpp runtime/web/config_edit.cpp runtime/web/http.cpp runtime/web/json.cpp \
    runtime/supervisor/config/boot_config.cpp runtime/supervisor/config/user_config.cpp \
    runtime/supervisor/control/control_protocol.cpp runtime/store/turn_file.cpp runtime/store/turn_file_system.cpp \
    -- -lssl -lcrypto -lpthread
