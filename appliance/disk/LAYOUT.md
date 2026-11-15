# appliance/disk: the appliance disk, A/B roots and the failsafe path (C1 S4)

Owner decisions, 2026-11-13 (notes `decisions/2026-11-13-root-failsafe-bound-effect.md`):
the root is a small read-only UFS2 partition; updates write a new root
image to an inactive slot and reboot into it (option 1: A/B slots, the
loader counts tries, fallback after 3 failed boots); if the appliance fails
to come up, the reason is logged to the writable user partition and
Ethernet (DHCP) and SSH come up, from the pinned base, with nothing
downloaded. Where the try count lives was settled with the coordinator
from the loader's abilities at the pin (below).

## Partitions

| # | VM and x86 (GPT, UEFI) | Pi 4 (MBR, U-Boot) | Type | Size | Written by |
|---|---|---|---|---|---|
| 1 | `gpt/stxesp` | slice 1 | FAT32 ESP | 64 MB | updater only (the slot file); never the loader |
| 2 | `gpt/stxbootctl` | slice 2, part `a` | UFS2 | 4 MB | loader (try count, in place); supervisor |
| 3 | `gpt/stxroota` | slice 2, part `d` | UFS2, mounted read-only | 64 MB | updater, as raw sectors |
| 4 | `gpt/stxrootb` | slice 2, part `e` | UFS2, mounted read-only | 64 MB | updater, as raw sectors |
| 5 | `gpt/stxuser` | slice 3 | FAT32 | 128 MB | the user (any computer); the system (boot log, keys in) |
| 6 | `gpt/stxdata` | slice 4 | UFS2, soft updates, label `STXDATA` | the rest | the supervisor and stxpkg (program store), targets (their folders), sshd's host key |

The data partition (C2, owner 2026-11-15, Q5 option 1 for `/data`): the
program store (`/data/programs/<program>/`, two slots per program and the
state file the supervisor alone writes) and every target's folder
(`/data/targets/<name>`, its own uid and gid, mode 0700). It is UFS2
because FAT32 keeps one owner and one mode for a whole mount: stxpkg checks
every package file's owner and mode before each start, and targets are kept
apart by their uids. `/user` stays FAT32 with one owner, the supervisor
(Q5), so the owner can edit it from any computer. Sizes are the build's
(`build_image.sh`); the data partition is not grown to the medium yet (an
open choice in `appliance/C2_DESIGN.md`).

The Pi boot ROM and U-Boot read FAT from an MBR slice, so the Pi uses MBR
with the boot control and both roots in one BSD-labelled slice (four
primaries are not enough for six partitions); the data partition is UFS2
directly in slice 4. Each UFS file system carries a UFS label
(`STXBOOTCTL`, `STXROOTA`, `STXROOTB`, `STXDATA`) so that the kernel's root
name, `ufs:/dev/ufs/STXROOTA`, and the configuration's `/dev/ufs/STXDATA`
are the same on every layout. The system updater writes a slot by its raw
device, named per board in `/etc/stx/root-devices` (`root-devices.gpt`:
`/dev/gpt/stxroota`, `/dev/gpt/stxrootb`; `root-devices.pi4`:
`/dev/mmcsd0s2d`, `/dev/mmcsd0s2e`), then sets the written file system's UFS
label to its slot's.

Root sizes are measured (`appliance/disk/root_manifest.txt`,
`measure_root.sh`); the owner expects under 64 MB.

## How a boot picks its slot

- `loader.efi` on the ESP reads `/efi/freebsd/loader.env` from the ESP
  (`stand/efi/loader/main.c:1087-1125, 1289-1302`). It sets `rootdev` to the
  boot-control partition, which the loader then uses as its first choice of
  current device (`main.c:520-530`). No firmware variable is involved.
- The boot-control partition holds `/boot/loader.conf` (menu off), the
  pinned base's `/boot/lua`, our `/boot/lua/local.lua`
  (`appliance/boot/local.lua`, included by the stock loader.lua after the
  configuration is read, `stand/lua/loader.lua:50`), and the try-count file
  `/stx/tries`.
- `local.lua` reads the slot file from the ESP, `/stx/slots.conf`:
  `good=A|B` (the last slot proven good) and `try=A|B` (the slot to boot
  next). When `try` equals `good`, it boots `good`. Otherwise it reads
  `/stx/tries`; below 3 it writes the count plus one and boots `try`; at 3 or
  more it boots `good` and reports a fallback.
