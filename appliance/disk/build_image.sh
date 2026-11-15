#!/bin/sh
# build_image.sh <served-url> <disk> <board> [extra-directory] -- run inside
# the stock FreeBSD arm64 VM (C2): writes a whole appliance disk onto <disk>
# (a blank raw file attached as the VM's second disk), as
# appliance/disk/LAYOUT.md lays it out for <board>:
#
#   vm     GPT, QEMU virt arm64: BOOTAA64.EFI, kernel STXLIB-VM
#   pi4    MBR, Raspberry Pi 4: the RPI image's FAT partition, BOOTAA64.EFI,
#          kernel STXLIB-PI4
#   amd64  GPT, x86-64 UEFI (QEMU q35 + OVMF, and boards per
#          appliance/boards/x86.md): BOOTX64.EFI, kernel STXLIB-AMD64; the
#          base programs come from the release's amd64 base.txz (payload
#          file base.txz), not from this VM
#
# The payload directory at <served-url> (assembled on the host by
# appliance/vm/image_payload.sh) holds SHA256 and every file it names:
#   kernel, stxinit, stxsupervisor, stxpkg, stxnet, stxsntp   the board's
#   stxpkg-build              stxpkg for this VM (arm64), for the factory
#                             installs below
#   root_manifest.txt         what the root takes from the base system
#   root_manifest_extra.txt   optional: more of it (a test image's sockstat)
#   check_root.sh             the build-time surface checks
#   local.lua, loader.conf, loader.env, slot_loader.conf
#   failsafe.conf, sshd_failsafe.conf, sshd_appliance.conf, appliance.conf
#   master.passwd, group      the accounts; pwd_mkdb makes the databases
#   dhclient-enter-hooks      dhclient-script writes /etc/resolv.conf itself
#   root-devices              /etc/stx/root-devices for the board
#   version                   /etc/stx/version, the system image's version
#   key-<name>.pub            the trusted package keys (/etc/stx/keys/)
#   etc-stx-<name>            optional: more files for /etc/stx/<name>
#   package-<program>.stxpkg  optional: installed into slot a at the factory
#   extra-ca.pem              optional, tests only: one more trusted CA
#   authorized_keys           optional: the user partition's stx/ssh/authorized_keys
#   rpi-fat.img (pi4), base.txz (amd64)
# [extra-directory] (a directory in this VM) may add key-*.pub, etc-stx-*
# and package-*.stxpkg files; the VM scenario's throwaway test key and test
# packages come so (appliance/disk/build_test_packages.sh).
#
# Both root slots get the same root; the slot file says good=A try=A. The
# root is checked exhaustively (check_root.sh) before anything else is
# written; a finding stops the build.
set -eu
export LC_ALL=C
url=$1
disk=$2
board=$3
extra=${4:-}
case $board in vm|pi4|amd64) ;; *) echo "build_image: unknown board $board" >&2; exit 2 ;; esac
case $disk in vtbd[1-9]) ;; *) echo "build_image: refusing disk $disk" >&2; exit 2 ;; esac
[ -z "$extra" ] || [ -d "$extra" ] || { echo "build_image: no directory $extra" >&2; exit 2; }

# Sizes (LAYOUT.md). The data partition takes the rest of the disk.
esp_mib=64
bootctl_mib=4
root_mib=64
user_mib=128

work=/tmp/stximage
payload=$work/payload
rm -rf "$work"
mkdir -p "$payload"
fetch -q -o "$payload/SHA256" "$url/SHA256"
for file in $(awk '{print $2}' "$payload/SHA256"); do
    case $file in
    *[!A-Za-z0-9._-]*|.*) echo "build_image: bad payload name $file" >&2; exit 1 ;;
    esac
    fetch -q -o "$payload/$file" "$url/$file"
done
( cd "$payload" && sha256sum -c --quiet SHA256 ) || { echo "build_image: payload checksum mismatch" >&2; exit 1; }
for file in kernel stxinit stxsupervisor stxpkg stxnet stxsntp stxpkg-build root_manifest.txt local.lua \
            loader.conf loader.env slot_loader.conf failsafe.conf sshd_failsafe.conf sshd_appliance.conf \
            appliance.conf master.passwd group root-devices version check_root.sh dhclient-enter-hooks; do
    [ -f "$payload/$file" ] || { echo "build_image: payload lacks $file" >&2; exit 1; }
