#!/bin/sh
# pi4_image.sh -- the Raspberry Pi 4 SD image (C2; layout in
# appliance/disk/LAYOUT.md, board checklist appliance/boards/pi4.md).
# Assembles the pi4 payload (image_payload.sh: the STXLIB-PI4 kernel, the
# arm64 programs, the factory configuration, the FAT partition of FreeBSD's
# RPI image at the pin's release, checked against its checksum file) and has
# one stock VM (k-pi4-build) write the image onto a blank raw file attached
# as its second disk (appliance/disk/build_image.sh, board pi4). Writes
#   <storage>/appliance/pi4/stxlib-pi4.img         the raw image (dd to a card)
#   <storage>/appliance/pi4/stxlib-pi4.img.sha256  "SHA256 (stxlib-pi4.img) = <hex>"
# and prints the image's size and SHA-256.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS (the stock VM
# image), and image_payload.sh's (STXLIB_TRUST_KEYS: the package keys the
# image trusts; none by default).
set -eu
cd "$(dirname "$0")"
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
disk_dir=$(cd ../disk && pwd)
# 512 MiB: ESP 64 MiB from 4 MiB, the BSD slice 136 MiB, the user partition
# 128 MiB, the data partition the rest (about 176 MiB; build_image.sh).
image_bytes=$((512 * 1024 * 1024))

vm=k-pi4-build
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
vmdir=$storage/appliance/vm/$vm
payload=$vmdir/payload
sh image_payload.sh pi4 "$payload"
cp "$disk_dir/build_image.sh" "$payload/"
build_sha=$(shasum -a 256 "$payload/build_image.sh" | cut -d' ' -f1)

out_dir=$storage/appliance/pi4
mkdir -p "$out_dir"
out=$out_dir/stxlib-pi4.img
rm -f "$out" "$out.sha256"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(int(sys.argv[2])); f.close()' "$out" "$image_bytes"

python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 300 \
    --commands sessions/build-image.txt --serve-dir "$payload" \
    --appliance-disk "$out" --define "build_sha256=$build_sha" --define "board=pi4"

size=$(stat -f %z "$out")
[ "$size" -eq "$image_bytes" ] || { echo "pi4_image: image is $size bytes, expected $image_bytes" >&2; exit 1; }
sha=$(shasum -a 256 "$out" | cut -d' ' -f1)
printf 'SHA256 (stxlib-pi4.img) = %s\n' "$sha" > "$out.sha256"
echo "pi4_image: $out bytes=$size sha256=$sha"
