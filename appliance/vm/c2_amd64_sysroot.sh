#!/bin/sh
# c2_amd64_sysroot.sh -- the amd64 cross sysroot for runtime/build_programs.sh
# (STXLIB_ARCH=amd64) and the amd64 base the image builder copies from
# (appliance/disk/build_image.sh, board amd64). Takes the release's amd64
# base.txz, checks it against the release's MANIFEST (the distribution
# checksums bsdinstall checks: "base.txz <sha256> <files> base ..."), and
# unpacks /usr/include and /usr/lib's static libraries and start files
# under <storage>/appliance/sysroot/15.2-BETA1-amd64/, as c1_sysroot.sh
# does for arm64 from the VM image. Nothing is downloaded here: the two
# files are fetched once, with the owner's approval, by the integrator
# (FreeBSD 15.2-BETA1 amd64: .../releases/amd64/amd64/15.2-BETA1/base.txz
# and MANIFEST beside it).
#
# Environment: STXLIB_STORAGE, STXLIB_AMD64_BASE (default
# <storage>/downloads/FreeBSD-15.2-BETA1-amd64-base.txz),
# STXLIB_AMD64_MANIFEST (default <storage>/downloads/FreeBSD-15.2-BETA1-amd64-MANIFEST).
set -eu
cd "$(dirname "$0")"
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
base=${STXLIB_AMD64_BASE:-$storage/downloads/FreeBSD-15.2-BETA1-amd64-base.txz}
manifest=${STXLIB_AMD64_MANIFEST:-$storage/downloads/FreeBSD-15.2-BETA1-amd64-MANIFEST}
name=15.2-BETA1-amd64
root=$storage/appliance/sysroot
for file in "$base" "$manifest"; do
    [ -f "$file" ] || { echo "c2_amd64_sysroot: missing $file" >&2; exit 2; }
done
want=$(awk '$1 == "base.txz" {print $2}' "$manifest")
[ -n "$want" ] || { echo "c2_amd64_sysroot: MANIFEST names no base.txz" >&2; exit 1; }
have=$(shasum -a 256 "$base" | cut -d' ' -f1)
[ "$have" = "$want" ] || { echo "c2_amd64_sysroot: base.txz sha256 $have, MANIFEST says $want" >&2; exit 1; }
[ ! -e "$root/$name" ] || { echo "c2_amd64_sysroot: $root/$name exists; remove it to rebuild" >&2; exit 2; }
mkdir -p "$root/$name.partial"
# bsdtar (macOS's tar) reads xz. Only the headers and the static
# libraries and start files: what a static cross build links against.
tar -xf "$base" -C "$root/$name.partial" --include './usr/include/*' --include './usr/lib/*.a' \
    --include './usr/lib/*.o'
[ -f "$root/$name.partial/usr/lib/libc.a" ] || { echo "c2_amd64_sysroot: no usr/lib/libc.a in base.txz" >&2; exit 1; }
mv "$root/$name.partial" "$root/$name"
echo "c2_amd64_sysroot: $root/$name ($(du -sh "$root/$name" | cut -f1)) from base.txz sha256 $have"
ls "$root/$name/usr/lib" | grep -E '^(libc|libsys|libssl|libcrypto|libufs|libcompiler_rt)\.a$|^crt' | tr '\n' ' '; echo
