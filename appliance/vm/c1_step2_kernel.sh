#!/bin/sh
# c1_step2_kernel.sh -- C1 step 2, run B (one timed run): install the built
# STXLIB-VM kernel into the "tuned" VM, time three boots that must show the
# STXLIB-VM kernel's ident, then dump its TSLOG timestamps in one more session.
set -eu
cd "$(dirname "$0")"
kernels=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}/appliance/kernels/STXLIB-VM
[ -f "$kernels/kernel.sha256" ] || { echo "c1_step2_kernel: no built kernel in $kernels" >&2; exit 2; }
sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
python3 guest_session.py --name tuned --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" --define "kernel_sha256=$sha"
python3 boot_timing.py --name tuned --firmware-timeout-ms 0 --runs 3 --label stxlib-vm \
    --require '/sys/STXLIB-VM arm64'
python3 guest_session.py --name tuned --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/tslog-dump.txt
