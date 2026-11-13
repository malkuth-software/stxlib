#!/bin/sh
# c1_pid1.sh [KERNCONF] -- time the appliance boot: a built kernel and our
# PID 1 (runtime/init, built by build_stxinit.sh) in a fresh VM
# k-<kernconf>-pid1. Sessions on the stock kernel write the loader
# configuration, install the kernel, then install stxinit as init_path (no
# rc, no getty; after that the VM has no login). Up to three timed boots end
# at stxinit's marker, stopping at the first failure and saving the vCPU
# registers if a boot hangs. Prints the kernel's and stxinit's sizes first.
#
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS (as in
# c1_step2_kernel.sh).
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_pid1: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
vm=k-$label-pid1
kernels=$storage/appliance/kernels/$kernconf
init=$storage/appliance/init
[ -f "$kernels/kernel.sha256" ] || { echo "c1_pid1: no built kernel in $kernels" >&2; exit 2; }
[ -f "$init/stxinit.sha256" ] || { echo "c1_pid1: no built stxinit in $init" >&2; exit 2; }
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$init/stxinit.sha256")
[ -f "$kernels/kernel.size" ] && sed "s/^/kernel size ($kernconf): /" "$kernels/kernel.size"
[ -f "$init/stxinit.size" ] && sed 's/^/stxinit size: /' "$init/stxinit.size"
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-init.txt --serve-dir "$init" --define "init_sha256=$init_sha"
python3 boot_timing.py --name "$vm" --firmware-timeout-ms 0 --runs 3 --label "$label-pid1" \
    --end pid1 --require "[ /]$kernconf arm64" --stop-after-failure --hang-dump