done
if [ -n "$extra" ]; then
    for file in "$extra"/key-*.pub "$extra"/etc-stx-* "$extra"/package-*.stxpkg; do
        [ -f "$file" ] || continue
        cp "$file" "$payload/"
    done
fi

# The base the root's programs come from: this VM, or the amd64 release.
case $board in
amd64)
    [ -f "$payload/base.txz" ] || { echo "build_image: payload lacks base.txz" >&2; exit 1; }
    src=$work/base
    mkdir -p "$src"
    tar -xf "$payload/base.txz" -C "$src"
    boot_file=BOOTX64.EFI
    ;;
*)
    src=
    boot_file=BOOTAA64.EFI
    ;;
esac
# A path of the root's source: $src is empty for this VM's own base.
at() { printf '%s%s' "$src" "$1"; }

# --- Partitions ---------------------------------------------------------
gpart destroy -F "$disk" >/dev/null 2>&1 || true
case $board in
vm|amd64)
    gpart create -s gpt "$disk" >/dev/null
    gpart add -t efi -a 1m -s "${esp_mib}m" -l stxesp "$disk" >/dev/null
    gpart add -t freebsd-ufs -a 1m -s "${bootctl_mib}m" -l stxbootctl "$disk" >/dev/null
    gpart add -t freebsd-ufs -a 1m -s "${root_mib}m" -l stxroota "$disk" >/dev/null
    gpart add -t freebsd-ufs -a 1m -s "${root_mib}m" -l stxrootb "$disk" >/dev/null
    gpart add -t ms-basic-data -a 1m -s "${user_mib}m" -l stxuser "$disk" >/dev/null
    gpart add -t freebsd-ufs -a 1m -l stxdata "$disk" >/dev/null
    esp=${disk}p1 bootctl=${disk}p2 slot_a=${disk}p3 slot_b=${disk}p4 user=${disk}p5 data=${disk}p6
    ;;
pi4)
    # MBR: s1 FAT32 LBA (0x0c, as FreeBSD's RPI image), active; s2 FreeBSD
    # with a BSD label (a boot control, d root A, e root B); s3 FAT32 LBA,
    # the user partition; s4 FreeBSD, the data partition (UFS2 directly in
    # the slice, no label), the rest. Every slice on a 4 MiB boundary (SD
    # erase blocks).
    gpart create -s mbr "$disk" >/dev/null
    gpart add -t '!12' -a 4m -s "${esp_mib}m" "$disk" >/dev/null
    gpart set -a active -i 1 "$disk" >/dev/null
    gpart add -t freebsd -a 4m -s "$((bootctl_mib + 2 * root_mib + 4))m" "$disk" >/dev/null
    gpart add -t '!12' -a 4m -s "${user_mib}m" "$disk" >/dev/null
    gpart add -t freebsd -a 4m "$disk" >/dev/null
    gpart create -s bsd "${disk}s2" >/dev/null
    gpart add -t freebsd-ufs -i 1 -a 1m -s "${bootctl_mib}m" "${disk}s2" >/dev/null
    gpart add -t freebsd-ufs -i 4 -a 1m -s "${root_mib}m" "${disk}s2" >/dev/null
    gpart add -t freebsd-ufs -i 5 -a 1m -s "${root_mib}m" "${disk}s2" >/dev/null
    esp=${disk}s1 bootctl=${disk}s2a slot_a=${disk}s2d slot_b=${disk}s2e user=${disk}s3 data=${disk}s4
    ;;
esac
# FAT32 with one sector per cluster, as the pinned installer formats its
# ESP (usr.sbin/bsdinstall/scripts/zfsboot:219).
newfs_msdos -F 32 -c 1 -L STXESP "/dev/$esp" >/dev/null
newfs -L STXBOOTCTL "/dev/$bootctl" >/dev/null
newfs -L STXROOTA "/dev/$slot_a" >/dev/null
newfs -L STXROOTB "/dev/$slot_b" >/dev/null
newfs_msdos -F 32 -c 1 -L STXUSER "/dev/$user" >/dev/null
# The data partition: UFS2 with soft updates (owners and modes per target,
# which FAT32 cannot keep: LAYOUT.md).
newfs -U -L STXDATA "/dev/$data" >/dev/null

