#!/bin/sh
# c1_step2_kernel.sh [KERNCONF] -- run B: time a built kernel in its own VM.
#
# Each kernel gets its own overlay, k-<kernconf>, made fresh from the pristine
# image, so a kernel that does not boot cannot stop the next one from being
# installed (the install sessions run on the stock kernel). Steps: prepare the
# VM; session 1 writes the loader configuration (sessions/loader-config.txt);
# session 2 installs the kernel and selects it; three timed boots that must
# show its ident, stopping after the first failure and saving the vCPU
# registers if a boot hangs; then the TSLOG dump, if the boots passed.
# Prints the kernel's recorded size first.
#
# Environment: STXLIB_STORAGE; STXLIB_IMAGE and STXLIB_CHECKSUMS (default: the
# 15.2-BETA1 image and checksum file under downloads/).
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_step2_kernel: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
vm=k-$label
kernels=$storage/appliance/kernels/$kernconf
[ -f "$kernels/kernel.sha256" ] || { echo "c1_step2_kernel: no built kernel in $kernels" >&2; exit 2; }
sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
[ -f "$kernels/kernel.size" ] && sed "s/^/kernel size ($kernconf): /" "$kernels/kernel.size"
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$sha" --define "kernconf=$kernconf"
python3 boot_timing.py --name "$vm" --firmware-timeout-ms 0 --runs 3 --label "$label" \
    --require "[ /]$kernconf arm64" --stop-after-failure --hang-dump
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/tslog-dump.txt
