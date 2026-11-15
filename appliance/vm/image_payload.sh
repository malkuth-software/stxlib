#!/bin/sh
# image_payload.sh <board> <payload-dir> [appliance.conf] -- assemble the
# payload appliance/disk/build_image.sh reads for <board> (vm, pi4, amd64)
# into <payload-dir>, with its SHA256. Host side (the M4); copies built
# files only, downloads nothing.
#
# From <storage>/appliance: the board's kernel (kernels/STXLIB-VM,
# STXLIB-PI4 or STXLIB-AMD64) and programs (programs/ for arm64,
# programs-amd64/ for amd64; build_programs.sh), and programs/stxpkg as
# stxpkg-build (it runs in the arm64 build VM for the factory installs).
# From this repository: the boot files, the disk files, and
# [appliance.conf] (default appliance/disk/appliance.conf, the factory one).
# pi4 adds rpi-fat.img (extract_rpi_fat.py, checked against the release's
# checksum); amd64 adds base.txz (checked against the release's MANIFEST).
#
# Environment (all optional):
#   STXLIB_STORAGE          the storage root
#   STXLIB_TRUST_KEYS       a directory of <name>.pub package keys to trust
#   STXLIB_PACKAGES         a directory of <program>.stxpkg factory packages
#   STXLIB_ETC_STX          a directory of files for /etc/stx/ (tests)
#   STXLIB_EXTRA_MANIFEST   root_manifest_extra.txt (tests: sockstat)
#   STXLIB_AUTHORIZED_KEYS  a file for the user partition's authorized_keys
#   STXLIB_EXTRA_CA         one more trusted CA certificate (tests only)
#   STXLIB_SYSTEM_VERSION   /etc/stx/version (default 1)
#   STXLIB_RPI_IMAGE, STXLIB_RPI_CHECKSUMS, STXLIB_AMD64_BASE, STXLIB_AMD64_MANIFEST
set -eu
cd "$(dirname "$0")"
board=$1
payload=$2
conf=${3:-../disk/appliance.conf}
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
disk_dir=$(cd ../disk && pwd)
boot_dir=$(cd ../boot && pwd)
case $board in
vm) kernconf=STXLIB-VM programs=$storage/appliance/programs env=loader.env.vm devices=root-devices.gpt ;;
pi4) kernconf=STXLIB-PI4 programs=$storage/appliance/programs env=loader.env.pi4 devices=root-devices.pi4 ;;
amd64) kernconf=STXLIB-AMD64 programs=$storage/appliance/programs-amd64 env=loader.env.amd64 devices=root-devices.gpt ;;
*) echo "image_payload: unknown board $board" >&2; exit 2 ;;
esac
kernel=$storage/appliance/kernels/$kernconf/kernel
for file in "$kernel" "$conf" "$storage/appliance/programs/stxpkg"; do
    [ -f "$file" ] || { echo "image_payload: missing $file" >&2; exit 2; }
done
for program in stxinit stxsupervisor stxpkg stxnet stxsntp; do
    [ -f "$programs/$program" ] || { echo "image_payload: missing $programs/$program" >&2; exit 2; }
done
rm -rf "$payload"
mkdir -p "$payload"
cp "$kernel" "$payload/kernel"
for program in stxinit stxsupervisor stxpkg stxnet stxsntp; do
    cp "$programs/$program" "$payload/$program"
done
cp "$storage/appliance/programs/stxpkg" "$payload/stxpkg-build"
cp "$boot_dir/local.lua" "$boot_dir/loader.conf" "$payload/"
cp "$boot_dir/$env" "$payload/loader.env"
cp "$disk_dir/root_manifest.txt" "$disk_dir/slot_loader.conf" "$disk_dir/failsafe.conf" \
   "$disk_dir/sshd_failsafe.conf" "$disk_dir/sshd_appliance.conf" "$disk_dir/master.passwd" \
   "$disk_dir/group" "$disk_dir/check_root.sh" "$disk_dir/dhclient-enter-hooks" "$payload/"
cp "$conf" "$payload/appliance.conf"
cp "$disk_dir/$devices" "$payload/root-devices"
printf '%s\n' "${STXLIB_SYSTEM_VERSION:-1}" > "$payload/version"
if [ -n "${STXLIB_TRUST_KEYS:-}" ]; then
    for file in "$STXLIB_TRUST_KEYS"/*.pub; do
        [ -f "$file" ] || continue
        cp "$file" "$payload/key-$(basename "$file")"
    done
fi
if [ -n "${STXLIB_PACKAGES:-}" ]; then
    for file in "$STXLIB_PACKAGES"/*.stxpkg; do
        [ -f "$file" ] || continue
        cp "$file" "$payload/package-$(basename "$file")"
    done
fi
if [ -n "${STXLIB_ETC_STX:-}" ]; then
    for file in "$STXLIB_ETC_STX"/*; do
        [ -f "$file" ] || continue
        cp "$file" "$payload/etc-stx-$(basename "$file")"
    done
fi
[ -z "${STXLIB_EXTRA_MANIFEST:-}" ] || cp "$STXLIB_EXTRA_MANIFEST" "$payload/root_manifest_extra.txt"
[ -z "${STXLIB_EXTRA_CA:-}" ] || cp "$STXLIB_EXTRA_CA" "$payload/extra-ca.pem"
[ -z "${STXLIB_AUTHORIZED_KEYS:-}" ] || cp "$STXLIB_AUTHORIZED_KEYS" "$payload/authorized_keys"
case $board in
pi4)
    rpi_image=${STXLIB_RPI_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-RPI.img.xz}
    rpi_checksums=${STXLIB_RPI_CHECKSUMS:-$storage/downloads/CHECKSUM.SHA256-FreeBSD-15.2-BETA1-arm64-aarch64-RPI}
    python3 -I extract_rpi_fat.py --image "$rpi_image" --checksums "$rpi_checksums" --out "$payload/rpi-fat.img"
    ;;
amd64)
    base=${STXLIB_AMD64_BASE:-$storage/downloads/FreeBSD-15.2-BETA1-amd64-base.txz}
    manifest=${STXLIB_AMD64_MANIFEST:-$storage/downloads/FreeBSD-15.2-BETA1-amd64-MANIFEST}
    want=$(awk '$1 == "base.txz" {print $2}' "$manifest")
    have=$(shasum -a 256 "$base" | cut -d' ' -f1)
    [ -n "$want" ] && [ "$have" = "$want" ] || { echo "image_payload: base.txz sha256 $have, MANIFEST says ${want:-nothing}" >&2; exit 1; }
    cp "$base" "$payload/base.txz"
    ;;
esac
# Every name must be one build_image.sh accepts.
for file in $(ls "$payload"); do
    case $file in
    *[!A-Za-z0-9._-]*|.*) echo "image_payload: bad payload name $file" >&2; exit 1 ;;
    esac
done
( cd "$payload" && shasum -a 256 $(ls | grep -v '^SHA256$') > SHA256 )
echo "image_payload: $board $(wc -l < "$payload/SHA256" | tr -d ' ') files in $payload"