mnt=$work/mnt
mkdir -p "$mnt/esp" "$mnt/bootctl" "$mnt/a" "$mnt/b" "$mnt/user" "$mnt/rpi"
mount -t msdosfs "/dev/$esp" "$mnt/esp"
mount "/dev/$bootctl" "$mnt/bootctl"
mount "/dev/$slot_a" "$mnt/a"
mount "/dev/$slot_b" "$mnt/b"
mount -t msdosfs "/dev/$user" "$mnt/user"

# --- ESP ----------------------------------------------------------------
if [ "$board" = pi4 ]; then
    [ -f "$payload/rpi-fat.img" ] || { echo "build_image: payload lacks rpi-fat.img" >&2; exit 1; }
    md=$(mdconfig -a -t vnode -o readonly -f "$payload/rpi-fat.img")
    mount -t msdosfs -o ro "/dev/$md" "$mnt/rpi"
    for required in start4.elf fixup4.dat bcm2711-rpi-4-b.dtb armstub8-gic.bin u-boot.bin config.txt \
                    overlays/disable-bt.dtbo; do
        [ -f "$mnt/rpi/$required" ] || { echo "build_image: RPI FAT lacks $required" >&2; exit 1; }
    done
    cp -R "$mnt/rpi/" "$mnt/esp/"
    umount "$mnt/rpi"
    mdconfig -d -u "$md"
    rm -f "$mnt/esp/EFI/BOOT/"*
    grep -q '^dtoverlay=disable-bt' "$mnt/esp/config.txt" || {
        echo "build_image: config.txt does not move the PL011 to GPIO 14/15 (dtoverlay=disable-bt)" >&2; exit 1; }
    grep -q '^enable_uart=1' "$mnt/esp/config.txt" || printf '[all]\nenable_uart=1\n' >> "$mnt/esp/config.txt"
fi
mkdir -p "$mnt/esp/EFI/BOOT" "$mnt/esp/efi/freebsd" "$mnt/esp/stx"
cp "$(at /boot/loader.efi)" "$mnt/esp/EFI/BOOT/$boot_file"
cp "$payload/loader.env" "$mnt/esp/efi/freebsd/loader.env"
printf 'good=A\ntry=A\n\n' > "$mnt/esp/stx/slots.conf"
test "$(stat -f %z "$mnt/esp/stx/slots.conf")" = 14

# --- Boot control -------------------------------------------------------
mkdir -p "$mnt/bootctl/boot" "$mnt/bootctl/stx"
cp -R "$(at /boot/lua)" "$(at /boot/defaults)" "$mnt/bootctl/boot/"
cp "$payload/local.lua" "$mnt/bootctl/boot/lua/local.lua"
cp "$payload/loader.conf" "$mnt/bootctl/boot/loader.conf"
printf 'tries=0        \n' > "$mnt/bootctl/stx/tries"
test "$(stat -f %z "$mnt/bootctl/stx/tries")" = 16

