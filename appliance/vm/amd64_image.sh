#!/bin/sh
# amd64_image.sh [--boot] -- the x86-64 appliance image (C2; layout in
# appliance/disk/LAYOUT.md, board checklist appliance/boards/x86.md), beside
# the Pi 4's. Assembles the amd64 payload (image_payload.sh: the
# STXLIB-AMD64 kernel, the amd64 programs, the factory configuration, the
# release's amd64 base.txz, checked against its MANIFEST) and has the stock
# arm64 VM (k-amd64-build) write the image onto a blank raw file attached as
# its second disk (appliance/disk/build_image.sh, board amd64: the base
# programs come from base.txz, their libraries found with readelf). Writes
#   <storage>/appliance/amd64/stxlib-amd64.img         the raw image
#   <storage>/appliance/amd64/stxlib-amd64.img.sha256
# With --boot, then boots the image alone on QEMU q35 with OVMF under TCG
# (k-amd64-run) and checks the first boot's normal running: the loader's
# slot line, the supervisor, DHCP, the host key and a good boot.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS, and
# image_payload.sh's.
set -eu
cd "$(dirname "$0")"
boot_it=no
[ "${1:-}" != --boot ] || boot_it=yes
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
disk_dir=$(cd ../disk && pwd)
image_bytes=$((1024 * 1024 * 1024))

vm=k-amd64-build
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
vmdir=$storage/appliance/vm/$vm
payload=$vmdir/payload
sh image_payload.sh amd64 "$payload"
cp "$disk_dir/build_image.sh" "$payload/"
build_sha=$(shasum -a 256 "$payload/build_image.sh" | cut -d' ' -f1)

out_dir=$storage/appliance/amd64
mkdir -p "$out_dir"
out=$out_dir/stxlib-amd64.img
rm -f "$out" "$out.sha256"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(int(sys.argv[2])); f.close()' "$out" "$image_bytes"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 600 \
    --commands sessions/build-image.txt --serve-dir "$payload" \
    --appliance-disk "$out" --define "build_sha256=$build_sha" --define "board=amd64"
sha=$(shasum -a 256 "$out" | cut -d' ' -f1)
printf 'SHA256 (stxlib-amd64.img) = %s\n' "$sha" > "$out.sha256"
echo "amd64_image: $out bytes=$(stat -f %z "$out") sha256=$sha"
[ "$boot_it" = yes ] || exit 0

# The boot: a copy of the image (the published one stays as built), OVMF's
# variable store copied into the run VM's directory.
run=k-amd64-run
rundir=$storage/appliance/vm/$run
mkdir -p "$rundir"
cp /opt/homebrew/share/qemu/edk2-i386-vars.fd "$rundir/vars.fd"
cp "$out" "$rundir/appliance.raw"
python3 scenario.py --name "$run" --arch x86_64 --firmware-timeout-ms 0 --label amd64-boot1 \
    --appliance-disk "$rundir/appliance.raw" --appliance-only \
    --step 'expect:120:STXLOADER slot=A tries=0 fallback=0 reason=good' \
    --step 'expect:180:STXINIT uptime_ns=\d+' \
    --step 'expect:30:STXSUPERVISOR boot slot=A tries=0 fallback=0' \
    --step 'expect:60:\(ED25519\)' \
    --step 'expect:60:STXNET dhcp \S+ ok' \
    --step 'expect:150:STXSUPERVISOR boot good slot=A' \
    --step 'monitor:system_powerdown' \
    --step 'expect:30:STXINIT shutdown power-off' \
    --step 'exit:60' || { echo "amd64_image: the boot failed"; exit 1; }
echo "amd64_image: booted and proved good on q35 + OVMF"
