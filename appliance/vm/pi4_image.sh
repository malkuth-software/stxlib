#!/bin/sh
# pi4_image.sh -- the Raspberry Pi 4 SD image (C1 step S4; layout in
# appliance/disk/LAYOUT.md). Takes the FAT partition of FreeBSD's RPI image
# at the pin's release (checked against its checksum file), stages it with
# our Pi kernel (appliance/kernel/STXLIB-PI4, built by build_kernel.sh),
# programs and configurations, and has one stock VM (k-pi4-build) write the
# image onto a blank raw file attached as its second disk
# (appliance/disk/build_pi4_image.sh). Writes
#   <storage>/appliance/pi4/stxlib-pi4.img         the raw image (dd to a card)
#   <storage>/appliance/pi4/stxlib-pi4.img.sha256  "SHA256 (stxlib-pi4.img) = <hex>"
# and prints the image's size and SHA-256.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS (the stock VM
# image), STXLIB_RPI_IMAGE, STXLIB_RPI_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
rpi_image=${STXLIB_RPI_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-RPI.img.xz}
rpi_checksums=${STXLIB_RPI_CHECKSUMS:-$storage/downloads/CHECKSUM.SHA256-FreeBSD-15.2-BETA1-arm64-aarch64-RPI}
kernel=$storage/appliance/kernels/STXLIB-PI4/kernel
programs=$storage/appliance/programs
disk_dir=$(cd ../disk && pwd)
boot_dir=$(cd ../boot && pwd)
for file in "$kernel" "$programs/stxinit" "$programs/stxsupervisor" "$rpi_image" "$rpi_checksums"; do
    [ -f "$file" ] || { echo "pi4_image: missing $file" >&2; exit 2; }
done
# 384 MiB: ESP 64 MiB from 4 MiB, the BSD slice 136 MiB, the user partition
# the remaining 180 MiB (build_pi4_image.sh).
image_bytes=$((384 * 1024 * 1024))

vm=k-pi4-build
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
vmdir=$storage/appliance/vm/$vm
payload=$vmdir/payload
rm -rf "$payload"; mkdir -p "$payload"
python3 -I extract_rpi_fat.py --image "$rpi_image" --checksums "$rpi_checksums" --out "$payload/rpi-fat.img"
cp "$kernel" "$programs/stxinit" "$programs/stxsupervisor" "$payload/"
cp "$disk_dir/root_manifest.txt" "$disk_dir/slot_loader.conf" "$disk_dir/failsafe.conf" \
   "$disk_dir/sshd_failsafe.conf" "$disk_dir/pi4-appliance.conf" "$disk_dir/build_pi4_image.sh" "$payload/"
cp "$boot_dir/local.lua" "$boot_dir/loader.conf" "$boot_dir/loader.env.pi4" "$payload/"
( cd "$payload" && shasum -a 256 kernel stxinit stxsupervisor root_manifest.txt local.lua loader.conf \
    loader.env.pi4 slot_loader.conf failsafe.conf sshd_failsafe.conf pi4-appliance.conf rpi-fat.img > SHA256 )
build_sha=$(shasum -a 256 "$payload/build_pi4_image.sh" | cut -d' ' -f1)

out_dir=$storage/appliance/pi4
mkdir -p "$out_dir"
out=$out_dir/stxlib-pi4.img
rm -f "$out" "$out.sha256"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(int(sys.argv[2])); f.close()' "$out" "$image_bytes"

python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 300 \
    --commands sessions/build-pi4.txt --serve-dir "$payload" \
    --appliance-disk "$out" --define "build_sha256=$build_sha"

size=$(stat -f %z "$out")
[ "$size" -eq "$image_bytes" ] || { echo "pi4_image: image is $size bytes, expected $image_bytes" >&2; exit 1; }
sha=$(shasum -a 256 "$out" | cut -d' ' -f1)
printf 'SHA256 (stxlib-pi4.img) = %s\n' "$sha" > "$out.sha256"
echo "pi4_image: $out bytes=$size sha256=$sha"