# --- Root A -------------------------------------------------------------
root=$mnt/a
list=$work/root.list
expected=$work/root.expected
: > "$list"
cat "$payload/root_manifest.txt" > "$work/manifest"
[ ! -f "$payload/root_manifest_extra.txt" ] || cat "$payload/root_manifest_extra.txt" >> "$work/manifest"
for path in $(grep -v '^#' "$work/manifest" | grep -v '^$'); do
    case $path in
    /*) ;;
    *) echo "build_image: manifest path not absolute: $path" >&2; exit 1 ;;
    esac
    if [ -e "$(at "$path")" ] || [ -L "$(at "$path")" ]; then
        echo "$path" >> "$list"
    else
        echo "STXROOT absent $path"
    fi
done
# The shared libraries the root's ELF files need, by readelf (not ldd,
# which cannot run the amd64 programs here), to a fixed point: each round
# adds the libraries the last round's files need. 16 rounds bound it; the
# base needs fewer than 4.
cp "$list" "$work/scan"
: > "$work/libraries"
round=0
while [ -s "$work/scan" ]; do
    round=$((round + 1))
    [ "$round" -le 16 ] || { echo "build_image: library closure not reached in 16 rounds" >&2; exit 1; }
    : > "$work/next"
    while IFS= read -r path; do
        [ -f "$(at "$path")" ] || continue
        readelf -h "$(at "$path")" > /dev/null 2>&1 || continue
        for library in $(readelf -d "$(at "$path")" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p'); do
            found=
            for directory in /lib /usr/lib; do
                if [ -e "$(at "$directory/$library")" ]; then found=$directory/$library; break; fi
            done
            [ -n "$found" ] || { echo "build_image: $path needs $library, found in neither /lib nor /usr/lib" >&2; exit 1; }
            grep -qxF "$found" "$work/libraries" "$list" || { echo "$found" >> "$work/libraries"; echo "$found" >> "$work/next"; }
        done
    done < "$work/scan"
    sort -u "$work/next" > "$work/scan"
done
cat "$work/libraries" >> "$list"
# The inventory check_root.sh holds the root to: each listed path, and
# everything below a listed directory.
: > "$expected"
while IFS= read -r path; do
    if [ -d "$(at "$path")" ] && [ ! -L "$(at "$path")" ]; then
        ( cd "$(at "$path")" && find . ) | sed "s|^\.|$path|" >> "$expected"
    else
        echo "$path" >> "$expected"
    fi
done < "$list"
( cd "${src:-/}" && tar -cf - $(sed 's|^/||' "$list") ) | tar -xpf - -C "$root"
# Base programs keep no set-ID bit in the appliance (nothing here needs
# one: the supervisor and sshd run as root).
( cd "$root" && find . -type f \( -perm -4000 -o -perm -2000 \) ) | while IFS= read -r file; do
    echo "STXROOT set-id removed ${file#.}"
    chmod ug-s "$root/${file#./}"
done

# Our files, mount points and generated configuration.
for directory in /boot/kernel /sbin /etc/stx /etc/stx/keys /etc/ssl /dev /var /tmp /user /data /stx \
                 /stx/bootctl /stx/esp /root; do
    mkdir -p "$root$directory"
    echo "$directory" >> "$expected"
done
chmod 755 "$root/tmp"
cp "$payload/kernel" "$root/boot/kernel/kernel"
cp "$payload/slot_loader.conf" "$root/boot/loader.conf"
echo /boot/kernel/kernel >> "$expected"
echo /boot/loader.conf >> "$expected"
if [ "$board" = amd64 ] && [ -f "$(at /boot/device.hints)" ]; then
    # The ISA serial port and keyboard hints the loader passes to the
    # amd64 kernel (uart0 at 0x3f8 for the console).
    cp "$(at /boot/device.hints)" "$root/boot/device.hints"
    echo /boot/device.hints >> "$expected"
fi
for program in stxinit stxsupervisor stxpkg stxnet stxsntp; do
    install -m 555 "$payload/$program" "$root/sbin/$program"
    echo "/sbin/$program" >> "$expected"
done
for file in failsafe.conf sshd_failsafe.conf sshd_appliance.conf appliance.conf root-devices version; do
    install -m 444 "$payload/$file" "$root/etc/stx/$file"
    echo "/etc/stx/$file" >> "$expected"
done
for file in "$payload"/etc-stx-*; do
    [ -f "$file" ] || continue
    name=${file##*/etc-stx-}
    install -m 444 "$file" "$root/etc/stx/$name"
    echo "/etc/stx/$name" >> "$expected"
