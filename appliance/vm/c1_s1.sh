#!/bin/sh
# c1_s1.sh [KERNCONF] -- C1 step S1: PID 1 starts, respawns and shuts down
# the supervisor. Two fresh VMs, each set up from the stock kernel (loader
# configuration, the kernel, then stxinit as init_path and a supervisor):
#
#   A  k-<kernconf>-s1       the real stxsupervisor. Boot to stxinit's marker
#                            and the supervisor's first start, then press the
#                            ACPI power button (QEMU system_powerdown):
#                            stxinit must shut down (SIGTERM reaches the
#                            supervisor, which stops) and the VM power off.
#   B  k-<kernconf>-s1crash  stxsupervisor-crash, which exits at once. stxinit
#                            must restart it with back-off 100, 200, 400, 800,
#                            1600, 3200 and 6400 ms, then power off as in A.
#
# A failed scenario does not stop the other; the exit status is 1 if either
# failed. Each boot is capped (scenario steps; at most about 60 s per VM).
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_s1: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
for file in "$kernels/kernel.sha256" "$programs/stxinit.sha256" "$programs/stxsupervisor.sha256" \
            "$programs/stxsupervisor-crash.sha256"; do
    [ -f "$file" ] || { echo "c1_s1: missing $file" >&2; exit 2; }
done
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$programs/stxinit.sha256")
for program in stxinit stxsupervisor stxsupervisor-crash; do
    sed "s/^/$program size: /" "$programs/$program.size"
done

set_up_vm() {
    vm=$1
    supervisor_file=$2
    supervisor_sha=$(cut -d' ' -f1 "$programs/$supervisor_file.sha256")
    python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
    python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
        --commands sessions/loader-config.txt
    # Before install-programs: once init_path names stxinit there is no login.
    python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
        --commands sessions/acpi-verbose.txt
    python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
        --commands sessions/install-kernel.txt --serve-dir "$kernels" \
        --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
    python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
        --commands sessions/install-programs.txt --serve-dir "$programs" \
        --define "init_sha256=$init_sha" --define "supervisor_file=$supervisor_file" \
        --define "supervisor_sha256=$supervisor_sha"
}

failed=0

set_up_vm "k-$label-s1" stxsupervisor
python3 scenario.py --name "k-$label-s1" --firmware-timeout-ms 0 --label power-off \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXINIT supervisor started pid=\d+ restart_count=0' \
    --step 'expect:10:STXSUPERVISOR start restart_count=0 ' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'expect:5:STXSUPERVISOR stop' \
    --step 'exit:20' || failed=1

set_up_vm "k-$label-s1crash" stxsupervisor-crash
python3 scenario.py --name "k-$label-s1crash" --firmware-timeout-ms 0 --label respawn \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR crash-test restart_count=0' \
    --step 'expect:5:STXINIT supervisor exited pid=\d+ status=\d+ ran_ms=\d+ next_in_ms=100' \
    --step 'expect:5:STXSUPERVISOR crash-test restart_count=1' \
    --step 'expect:5:next_in_ms=200' \
    --step 'expect:5:STXSUPERVISOR crash-test restart_count=2' \
    --step 'expect:5:next_in_ms=400' \
    --step 'expect:5:STXSUPERVISOR crash-test restart_count=3' \
    --step 'expect:5:next_in_ms=800' \
    --step 'expect:5:STXSUPERVISOR crash-test restart_count=4' \
    --step 'expect:5:next_in_ms=1600' \
    --step 'expect:5:STXSUPERVISOR crash-test restart_count=5' \
    --step 'expect:5:next_in_ms=3200' \
    --step 'expect:8:STXSUPERVISOR crash-test restart_count=6' \
    --step 'expect:5:next_in_ms=6400' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:20' || failed=1
echo "c1_s1: scenarios failed=$failed"
exit $failed
