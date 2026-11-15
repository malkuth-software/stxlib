#!/bin/sh
# c1_sysroot.sh -- build a cross sysroot from the pinned VM image's own
# userland: boot a fresh VM "sysroot" on the stock kernel, stream
# /usr/include and /usr/lib/*.a, *.o out of it (sessions/sysroot-export.txt),
# and unpack them under <storage>/appliance/sysroot/<name>/. Nothing is
# downloaded: the files come from the image already verified by its
# checksum. Compiling against it stays with our toolchain (toolchain.env).
#
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS (as in
# c1_step2_kernel.sh), STXLIB_SYSROOT_NAME (default 15.2-BETA1-aarch64).
set -eu
cd "$(dirname "$0")"
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
name=${STXLIB_SYSROOT_NAME:-15.2-BETA1-aarch64}
root=$storage/appliance/sysroot
archive=$root/$name.tar.gz
[ ! -e "$root/$name" ] || { echo "c1_sysroot: $root/$name exists; remove it to rebuild" >&2; exit 2; }
mkdir -p "$root"
rm -f "$archive" "$archive.partial"
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name sysroot --reset
python3 guest_session.py --name sysroot --firmware-timeout-ms 0 --timeout 300 \
    --commands sessions/sysroot-export.txt --receive-file "$archive"
mkdir "$root/$name.partial"
tar -xzf "$archive" -C "$root/$name.partial"
mv "$root/$name.partial" "$root/$name"
shasum -a 256 "$archive" > "$archive.sha256"
echo "c1_sysroot: $root/$name ($(du -sh "$root/$name" | cut -f1)); archive sha256 $(cut -d' ' -f1 "$archive.sha256")"
ls "$root/$name/usr/lib" | grep -E '^(libc|libsys|libgcc|libgcc_eh|libcompiler_rt)\.a$|^crt' | tr '\n' ' '; echo
