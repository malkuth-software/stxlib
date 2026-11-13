#!/bin/sh
# build_programs.sh -- cross-compile the appliance's own programs for arm64
# FreeBSD on the M4, with the pinned toolchain (appliance/toolchain.env,
# versions checked) against the sysroot made by appliance/vm/c1_sysroot.sh:
#
#   stxinit                 runtime/init/stxinit.cpp (PID 1)
#   stxsupervisor           runtime/supervisor/stxsupervisor.cpp and
#                           runtime/supervisor/config/*.cpp
#   stxsupervisor-crash     runtime/supervisor/test/crash_supervisor.cpp (test VMs only)
#
# Static (no run-time linker for PID 1), stripped (-s), unused sections
# dropped (--gc-sections); no exceptions, no RTTI. The C++ library is linked
# statically only for what the programs use (std::optional, std::variant
# headers; libc++.a supplies anything they reference). Output in
# <storage>/appliance/programs/: each program, <program>.sha256,
# <program>.size, and build.log. Stops at the first failure.
#
# Environment: STXLIB_STORAGE, STXLIB_SYSROOT (default
# <storage>/appliance/sysroot/15.2-BETA1-aarch64).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
sysroot=${STXLIB_SYSROOT:-$storage/appliance/sysroot/15.2-BETA1-aarch64}
out=$storage/appliance/programs
[ -f "$sysroot/usr/lib/libc.a" ] || { echo "build_programs: no sysroot at $sysroot (run c1_sysroot.sh)" >&2; exit 2; }
. "$repo/appliance/toolchain.env"
reported=$("$STXLIB_LLVM_BINDIR/clang" --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -1)
lld_reported=$("$STXLIB_LLD_BINDIR/ld.lld" --version | sed -n 's/.*LLD \([0-9.]*\).*/\1/p' | head -1)
[ "$reported" = "$STXLIB_LLVM_VERSION" ] && [ "$lld_reported" = "$STXLIB_LLVM_VERSION" ] || {
    echo "build_programs: clang $reported / ld.lld $lld_reported, toolchain.env pins $STXLIB_LLVM_VERSION" >&2; exit 2; }
mkdir -p "$out"
log=$out/build.log
: > "$log"

# build NAME SOURCE... (paths relative to the repository)
build() {
    name=$1
    shift
    sources=
    for source in "$@"; do
        sources="$sources $repo/$source"
    done
    status=0
    echo "=== $name:$sources" >> "$log"
    "$STXLIB_LLVM_BINDIR/clang++" --target=aarch64-unknown-freebsd15.2 --sysroot="$sysroot" \
        -fuse-ld=lld --ld-path="$STXLIB_LLD_BINDIR/ld.lld" \
        -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror \
        -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -static -s -Wl,--gc-sections \
        -o "$out/$name.new" $sources >>"$log" 2>&1 || status=$?
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
    runtime/supervisor/config/system_actions.cpp
build stxsupervisor-crash runtime/supervisor/test/crash_supervisor.cpp
