# C1 VM: preparation and boot-stage timing

## Files

- `vmlayout.py`: where everything lives under
  `/Volumes/Storage/stxlib_workers/appliance/` (owner's storage rule,
  2026-11-13) and the default QEMU and firmware paths on the M4.
- `prepare_vm.py`: checks the downloaded image against the release's
  `CHECKSUM.SHA256`, decompresses it once to a read-only pristine image,
  and makes a VM directory with a copy-on-write overlay disk and its own
  UEFI variable store (the M4's QEMU 10.1.3 has `edk2-aarch64-code.fd` and
  no aarch64 variable file).
- `bootstages.py`: the stages recognised on the serial console, each tied to
  the FreeBSD source line that prints it (pin `fb9033d2876d`), and the
  tracker that times them.
- `boot_timing.py`: boots the VM under QEMU (UEFI, serial console on
  standard input and output, no display), timestamps every byte, records the
  stages, stops QEMU at the login prompt or a time-out, writes logs and a
  summary.
- `test_bootstages.py`: tests of stage recognition on assembled transcripts.
- `guest_session.py` and `sessions/*.txt`: log in on the stock kernel and run
  commands; `{http}` serves a host directory, `{recv}` takes one stream back
  (`--receive-file`).
- `c1_step2_kernel.sh KERNCONF`: a built kernel in a fresh VM, timed to
  login. `c1_pid1.sh KERNCONF`: a built kernel plus our PID 1
  (`runtime/init/`), timed to its marker with `boot_timing --end pid1`; the
  marker also carries the in-guest uptime and generic-timer count
  (`pid1_uptime`, `pid1_counter`). `diag_hang.sh`: symbolise a hung boot.
- `c1_sysroot.sh`: a cross sysroot (headers, static libraries) taken from
  the pinned image's own userland, for building `runtime/init/`.

The appliance boot ends at our PID 1's marker, not at a login prompt
(owner, 2026-11-13: no login); the login path remains for the stock
baseline.

## What a run measures

Host-observed times from QEMU's start to: the edk2 boot option message
(optional, unconfirmed in this firmware build), the loader banner, the
countdown, the loader's final "Booting [...]..." (autoboot path only), the
kernel's first line, "Trying to mount root from", the first rc message, and
the login prompt. Intervals: firmware, loader, kernel to root, root to
userland, userland to login, total. The stock loader waits 10 s by default
(`stand/common/boot.c:185-191`); `--skip-countdown` answers it with Enter
so the same image also gives the loader's time without the wait.

These are not the appliance's numbers: the stock image runs the full rc
system. They are the baseline every later change is measured against, on
the same VM and firmware.

## The granted run (asked of the coordinator; needs the integrator's downloads)

From `sb2/stxlib` on the M4, through `bin/m4sbrun sb2` (or
`sb2/dev/tools/c1_stock_timing.sh`, which runs the same three steps):

1. `python3 appliance/vm/prepare_vm.py --image <path of FreeBSD-15.2-*-arm64-aarch64-ufs.qcow2.xz>`
   (the checksum file is expected beside it; `--checksums` otherwise).
2. `python3 appliance/vm/boot_timing.py --runs 3` (as shipped).
3. `python3 appliance/vm/boot_timing.py --runs 3 --skip-countdown`.

Each prints one line per run and the minimum, median and maximum per
interval; exit status 1 if any run missed a required stage. Logs stay on
`/Volumes/Storage`. Expected cost: one decompression of about 600 MB
(seconds to a minute), six boots of at most 180 s each, in practice
well under that.

## Unconfirmed until the first run

- QEMU 10.1.3 accepts `-accel hvf -cpu host -machine virt` with these pflash
  sizes on macOS 26.7.2 (no `highmem` or GIC option given).
- The stock arm64 image's console reaches the serial port in this setup, with
  the login prompt on `ttyu0`.
- edk2 prints its `BdsDxe` boot-option line (the stage is optional).
