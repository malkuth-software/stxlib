#!/bin/sh
# build_stxinit.sh -- cross-compile PID 1 (stxinit.cpp) for arm64 FreeBSD on
# the M4, with the pinned toolchain (appliance/toolchain.env, versions
# checked) against the sysroot made by appliance/vm/c1_sysroot.sh. Static, so
# PID 1 needs no run-time linker. Output in <storage>/appliance/init/:
# stxinit, stxinit.sha256, stxinit.size, and the build log.
#
# Environment: STXLIB_STORAGE, STXLIB_SYSROOT (default
# <storage>/appliance/sysroot/15.2-BETA1-aarch64).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
sysroot=${STXLIB_SYSROOT:-$storage/appliance/sysroot/15.2-BETA1-aarch64}
out=$storage/appliance/init
[ -f "$sysroot/usr/lib/libc.a" ] || { echo "build_stxinit: no sysroot at $sysroot (run c1_sysroot.sh)" >&2; exit 2; }
. "$repo/appliance/toolchain.env"
reported=$("$STXLIB_LLVM_BINDIR/clang" --version | sed -n 's/.*clang version \([0-9.]*\).*/\1/p' | head -1)
lld_reported=$("$STXLIB_LLD_BINDIR/ld.lld" --version | sed -n 's/.*LLD \([0-9.]*\).*/\1/p' | head -1)
[ "$reported" = "$STXLIB_LLVM_VERSION" ] && [ "$lld_reported" = "$STXLIB_LLVM_VERSION" ] || {
    echo "build_stxinit: clang $reported / ld.lld $lld_reported, toolchain.env pins $STXLIB_LLVM_VERSION" >&2; exit 2; }
mkdir -p "$out"
log=$out/build.log
status=0
"$STXLIB_LLVM_BINDIR/clang++" --target=aarch64-unknown-freebsd15.2 --sysroot="$sysroot" \
    -fuse-ld=lld --ld-path="$STXLIB_LLD_BINDIR/ld.lld" \
    -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror \
    -fno-exceptions -fno-rtti -nostdlib++ -static \
    -o "$out/stxinit.new" "$here/stxinit.cpp" >"$log" 2>&1 || status=$?
if [ "$status" -ne 0 ]; then
    echo "build_stxinit: FAILED (exit $status)"
    head -40 "$log"
    exit "$status"
fi
mv "$out/stxinit.new" "$out/stxinit"
( cd "$out" && shasum -a 256 stxinit > stxinit.sha256 )
{
    echo "file_bytes $(wc -c < "$out/stxinit" | tr -d ' ')"
    "$STXLIB_LLVM_BINDIR/llvm-size" "$out/stxinit"
    "$STXLIB_LLVM_BINDIR/llvm-readelf" -h "$out/stxinit" | grep -E 'Class|Machine|OS/ABI|Type'
} > "$out/stxinit.size"
echo "build_stxinit: OK: $out/stxinit"
sed 's/^/build_stxinit: /' "$out/stxinit.size"
