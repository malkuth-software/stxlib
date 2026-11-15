# Board checklist: Raspberry Pi 4 Model B

The image is `stxlib-pi4.img` (`appliance/vm/pi4_image.sh`; layout in
`appliance/disk/LAYOUT.md`). Nothing here has run on a Pi yet. Each line
below is either a fact of the build, with its source, or a check the first
boot must pass, with the console line that shows it. Writing the card and
the serial console are in LAYOUT.md, "What the owner does by hand".

## What the image assumes

| Item | Value | Source |
|---|---|---|
| Firmware and U-Boot | from FreeBSD's RPI image at the pin's release, unchanged | `extract_rpi_fat.py`, `build_image.sh` |
| Boot path | Pi firmware, U-Boot (UEFI), `loader.efi`, `local.lua`, the slot's kernel | LAYOUT.md |
| Kernel | `STXLIB-PI4`: GENERIC minus batches 1-3, with the BCM2711 drivers | `appliance/kernel/STXLIB-PI4` |
| Ethernet | genet (`genet0`), DHCP | the kernel's SOC_BRCM_BCM2838 files |
| Storage | the SD card as `mmcsd0` (sdhci on emmc2) | `root-devices.pi4` |
| Root slot devices | `/dev/mmcsd0s2d`, `/dev/mmcsd0s2e` | `appliance/disk/root-devices.pi4` |
| Console | PL011 on GPIO 14/15, 115200 8N1 (`dtoverlay=disable-bt`, `enable_uart=1`) | `build_image.sh` |
| Clock | no battery clock: 1970 at power-on until the SNTP query sets it | `appliance.conf`, service `clock` |
| vm.overcommit | not set: FreeBSD's default, 0 | no `sysctl vm.overcommit` anywhere in the image |
| Package keys | none in the factory image (who holds the key is an open choice) | `image_payload.sh`, `STXLIB_TRUST_KEYS` |

## First boot: what the console must show, in order

1. U-Boot's banner, then `STXLOADER slot=A tries=0 fallback=0 reason=good`.
2. The kernel, then `STXINIT uptime_ns=`.
3. `STXSUPERVISOR boot slot=A tries=0 fallback=0`.
4. The host key's fingerprint, `(ED25519)`.
5. `STXNET hostname stx ok` and `STXNET dhcp genet0 ok`.
6. `STXSUPERVISOR devices ruleset=10 ...`.
7. `STXSNTP set unix=...`, once the network answers. The date matters:
   the update service checks HTTPS certificates against it.
8. After 60 s, `STXSUPERVISOR boot good slot=A`.
9. `ssh root@<address>` works with the owner's key, and the fingerprint
   matches line 4.

## Checks the first boot owes (each is a 1.0 gate item)

- **The SD card's name.** The slot devices above must exist:
  `ls /dev/mmcsd0s2d /dev/mmcsd0s2e` over SSH. If the card is another
  unit, fix `root-devices.pi4` before any system update is tried.
- **A second USB disk.** The loader names the boot medium `disk0`; with a
  USB disk attached at power-on it may take that name. Check once with one
  attached; if it does, the rule becomes "no USB storage at power-on".
- **The framebuffer.** With HDMI connected, `/dev/fb0` exists (vt's
  framebuffer through fbd) and is 32 bits deep. The drawing test target
  (owner answer 20) fills the screen with red, green and blue bars through
  the surface interface (`stxlib/surface.hpp`); the owner confirms the
  colours are in that order (fbd does not report the channel order;
  `src/core/surface_blit.hpp`).
- **Input.** A USB keyboard and mouse show as `/dev/input/event*`, visible
  only to the foreground target's group (`ls -l /dev/input` over SSH as
  root shows them owned by that group, mode 0660). The test target echoes
  the keys and the pointer position.
- **The data partition's check time.** `fsck_ffs -p` runs within the init
  command's 10 s limit on a clean partition. After a power cut it may not,
  on a large card; the boot then logs the command as failed and mounts what
  is there. Measure once with a power cut during a write.
- **Power-loss during a system update.** Cut power once while
  `stxpkg system-update` writes. The next boot must come up on the old slot
  (the slot file was not yet changed), and the next update must succeed.

## Not covered by this board in 1.0

Wi-Fi and Bluetooth (no drivers in the kernel), the camera and display
connectors, GPIO for targets (no device is in the base list; a target names
`gpioc*` in `devices =` if it needs it).
