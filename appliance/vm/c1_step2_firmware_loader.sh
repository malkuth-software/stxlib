#!/bin/sh
# c1_step2_firmware_loader.sh <image.qcow2.xz> <CHECKSUM.SHA256>
# C1 step 2, run A (one timed run): a second VM, "tuned", from the same
# pristine image; edk2's boot-menu timeout set to 0 through QEMU; the loader
# configured with no menu and no countdown (sessions/loader-config.txt); then
# three timed boots. The stock VM is left as the baseline.
set -eu
[ $# -eq 2 ] || { echo "usage: $0 <image.qcow2.xz> <CHECKSUM.SHA256>" >&2; exit 2; }
cd "$(dirname "$0")"
python3 prepare_vm.py --image "$1" --checksums "$2" --name tuned
python3 guest_session.py --name tuned --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 boot_timing.py --name tuned --firmware-timeout-ms 0 --runs 3 --label fw0-cfg
