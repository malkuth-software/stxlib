#!/bin/sh
# diag_hang.sh [KERNCONF] [SECONDS] -- one diagnostic boot of the kernel
# already installed in the "tuned" VM: if it has not reached the login prompt
# after SECONDS (default 30), save every vCPU's registers from the QEMU
# monitor, then map each PC and X30 (the link register: the caller's
# return address) to a function with llvm-symbolizer on kernel.debug.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
seconds=${2:-30}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "diag_hang: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
debug=$storage/appliance/kernels/$kernconf/kernel.debug
label=hang-$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
python3 boot_timing.py --name tuned --firmware-timeout-ms 0 --runs 1 --timeout "$seconds" \
    --label "$label" --hang-dump --require "/sys/$kernconf arm64" || true
dump=$(ls -td "$storage"/appliance/logs/boot-timing/tuned/*-"$label"-1 | head -1)/hang-registers.txt
[ -f "$dump" ] || { echo "diag_hang: no register dump (the boot did not time out)"; exit 0; }
. ../toolchain.env
grep -a -o -E '(PC|X30)=[0-9a-fA-F]+' "$dump" | sort -u | while IFS== read -r name value; do
    printf '%s=%s: ' "$name" "$value"
    "$STXLIB_LLVM_BINDIR/llvm-symbolizer" --obj="$debug" "0x$value" | head -2 | tr '\n' ' '
    echo
done
grep -a -E 'PSTATE|ESR|FAR' "$dump" | head -8 || true
echo "diag_hang: dump $dump"
