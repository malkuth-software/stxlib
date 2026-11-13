#!/bin/sh
# c1_s4.sh [KERNCONF] -- C1 step S4: the appliance disk with A/B root slots,
# the boot-control partition and the failsafe path (appliance/disk/LAYOUT.md).
# One stock VM (k-<kernconf>-s4) builds a blank raw disk as its second disk
# (appliance/disk/build_disk.sh); then that disk boots alone, five times:
#   boots 1-3: the slot file says good=A try=B; the loader tries B (tries
#     1, 2, 3); B's configuration is refused, so the supervisor takes the
#     failsafe path (user partition, host key, DHCP, sshd) and logs it;
#     B never proves good;
#   boot 4: tries exhausted: the loader falls back to A; A runs normally,
#     logs the fallback, and after 60 s marks itself good, resetting the
#     count and cancelling the try of B;
#   boot 5: A boots as the good slot and proves good again, writing nothing.
# Each boot ends with the power button.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_s4: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
disk_dir=$(cd ../disk && pwd)
boot_dir=$(cd ../boot && pwd)
for file in "$kernels/kernel" "$programs/stxinit" "$programs/stxsupervisor"; do
    [ -f "$file" ] || { echo "c1_s4: missing $file" >&2; exit 2; }
done

vm=k-$label-s4
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
vmdir=$storage/appliance/vm/$vm
payload=$vmdir/payload
rm -rf "$payload"; mkdir -p "$payload"
cp "$kernels/kernel" "$programs/stxinit" "$programs/stxsupervisor" "$payload/"
cp "$disk_dir/root_manifest.txt" "$disk_dir/slot_loader.conf" "$disk_dir/failsafe.conf" \
   "$disk_dir/sshd_failsafe.conf" "$disk_dir/build_disk.sh" "$payload/"
cp "$boot_dir/local.lua" "$boot_dir/loader.conf" "$boot_dir/loader.env.vm" "$payload/"
cp configs/s4-root-a.conf configs/s4-root-b.conf "$payload/"
( cd "$payload" && shasum -a 256 kernel stxinit stxsupervisor root_manifest.txt local.lua loader.conf \
    loader.env.vm slot_loader.conf failsafe.conf sshd_failsafe.conf s4-root-a.conf s4-root-b.conf > SHA256 )
build_sha=$(shasum -a 256 "$payload/build_disk.sh" | cut -d' ' -f1)
disk=$vmdir/appliance.raw
rm -f "$disk"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(512 * 1024 * 1024); f.close()' "$disk"

python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 300 \
    --commands sessions/build-appliance.txt --serve-dir "$payload" \
    --appliance-disk "$disk" --define "build_sha256=$build_sha"

# boot NUMBER STEP... : one boot of the appliance disk alone.
boot() {
    number=$1
    shift
    python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label "s4-boot$number" \
        --appliance-disk "$disk" --appliance-only "$@"
}

status=0
for tries in 1 2 3; do
    boot "$tries" \
        --step "expect:30:STXLOADER slot=B tries=$tries fallback=0 reason=trying" \
        --step 'expect:30:STXINIT uptime_ns=\d+' \
        --step "expect:10:STXSUPERVISOR boot slot=B tries=$tries fallback=0" \
        --step 'expect:5:STXSUPERVISOR config path=/etc/stx/appliance.conf refused line=6 ' \
        --step 'expect:5:STXSUPERVISOR failsafe reason=config-refused' \
        --step 'expect:30:\(ED25519\)' \
        --step 'expect:30:STXSUPERVISOR init done commands=8 skipped=0 failed=0 ' \
        --step 'expect:5:STXSUPERVISOR boot-log written event=failsafe-config-refused' \
        --step 'monitor:system_powerdown' \
        --step 'expect:15:STXINIT shutdown power-off' \
        --step 'exit:30' || status=1
    [ "$status" -eq 0 ] || { echo "c1_s4: boot $tries failed"; exit 1; }
done
boot 4 \
    --step 'expect:30:STXLOADER slot=A tries=3 fallback=1 reason=tries exhausted' \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR boot slot=A tries=3 fallback=1' \
    --step 'expect:5:STXSUPERVISOR config path=/etc/stx/appliance.conf sections=2 settings=2 commands=5' \
    --step 'expect:30:STXSUPERVISOR init done commands=5 skipped=0 failed=0 ' \
    --step 'expect:5:STXSUPERVISOR boot-log written event=fallback' \
    --step 'expect:75:STXSUPERVISOR boot good slot=A tries=reset slot-file=written' \
    --step 'monitor:system_powerdown' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:20' || { echo "c1_s4: boot 4 failed"; exit 1; }
boot 5 \
    --step 'expect:30:STXLOADER slot=A tries=0 fallback=0 reason=good' \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR boot slot=A tries=0 fallback=0' \
    --step 'expect:30:STXSUPERVISOR init done commands=5 skipped=0 failed=0 ' \
    --step 'expect:75:STXSUPERVISOR boot good slot=A tries=kept slot-file=kept' \
    --step 'monitor:system_powerdown' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:20' || { echo "c1_s4: boot 5 failed"; exit 1; }
echo "c1_s4: all five boots passed"
