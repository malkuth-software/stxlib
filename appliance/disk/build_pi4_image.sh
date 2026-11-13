#!/bin/sh
# build_pi4_image.sh <served-url> <disk> -- run inside the stock FreeBSD VM
# (C1 step S4, Pi image): writes the Raspberry Pi 4 SD image onto <disk>,
# which is the image file attached as the VM's second disk, as
# appliance/disk/LAYOUT.md says for the Pi: MBR, slice 1 the FAT32 ESP,
# slice 2 BSD-labelled (a boot control, d root A, e root B), slice 3 the
# FAT32 user partition.
# From <served-url> (the host's payload directory): our Pi kernel
# (STXLIB-PI4), programs and configurations, and rpi-fat.img, the FAT
# partition of FreeBSD's own RPI image at the pin's release (Pi firmware,
# U-Boot, device trees, config.txt), checked by appliance/vm/extract_rpi_fat.py
# against the release's checksum. The loader is this VM's pinned
# /boot/loader.efi; the root's base programs come from this VM per
# root_manifest.txt, as for the VM disk (build_disk.sh).
# Partitions are made with gpart and newfs, as for the VM disk, so that each
# starts on a 4 MiB boundary (SD cards' erase blocks); mkimg's MBR scheme can
# align only to a track of at most 63 sectors (usr.bin/mkimg/mkimg.c:734-735,
# mbr.c:55-60). Both roots get pi4-appliance.conf; the slot file is
# good=A try=A (no trial pending).
set -eu
url=$1
disk=$2
case $disk in vtbd[1-9]) ;; *) echo "build_pi4_image: refusing disk $disk" >&2; exit 2 ;; esac
work=/tmp/stxpi4
rm -rf "$work"; mkdir -p "$work"
for file in kernel stxinit stxsupervisor root_manifest.txt local.lua loader.conf loader.env.pi4 \
            slot_loader.conf failsafe.conf sshd_failsafe.conf pi4-appliance.conf rpi-fat.img SHA256; do
    fetch -q -o "$work/$file" "$url/$file"
done
( cd "$work" && sha256sum -c --quiet SHA256 ) || { echo "build_pi4_image: payload checksum mismatch" >&2; exit 1; }

# MBR: slice 1 FAT32 LBA (type 0x0c, as FreeBSD's RPI image,
# release/tools/arm.subr arm_create_disk), active; slice 2 FreeBSD; slice 3
# FAT32 LBA, the rest of the image.
gpart destroy -F "$disk" >/dev/null 2>&1 || true
gpart create -s mbr "$disk" >/dev/null
gpart add -t '!12' -a 4m -s 64m "$disk" >/dev/null
gpart set -a active -i 1 "$disk" >/dev/null
gpart add -t freebsd -a 4m -s 136m "$disk" >/dev/null
gpart add -t '!12' -a 4m "$disk" >/dev/null
gpart create -s bsd "${disk}s2" >/dev/null
gpart add -t freebsd-ufs -i 1 -a 1m -s 4m "${disk}s2" >/dev/null
gpart add -t freebsd-ufs -i 4 -a 1m -s 64m "${disk}s2" >/dev/null
gpart add -t freebsd-ufs -i 5 -a 1m -s 64m "${disk}s2" >/dev/null
# FAT32 with one sector per cluster, as the pinned installer formats its
# ESP (usr.sbin/bsdinstall/scripts/zfsboot:219).
newfs_msdos -F 32 -c 1 -L STXESP "/dev/${disk}s1" >/dev/null
newfs -L STXBOOTCTL "/dev/${disk}s2a" >/dev/null
newfs -L STXROOTA "/dev/${disk}s2d" >/dev/null
newfs -L STXROOTB "/dev/${disk}s2e" >/dev/null
newfs_msdos -F 32 -c 1 -L STXUSER "/dev/${disk}s3" >/dev/null

mnt=/tmp/stxpi4mnt
mkdir -p "$mnt/esp" "$mnt/bootctl" "$mnt/a" "$mnt/b" "$mnt/user" "$mnt/rpi"
mount -t msdosfs "/dev/${disk}s1" "$mnt/esp"
mount "/dev/${disk}s2a" "$mnt/bootctl"
mount "/dev/${disk}s2d" "$mnt/a"
mount "/dev/${disk}s2e" "$mnt/b"
mount -t msdosfs "/dev/${disk}s3" "$mnt/user"
md=$(mdconfig -a -t vnode -o readonly -f "$work/rpi-fat.img")
mount -t msdosfs -o ro "/dev/$md" "$mnt/rpi"

# ESP: the RPI image's FAT partition as it is (firmware, U-Boot, device
# trees, overlays, config.txt), our loader as the removable-media boot
# file, its environment file, the slot file.
for required in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb armstub8-gic.bin u-boot.bin config.txt \
                overlays/disable-bt.dtbo; do
    [ -f "$mnt/rpi/$required" ] || { echo "build_pi4_image: RPI FAT lacks $required" >&2; exit 1; }
