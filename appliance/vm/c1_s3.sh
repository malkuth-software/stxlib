#!/bin/sh
# c1_s3.sh [KERNCONF] -- C1 step S3: the services registry. One fresh VM
# (k-<kernconf>-s3) set up as in c1_s2.sh with configs/s3-appliance.conf:
# the init section starts two services; "flaky" exits at once with status 3
# and is restarted with back-off 100, 200, 400, 800 ms; "sleeper" runs until
# the power button: stxinit shuts down, the supervisor stops its services and
# reports them stopped, the VM powers off.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c1_s3: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
configs=$(pwd)/configs
for file in "$kernels/kernel.sha256" "$programs/stxinit.sha256" "$programs/stxsupervisor.sha256"; do
    [ -f "$file" ] || { echo "c1_s3: missing $file" >&2; exit 2; }
done
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$programs/stxinit.sha256")
supervisor_sha=$(cut -d' ' -f1 "$programs/stxsupervisor.sha256")
config_sha=$(shasum -a 256 "$configs/s3-appliance.conf" | cut -d' ' -f1)
sed "s/^/stxsupervisor size: /" "$programs/stxsupervisor.size"

vm=k-$label-s3
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/acpi-verbose.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-config.txt --serve-dir "$configs" \
    --define "config_file=s3-appliance.conf" --define "config_sha256=$config_sha"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-programs.txt --serve-dir "$programs" \
    --define "init_sha256=$init_sha" --define "supervisor_file=stxsupervisor" \
    --define "supervisor_sha256=$supervisor_sha"

python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label services \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR start restart_count=0 ' \
    --step 'expect:5:STXSUPERVISOR config path=/etc/stx/appliance.conf sections=3 settings=5 commands=3' \
    --step 'expect:5:STXSUPERVISOR service flaky started pid=\d+ restart_count=0' \
    --step 'expect:5:STXSUPERVISOR init line=6 start ok ' \
    --step 'expect:5:STXSUPERVISOR service sleeper started pid=\d+ restart_count=0' \
    --step 'expect:5:STXSUPERVISOR init line=7 start ok ' \
    --step 'expect:5:INITRUN services started' \
    --step 'expect:5:STXSUPERVISOR init line=8 run exited status=0 ' \
    --step 'expect:5:STXSUPERVISOR init done commands=3 skipped=0 failed=0 ' \
    --step 'expect:5:STXSUPERVISOR service flaky exited pid=\d+ status=768 ran_ms=\d+ next_in_ms=100' \
    --step 'expect:5:STXSUPERVISOR service flaky started pid=\d+ restart_count=1' \
    --step 'expect:5:service flaky exited pid=\d+ status=768 ran_ms=\d+ next_in_ms=200' \
    --step 'expect:5:STXSUPERVISOR service flaky started pid=\d+ restart_count=2' \
    --step 'expect:5:service flaky exited pid=\d+ status=768 ran_ms=\d+ next_in_ms=400' \
    --step 'expect:5:STXSUPERVISOR service flaky started pid=\d+ restart_count=3' \
    --step 'expect:5:service flaky exited pid=\d+ status=768 ran_ms=\d+ next_in_ms=800' \
    --step 'expect:5:STXSUPERVISOR service flaky started pid=\d+ restart_count=4' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'expect:5:STXSUPERVISOR stop services=stopped' \
    --step 'exit:20'