- The count is written in place: libsa's UFS writes overwrite existing
  bytes and never extend a file (`stand/libsa/ufs.c:797-823`), which is how
  the stock nextboot one-shot rewrites `nextboot.conf` from Lua
  (`stand/lua/config.lua:486-499`). The file is therefore created at its
  full fixed size (16 bytes, `tries=0` padded with blanks and a newline) and
  only rewritten. The loader can write neither UEFI variables (`efi-set`
  calls SetVariable only under `ENABLE_UPDATES`, defined nowhere,
  `stand/efi/libefi/env.c:42-58, 927-964`) nor FAT (`stand/libsa/dosfs.c:68`).
- Switching to the slot follows the stock boot-environment switch
  (`stand/lua/core.lua:595-607`): set `vfs.root.mountfrom` and `currdev`,
  reload the configuration (the slot's own `/boot/loader.conf`), then load
  the slot's kernel. `local.lua` then sets `stx.boot.slot`,
  `stx.boot.tries`, `stx.boot.fallback`, `stx.boot.reason` (its reason,
  blanks as hyphens) and, on a fallback from a slot under trial,
  `stx.boot.tried`, which the kernel passes to userspace as kernel
  environment (kenv(2)).
- Any error in `local.lua` (missing or malformed file, failed write) boots
  `good`, or `A` when even `good` is unreadable, and says why on the console.

## When a boot is good

The supervisor marks the running slot good when, within one boot:
1. it is registered with the shim;
2. the init section has run and every service it started has been running,
   or restarting by its policy, for 60 s, with the failsafe path not taken;
3. the network is up: an Ethernet interface has an IPv4 address.

Marking good writes `/stx/tries` back to `tries=0` (boot-control partition,
mounted read-write only for that write) and, when the running slot differs
from `good` in the slot file, rewrites the slot file with `good` and `try`
set to the running slot (ESP mounted read-write only for that write). When
`good` already names the running slot, the file is left alone, so an update
written into `try` during this boot is kept. A fallback boot runs the
appliance from the last good slot as usual (it is not a failsafe reason);
its failed update is logged to `/user/stx/boot.log` and, once the boot
proves good, cancelled by setting `try` back to the running slot. The log
line says why the update failed, in both ways it can:
`event=fallback tried=B loader-reason=tries-exhausted tried-last-event=failsafe-config-refused`,
where `tried-last-event` is the last event the tried slot itself wrote to the
log (its failsafe entry, read from the log's last 4 KiB), or `none` when it
wrote none: it failed before its supervisor ran (kernel or mount failure).

## Updates (C2)

`stxpkg system-update` (or `system-install` for a staged package) writes
the signed system image to the inactive slot's raw device, reads it back
against the signed SHA-256, and sets its UFS label to the slot's. The
supervisor then writes `tries=0` (boot control), rewrites the slot file
with `good` the running slot and `try` the new one (ESP), and asks stxinit
to reboot. Three failed boots return the unit to `good`. A power loss at
any point leaves either the old slot file (the old slot boots; the
half-written slot is never named) or the new one (the new slot is tried,
with the count from 0); `appliance/C2_DESIGN.md`, "Early exits", has each
point.

## The failsafe path

The supervisor takes it when the appliance cannot come up:
- the configuration is absent, unreadable or refused;
- the supervisor has been restarted 3 times or more in this boot
  (stxinit's restart count).

The path is itself an init section, `/etc/stx/failsafe.conf` in the root
(`appliance/disk/failsafe.conf`), run by the same parser and executor as the
appliance's own, so it is bounded the same way (every program has the
executor's 10 s limit). It:
1. mounts the user partition read-write (`/dev/msdosfs/STXUSER` at
   `/user`); the supervisor then appends one line to `/user/stx/boot.log`:
   the boot's uptime, slot, tries, fallback flag and the reason;
2. brings up Ethernet with DHCP: `dhclient` on each interface
   `ifconfig -l ether` lists (pinned base);
3. starts `sshd` from the pinned base with `/etc/stx/sshd_failsafe.conf`
   (root): key login only (`PermitRootLogin prohibit-password`, no
   passwords), host keys and `authorized_keys` on the user partition under
   `/user/stx/ssh/` (the root is read-only); host keys are made once with
   `ssh-keygen -A`-equivalent calls if absent, and their fingerprints are
   printed on the console so the first connection can be checked.

Nothing is downloaded: dhclient, ifconfig, sshd, ssh-keygen and their
libraries are in the root (`root_manifest.txt`).

The VM test (`appliance/vm/c1_s4.sh`) checks the login once: on the first
failsafe boot the host forwards `127.0.0.1:52224` (host-only) to the
guest's port 22 and runs one `ssh ... root@127.0.0.1 true` with a
throwaway ed25519 key made for the run; its public half is the user
partition's `authorized_keys`, and the private half is deleted when the
script exits.

## The Raspberry Pi 4 image

`appliance/vm/pi4_image.sh` (host) has a stock VM run
`appliance/disk/build_image.sh` with board `pi4`, which writes a 512 MiB
raw image, `/Volumes/Storage/stxlib_workers/appliance/pi4/stxlib-pi4.img`,
with its SHA-256 beside it (`stxlib-pi4.img.sha256`). Every slice starts on
a 4 MiB boundary:

| Slice | Offset | Size | Contents |
|---|---|---|---|
| s1, type 0x0c, active | 4 MiB | 64 MiB | FAT32 `STXESP`: the FAT partition of FreeBSD's RPI image at the pin's release, unchanged (Pi firmware, `armstub8-gic.bin`, U-Boot, device trees, overlays, `config.txt`), with the pinned `loader.efi` as `EFI/BOOT/BOOTAA64.EFI`, `efi/freebsd/loader.env` (`appliance/boot/loader.env.pi4`) and `stx/slots.conf` (`good=A`, `try=A`) |
| s2, FreeBSD, BSD label | 68 MiB | 136 MiB | `a` boot control (4 MiB), `d` root A, `e` root B (64 MiB each) |
| s3, type 0x0c | 204 MiB | 128 MiB | FAT32 `STXUSER`, with an empty `stx/ssh/` |
| s4, FreeBSD | 332 MiB | 180 MiB | UFS2 `STXDATA`: `programs/`, `targets/` |

The amd64 image (`appliance/vm/amd64_image.sh`, board `amd64`) has the VM's
GPT layout in 1 GiB, with the amd64 loader as `EFI/BOOT/BOOTX64.EFI`, the
`STXLIB-AMD64` kernel and the release's amd64 base programs; the board
checklist is `appliance/boards/x86.md`.

Both roots of every image carry the factory configuration,
`appliance/disk/appliance.conf`: the user and data partitions, a memory
`/var`, DHCP on every Ethernet interface (stxnet), the base system's sshd
with key login only (the host key on `/data/ssh`, the owner's
`authorized_keys` copied from the user partition at every boot), one SNTP
query at boot, then securelevel 1. The console is the Pi's PL011 UART on
GPIO 14/15, 115200 baud, 8N1.

Not yet proven: no Pi has booted an image. The board checklist,
`appliance/boards/pi4.md`, is the procedure and the list of what the first
boot must show.

What the owner does by hand:
1. Check the image: its SHA-256 must equal the one in `stxlib-pi4.img.sha256`
   (`shasum -a 256 stxlib-pi4.img`).
2. Write it to a microSD card of at least 1 GB, whole card, as a raw
   image: Raspberry Pi Imager ("Use custom"), balenaEtcher, or on macOS
   `diskutil unmountDisk /dev/diskN` then
   `sudo dd if=stxlib-pi4.img of=/dev/rdiskN bs=4m`, where `diskN` is the
   card per `diskutil list`, never the Mac's own disk.
3. Before the first boot, with the card still in the Mac: the `STXUSER`
   volume mounts; copy your SSH public key into it as
   `stx/ssh/authorized_keys`. Eject the card.
4. Serial console: a 3.3 V USB-to-serial adapter (never 5 V) on the
   40-pin header: adapter GND to pin 6, adapter RX to pin 8 (GPIO 14, the
   Pi's TXD), adapter TX to pin 10 (GPIO 15, the Pi's RXD); leave the
   adapter's VCC unconnected. Open it at 115200 baud, 8N1, no flow control
   (on macOS, for example, `screen /dev/tty.usbserial-XXXX 115200`).
5. Ethernet: a cable to a network with a DHCP server.
6. Power: insert the card, then connect the official 5 V 3 A USB-C supply.
   The console shows U-Boot, `STXLOADER slot=A tries=0 fallback=0
   reason=good`, the kernel, the host key fingerprint `(ED25519)`,
   `STXNET dhcp genet0 ok` and, after 60 s, `STXSUPERVISOR boot good`.
   Then `ssh root@<address>`, checking the fingerprint against the console.