done
cp -R "$mnt/rpi/" "$mnt/esp/"
umount "$mnt/rpi"
mdconfig -d -u "$md"
mkdir -p "$mnt/esp/EFI/BOOT" "$mnt/esp/efi/freebsd" "$mnt/esp/stx"
rm -f "$mnt/esp/EFI/BOOT/"*
cp /boot/loader.efi "$mnt/esp/EFI/BOOT/BOOTAA64.EFI"
cp "$work/loader.env.pi4" "$mnt/esp/efi/freebsd/loader.env"
printf 'good=A\ntry=A\n' > "$mnt/esp/stx/slots.conf"
# The console on the PL011 at GPIO 14 (TXD) and 15 (RXD): disable-bt moves
# it there from the Bluetooth module; enable_uart is stated, not assumed.
grep -q '^dtoverlay=disable-bt' "$mnt/esp/config.txt" || {
    echo "build_pi4_image: config.txt does not move the PL011 to GPIO 14/15 (dtoverlay=disable-bt)" >&2; exit 1; }
grep -q '^enable_uart=1' "$mnt/esp/config.txt" || printf '[all]\nenable_uart=1\n' >> "$mnt/esp/config.txt"
tr -d '\r' < "$mnt/esp/config.txt" | grep -v '^#' | grep -v '^$' | sed 's/^/STXPI4 config.txt: /'

# Boot-control partition: loader scripts and defaults, our local.lua and
# loader.conf, the fixed-size try count (16 bytes).
mkdir -p "$mnt/bootctl/boot" "$mnt/bootctl/stx"
cp -R /boot/lua /boot/defaults "$mnt/bootctl/boot/"
cp "$work/local.lua" "$mnt/bootctl/boot/lua/local.lua"
cp "$work/loader.conf" "$mnt/bootctl/boot/loader.conf"
printf 'tries=0        \n' > "$mnt/bootctl/stx/tries"
test "$(stat -f %z "$mnt/bootctl/stx/tries")" = 16

# Root A: as build_disk.sh (the manifest, the programs' libraries, our
# files, mount points), with the Pi kernel and pi4-appliance.conf.
list=/tmp/stxpi4root.list
: > "$list"
for path in $(grep -v '^#' "$work/root_manifest.txt" | grep -v '^$'); do
    if [ -e "$path" ]; then echo "$path" >> "$list"; else echo "STXROOT absent $path"; fi
done
for program in $(grep -E '^/(s?bin|usr/s?bin|usr/libexec)/' "$list"); do
    ldd -f '%p\n' "$program" 2>/dev/null || true
done | sort -u | grep '^/' >> "$list"
tar -cf - -C / $(sed 's|^/||' "$list") | tar -xpf - -C "$mnt/a"
mkdir -p "$mnt/a/boot/kernel" "$mnt/a/sbin" "$mnt/a/etc/stx" "$mnt/a/dev" "$mnt/a/var" "$mnt/a/tmp" \
         "$mnt/a/user" "$mnt/a/stx/bootctl" "$mnt/a/stx/esp" "$mnt/a/root"
cp "$work/kernel" "$mnt/a/boot/kernel/kernel"
cp "$work/slot_loader.conf" "$mnt/a/boot/loader.conf"
install -m 555 "$work/stxinit" "$work/stxsupervisor" "$mnt/a/sbin/"
cp "$work/failsafe.conf" "$work/sshd_failsafe.conf" "$mnt/a/etc/stx/"
cp "$work/pi4-appliance.conf" "$mnt/a/etc/stx/appliance.conf"
chmod 444 "$mnt/a/etc/stx/"*

# Root B: the same.
tar -cf - -C "$mnt/a" . | tar -xpf - -C "$mnt/b"

# User partition: where the owner puts authorized_keys before the first boot.
mkdir -p "$mnt/user/stx/ssh"

echo "STXROOT files=$(find "$mnt/a" -type f | wc -l | tr -d ' ') du_kb=$(du -sk "$mnt/a" | cut -f1) used_kb=$(df -k "$mnt/a" | awk 'NR==2 {print $3}') kernel_bytes=$(stat -f %z "$mnt/a/boot/kernel/kernel")"
echo "STXPI4 esp_used_kb=$(df -k "$mnt/esp" | awk 'NR==2 {print $3}') bootctl_du_kb=$(du -sk "$mnt/bootctl" | cut -f1) user_avail_kb=$(df -k "$mnt/user" | awk 'NR==2 {print $4}')"
umount "$mnt/user" "$mnt/b" "$mnt/a" "$mnt/bootctl" "$mnt/esp"
sync
gpart show "$disk" "${disk}s2" | sed 's/^/STXPI4 gpart: /'
echo "build_pi4_image: done"
