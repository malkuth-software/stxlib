#!/bin/sh
# build_disk.sh <served-url> <disk> -- run inside the stock FreeBSD VM (C1
# step S4): partitions <disk> (the blank appliance disk) as
# appliance/disk/LAYOUT.md says (VM, GPT), populates the ESP, the
# boot-control partition and both root slots, and prints the root's size.
# Everything it installs from the network comes from <served-url> (the
# host's payload directory: our kernel, programs, configurations); the rest
# is copied from this VM's own pinned base per root_manifest.txt.
# Slot A gets s4-root-a.conf; slot B s4-root-b.conf. The slot file starts
# as good=A try=B (the test's update trial). The user partition gets
# authorized_keys (the run's throwaway test key, public half) for the
# failsafe sshd's login check.
set -eu
url=$1
disk=$2
case $disk in vtbd[0-9]) ;; *) echo "build_disk: refusing disk $disk" >&2; exit 2 ;; esac
work=/tmp/stxpayload
rm -rf "$work"; mkdir -p "$work"
for file in kernel stxinit stxsupervisor root_manifest.txt local.lua loader.conf loader.env.vm \
            slot_loader.conf failsafe.conf sshd_failsafe.conf s4-root-a.conf s4-root-b.conf authorized_keys \
            SHA256; do
    fetch -q -o "$work/$file" "$url/$file"
done
( cd "$work" && sha256sum -c --quiet SHA256 ) || { echo "build_disk: payload checksum mismatch" >&2; exit 1; }

gpart destroy -F "$disk" >/dev/null 2>&1 || true
gpart create -s gpt "$disk" >/dev/null
gpart add -t efi -s 64m -l stxesp "$disk" >/dev/null
gpart add -t freebsd-ufs -s 4m -l stxbootctl "$disk" >/dev/null
gpart add -t freebsd-ufs -s 64m -l stxroota "$disk" >/dev/null
gpart add -t freebsd-ufs -s 64m -l stxrootb "$disk" >/dev/null
gpart add -t ms-basic-data -l stxuser "$disk" >/dev/null
newfs_msdos -F 32 -L STXESP "/dev/${disk}p1" >/dev/null
newfs -L STXBOOTCTL "/dev/${disk}p2" >/dev/null
newfs -L STXROOTA "/dev/${disk}p3" >/dev/null
newfs -L STXROOTB "/dev/${disk}p4" >/dev/null
newfs_msdos -F 32 -L STXUSER "/dev/${disk}p5" >/dev/null

mnt=/tmp/stxmnt
mkdir -p "$mnt/esp" "$mnt/bootctl" "$mnt/a" "$mnt/b" "$mnt/user"
mount -t msdosfs "/dev/${disk}p1" "$mnt/esp"
mount "/dev/${disk}p2" "$mnt/bootctl"
mount "/dev/${disk}p3" "$mnt/a"
mount "/dev/${disk}p4" "$mnt/b"
mount -t msdosfs "/dev/${disk}p5" "$mnt/user"

# User partition: the failsafe sshd's authorized keys (sshd_failsafe.conf).
mkdir -p "$mnt/user/stx/ssh"
cp "$work/authorized_keys" "$mnt/user/stx/ssh/authorized_keys"

# ESP: the pinned base's loader as the removable-media boot file, its
# environment file, the slot file.
mkdir -p "$mnt/esp/EFI/BOOT" "$mnt/esp/efi/freebsd" "$mnt/esp/stx"
cp /boot/loader.efi "$mnt/esp/EFI/BOOT/BOOTAA64.EFI"
cp "$work/loader.env.vm" "$mnt/esp/efi/freebsd/loader.env"
printf 'good=A\ntry=B\n' > "$mnt/esp/stx/slots.conf"

# Boot-control partition: loader scripts and defaults, our local.lua and
# loader.conf, the fixed-size try count (16 bytes).
mkdir -p "$mnt/bootctl/boot" "$mnt/bootctl/stx"
cp -R /boot/lua /boot/defaults "$mnt/bootctl/boot/"
cp "$work/local.lua" "$mnt/bootctl/boot/lua/local.lua"
cp "$work/loader.conf" "$mnt/bootctl/boot/loader.conf"
printf 'tries=0        \n' > "$mnt/bootctl/stx/tries"
test "$(stat -f %z "$mnt/bootctl/stx/tries")" = 16

# Root A: the manifest, the programs' libraries, our files, mount points.
list=/tmp/stxroot.list
# Paths the base at the pin lacks (sshd-auth arrived with a later OpenSSH)
# are left out and named.
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
cp "$work/s4-root-a.conf" "$mnt/a/etc/stx/appliance.conf"
chmod 444 "$mnt/a/etc/stx/"*

# Root B: the same, with the configuration the test refuses.
tar -cf - -C "$mnt/a" . | tar -xpf - -C "$mnt/b"
chmod 644 "$mnt/b/etc/stx/appliance.conf"
cp "$work/s4-root-b.conf" "$mnt/b/etc/stx/appliance.conf"
chmod 444 "$mnt/b/etc/stx/appliance.conf"

echo "STXROOT files=$(find "$mnt/a" -type f | wc -l | tr -d ' ') du_kb=$(du -sk "$mnt/a" | cut -f1) used_kb=$(df -k "$mnt/a" | awk 'NR==2 {print $3}') kernel_bytes=$(stat -f %z "$mnt/a/boot/kernel/kernel")"
echo "STXBOOTCTL du_kb=$(du -sk "$mnt/bootctl" | cut -f1)"
umount "$mnt/user" "$mnt/b" "$mnt/a" "$mnt/bootctl" "$mnt/esp"
sync
echo "build_disk: done"
