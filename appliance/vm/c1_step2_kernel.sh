#!/bin/sh
# c1_step2_kernel.sh [KERNCONF] -- run B (one timed run): install the built
# kernel KERNCONF (default STXLIB-VM) into the "tuned" VM, time three boots
# that must show that kernel's ident, then dump its TSLOG timestamps in one
# more session. Prints the kernel's recorded size first.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_step2_kernel: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}/appliance/kernels/$kernconf
[ -f "$kernels/kernel.sha256" ] || { echo "c1_step2_kernel: no built kernel in $kernels" >&2; exit 2; }
sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
[ -f "$kernels/kernel.size" ] && sed "s/^/kernel size ($kernconf): /" "$kernels/kernel.size"
python3 guest_session.py --name tuned --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$sha" --define "kernconf=$kernconf"
python3 boot_timing.py --name tuned --firmware-timeout-ms 0 --runs 3 --label "$label" \
    --require "/sys/$kernconf arm64"
python3 guest_session.py --name tuned --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/tslog-dump.txt
