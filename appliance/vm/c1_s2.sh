#!/bin/sh
# c1_s2.sh [KERNCONF] -- C1 step S2: the supervisor reads the boot
# configuration and runs its init section once. One fresh VM
# (k-<kernconf>-s2) set up from the stock kernel: loader configuration,
# hw.acpi.verbose, the configuration (configs/s2-appliance.conf), the kernel,
# then stxinit and stxsupervisor. The scenario boots it, expects one line per
# init command with the outcome noted in the configuration, the summary,
# then presses the power button and expects a clean power-off.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_s2: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
configs=$(pwd)/configs
for file in "$kernels/kernel.sha256" "$programs/stxinit.sha256" "$programs/stxsupervisor.sha256"; do
    [ -f "$file" ] || { echo "c1_s2: missing $file" >&2; exit 2; }
done
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$programs/stxinit.sha256")
supervisor_sha=$(cut -d' ' -f1 "$programs/stxsupervisor.sha256")
config_sha=$(shasum -a 256 "$configs/s2-appliance.conf" | cut -d' ' -f1)
sed "s/^/stxsupervisor size: /" "$programs/stxsupervisor.size"

vm=k-$label-s2
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/acpi-verbose.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-config.txt --serve-dir "$configs" \
    --define "config_file=s2-appliance.conf" --define "config_sha256=$config_sha"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-programs.txt --serve-dir "$programs" \
    --define "init_sha256=$init_sha" --define "supervisor_file=stxsupervisor" \
    --define "supervisor_sha256=$supervisor_sha"

# The mount on line 14 is guarded: ok if the image has /dev/gpt/efiesp and
# /boot/efi, skipped if it has no such device; either is accepted.
python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label init \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR start restart_count=0 ' \
    --step 'expect:5:STXSUPERVISOR config path=/etc/stx/appliance.conf sections=3 settings=3 commands=9' \
    --step 'expect:5:STXSUPERVISOR init line=13 wait-for ok ' \
    --step 'expect:5:STXSUPERVISOR init line=14 mount (ok|skipped) ' \
    --step 'expect:5:STXSUPERVISOR init line=15 set ok ' \
    --step 'expect:5:INITRUN hello from init' \
    --step 'expect:5:STXSUPERVISOR init line=16 run exited status=0 ' \
    --step 'expect:5:STXSUPERVISOR init line=17 run skipped ' \
    --step 'expect:5:STXSUPERVISOR init line=18 run exited status=256 ' \
    --step 'expect:5:STXSUPERVISOR init line=19 start ok ' \
    --step 'expect:5:STXSUPERVISOR init line=20 wait-for timed-out ' \
    --step 'expect:5:STXSUPERVISOR init line=21 run exited status=0 ' \
    --step 'expect:5:STXSUPERVISOR init done commands=9 skipped=2 failed=2 ' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'expect:5:STXSUPERVISOR stop' \
    --step 'exit:20'
