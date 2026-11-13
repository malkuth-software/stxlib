#!/bin/sh
# c1_s5.sh [KERNCONF] -- C1 step S5: the kernel shim. One fresh VM
# (k-<kernconf>-s5) set up as in c1_s3.sh with configs/s5-appliance.conf and
# a kernel that has the shim compiled in (build_kernel.sh). The scenario
# shows: the shim ready at boot; the supervisor registered; a terminate of
# userspace asked by the "trigger" service: SIGTERM to every process but
# PID 1 and the supervisor, SIGKILL after the 1 s grace to the two that
# ignore SIGTERM ("stubborn" and its sleep), the counts from the kernel and
# the supervisor, and "stubborn" restarted by its policy; then the trigger,
# restarted, kills the supervisor: the shim reports the registered process's
# exit, PID 1 respawns the supervisor, which registers again; then the power
# button powers off.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_s5: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
configs=$(pwd)/configs
for file in "$kernels/kernel.sha256" "$programs/stxinit.sha256" "$programs/stxsupervisor.sha256"; do
    [ -f "$file" ] || { echo "c1_s5: missing $file" >&2; exit 2; }
done
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$programs/stxinit.sha256")
supervisor_sha=$(cut -d' ' -f1 "$programs/stxsupervisor.sha256")
config_sha=$(shasum -a 256 "$configs/s5-appliance.conf" | cut -d' ' -f1)
sed "s/^/kernel size: /" "$kernels/kernel.size" | head -1
sed "s/^/stxsupervisor size: /" "$programs/stxsupervisor.size"

vm=k-$label-s5
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/acpi-verbose.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-config.txt --serve-dir "$configs" \
    --define "config_file=s5-appliance.conf" --define "config_sha256=$config_sha"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-programs.txt --serve-dir "$programs" \
    --define "init_sha256=$init_sha" --define "supervisor_file=stxsupervisor" \
    --define "supervisor_sha256=$supervisor_sha"

# Which service the supervisor reaps first after the terminate is not fixed,
# so only stubborn's exit and restart are expected (each expect scans
# forward past other lines).
python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label shim \
    --step 'expect:30:stxshim: ready' \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR start restart_count=0 ' \
    --step 'expect:5:STXSUPERVISOR shim registered' \
    --step 'expect:5:STXSUPERVISOR config path=/etc/stx/appliance.conf sections=4 settings=6 commands=4' \
    --step 'expect:5:STXSUPERVISOR init done commands=4 skipped=0 failed=0 ' \
    --step 'expect:10:stxshim: terminate term=\d+ kill=[12] rounds=\d+ remaining=0 ms=\d+' \
    --step 'expect:5:STXSUPERVISOR terminate done term=\d+ kill=[12] rounds=\d+ remaining=0 ms=\d+' \
    --step 'expect:5:service stubborn exited pid=\d+ status=9 ran_ms=\d+ next_in_ms=100' \
    --step 'expect:5:service stubborn started pid=\d+ restart_count=1' \
    --step 'expect:10:stxshim: supervisor exited pid=\d+' \
    --step 'expect:5:STXINIT supervisor exited pid=\d+ status=9 ' \
    --step 'expect:5:STXSUPERVISOR start restart_count=1 ' \
    --step 'expect:5:STXSUPERVISOR shim registered' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'expect:5:STXSUPERVISOR stop services=stopped' \
    --step 'exit:20'