done
keys=0
for file in "$payload"/key-*.pub; do
    [ -f "$file" ] || continue
    grep -qxE '[0-9a-f]{64}' "$file" && [ "$(stat -f %z "$file")" -eq 65 ] || {
        echo "build_image: $file is not 64 hex digits and LF" >&2; exit 1; }
    name=${file##*/key-}
    install -m 444 "$file" "$root/etc/stx/keys/$name"
    echo "/etc/stx/keys/$name" >> "$expected"
    keys=$((keys + 1))
done
[ "$keys" -le 8 ] || { echo "build_image: $keys trusted keys (at most 8)" >&2; exit 1; }
# The certificate authorities stxpkg's downloader trusts: the base system's
# trusted set (certctl(8)'s source), one bundle.
cat "$(at /usr/share/certs/trusted)"/*.pem > "$root/etc/ssl/cert.pem"
[ -s "$root/etc/ssl/cert.pem" ] || { echo "build_image: no trusted certificates in the base" >&2; exit 1; }
if [ -f "$payload/extra-ca.pem" ]; then
    # A test image only (the VM scenario's throwaway update server).
    cat "$payload/extra-ca.pem" >> "$root/etc/ssl/cert.pem"
    echo "STXIMAGE extra-ca added"
fi
chmod 444 "$root/etc/ssl/cert.pem"
echo /etc/ssl/cert.pem >> "$expected"
# Accounts: root (no password: key login only), sshd's privilege
# separation user, dhclient's _dhcp; nothing else.
install -m 600 "$payload/master.passwd" "$root/etc/master.passwd"
install -m 644 "$payload/group" "$root/etc/group"
pwd_mkdb -p -d "$root/etc" "$root/etc/master.passwd"
for file in /etc/master.passwd /etc/group /etc/passwd /etc/pwd.db /etc/spwd.db; do
    echo "$file" >> "$expected"
done
printf '127.0.0.1\tlocalhost\n::1\t\tlocalhost\n' > "$root/etc/hosts"
chmod 444 "$root/etc/hosts"
# dhclient-script writes /etc/resolv.conf (resolvconf(8) is not in the
# root); the root is read-only, so it is a link into the memory /var.
ln -s /var/run/resolv.conf "$root/etc/resolv.conf"
install -m 444 "$payload/dhclient-enter-hooks" "$root/etc/dhclient-enter-hooks"
echo /etc/dhclient-enter-hooks >> "$expected"
echo /etc/hosts >> "$expected"
echo /etc/resolv.conf >> "$expected"

sh "$payload/check_root.sh" "$root" "$expected" || { echo "build_image: the root has findings" >&2; exit 1; }

# --- Root B: the same ---------------------------------------------------
( cd "$root" && tar -cf - . ) | tar -xpf - -C "$mnt/b"

# --- Data: the program store and the target folders ---------------------
mount "/dev/$data" "$root/data"
mkdir -p "$root/data/programs" "$root/data/targets"
chmod 755 "$root/data" "$root/data/programs" "$root/data/targets"
installed=0
for file in "$payload"/package-*.stxpkg; do
    [ -f "$file" ] || continue
    program=${file##*/package-}
    program=${program%.stxpkg}
    # The factory install: this VM's stxpkg in the new root, so that it
    # reads the root's own keys and writes the root's /data; then the
    # program's state, good=a (the supervisor writes it from then on).
    install -m 555 "$payload/stxpkg-build" "$root/tmp/stxpkg-build"
    mount -t devfs devfs "$root/dev"
    if ! chroot "$root" /tmp/stxpkg-build install "$program" a - < "$file"; then
        umount "$root/dev"
        echo "build_image: factory install of $program refused" >&2
        exit 1
    fi
    umount "$root/dev"
    rm -f "$root/tmp/stxpkg-build"
    printf 'stx-program-state 1\ngood a\nother empty\nrefused 0\n' > "$root/data/programs/$program/state"
    chmod 644 "$root/data/programs/$program/state"
    echo "STXIMAGE factory-installed $program slot=a"
    installed=$((installed + 1))
done
data_used=$(df -k "$root/data" | awk 'NR==2 {print $3}')
data_avail=$(df -k "$root/data" | awk 'NR==2 {print $4}')
umount "$root/data"

# --- User partition: where the owner puts authorized_keys ---------------
mkdir -p "$mnt/user/stx/ssh"
if [ -f "$payload/authorized_keys" ]; then
    cp "$payload/authorized_keys" "$mnt/user/stx/ssh/authorized_keys"
fi

echo "STXROOT board=$board files=$(find "$root" -type f | wc -l | tr -d ' ') du_kb=$(du -sk "$root" | cut -f1) used_kb=$(df -k "$root" | awk 'NR==2 {print $3}') avail_kb=$(df -k "$root" | awk 'NR==2 {print $4}') kernel_bytes=$(stat -f %z "$root/boot/kernel/kernel")"
echo "STXIMAGE esp_used_kb=$(df -k "$mnt/esp" | awk 'NR==2 {print $3}') bootctl_du_kb=$(du -sk "$mnt/bootctl" | cut -f1) user_avail_kb=$(df -k "$mnt/user" | awk 'NR==2 {print $4}') data_used_kb=$data_used data_avail_kb=$data_avail packages=$installed keys=$keys"
umount "$mnt/user" "$mnt/b" "$mnt/a" "$mnt/bootctl" "$mnt/esp"
sync
gpart show "$disk" | sed 's/^/STXIMAGE gpart: /'
[ "$board" != pi4 ] || gpart show "${disk}s2" | sed 's/^/STXIMAGE gpart: /'
echo "build_image: done"
