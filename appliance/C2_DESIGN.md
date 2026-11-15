# appliance: C2, the system side

C2 turns the C1 boot (stxinit, the supervisor, A/B roots, the failsafe path)
into a unit that runs registered programs and keeps itself up to date. This
note is the design the code comments point at. The disk layout and the boot's
slot choice are in `disk/LAYOUT.md`. The web configuration that drives the
supervisor from a browser is sc2's (`appliance/web/`, `runtime/web/`); this
note only fixes the interface between the two ("The supervisor interface").

The owner's answers it builds on (notes branch, `decisions/`):
- 15: only the foreground target is on the screen.
- 16: the supervisor uses FreeBSD's libc.
- 17: stock fbd, vt and evdev, opened only to the foreground target's group.
  The surface decision (2026-11-15) revised this: the foreground target now
  reaches them through the library's surface interface.
- 18: Ed25519 trust keys built into the root.
- 19: the boot gate measures only.
- 20: a test target meets the gate.
- 24: no pf.
- 25: HTTPS, with one SNTP query at boot.
- Hardening is subtraction: no jails, no Capsicum (our own code does not use
  it), no rctl.
- Q5 option 1 (2026-11-15-hand-in-questions): `/data` stays a UFS2 partition
  with per-target uids; Q5's single owner applies to `/user` only.
- Early-exit integrity (2026-11-15-early-exit-integrity): every call to code
  we do not control may be an early exit ("Early exits").

Nothing here was compiled or run on Neo. What has been proved, and by what, is
listed beside each module, with its bound. Everything else is written but not
yet built.

## Normal running

Before the init section, the supervisor mounts `/user` itself
(`boot_slots_system.h`, `mount_user_partition`; already mounted counts as
done) and chooses the configuration it runs (K18, "The supervisor
interface", item 3): the factory configuration merged with the newest valid
turn of the user configuration, or the factory configuration alone. The
init section always comes from the factory configuration; the user's own
`[init]` only says which of their targets start at boot.

The factory configuration (`disk/appliance.conf`) runs these steps, in this
order:
1. The init section sets the shutdown sysctl and the vt keyboard sysctls
   (`kbd_halt`, `kbd_poweroff`, `kbd_reboot`, `kbd_debug`, `kbd_panic` set
   to 0, only when `/dev/ttyv0` exists). The keyboard belongs to the
   foreground target and cannot reboot the unit or enter the debugger.
2. It mounts `/user` (FAT32; the supervisor has mounted it already, and the
   mount step takes the same device on the same folder as done), runs
   `fsck_ffs -p` on `STXDATA`, and mounts `/data` (UFS2 with soft updates).
3. It mounts a 32 MiB tmpfs on `/var`.
4. It sets up sshd's files:
   - the host key in `/data/ssh` (made once; its fingerprint is printed every
     boot);
   - `authorized_keys`, copied from `/user/stx/ssh/authorized_keys` each boot
     by a copy and a rename, so a power loss never leaves half a file.
5. It runs `stxnet` (Ethernet by DHCP or static, from `[network]`). stxnet
   makes the same K18 choice from the same files, so the user's `[system]`
   and `[network]` apply.
6. It starts the services `sshd` (`-D`, key login only, no forwarding, files
   under `/data/ssh`) and `clock`.
7. It sets `kern.securelevel=1`, last.

At securelevel 1:
- the system updater can still write the inactive root slot raw, because the
  slot is unmounted (level 2 would refuse it);
- stxsntp can still step the clock (`kern_time.c` limits steps only above
  level 1).

After the init section, the supervisor:
1. writes `/data/ssh/authorized_keys.access` from the chosen configuration's
   `[access]` keys (a new file, write, fsync, rename, then an fsync of the
   folder; mode 0600, never through a link). sshd reads it as its second
   `AuthorizedKeysFile`;
2. applies the device ruleset ("Devices");
3. registers with the shim ("Orphan registration");
4. starts the boot targets, the foreground target first. Each target's
   standard output and error go into a pipe the supervisor reads (K17,
   item 9 of "The supervisor interface").

Accounts are generated, not taken from the base system: `disk/master.passwd`
and `disk/group`, run through `pwd_mkdb` at build. There are three accounts:
- root, with password `*`;
- sshd;
- `_dhcp`.

None of them can log in with a password.

DNS: `/etc/resolv.conf` is a link to `/var/run/resolv.conf`, and dhclient
writes that file directly (`dhclient-enter-hooks` sets `resolvconf_enable`
to `NO`), so the read-only root needs no writable `/etc`.

`vm.overcommit` is not set anywhere: not in the factory configuration, not in
`loader.env*`, not in the kernel configurations. FreeBSD's default, 0,
applies (swap-backed overcommit accounting off; the appliance has no swap).
smi owns the mimalloc side of that choice.

## Targets

A target is a `[target-<name>]` section (`runtime/supervisor/config/boot_config.h`
has the keys). Each target is a registered program from a signed package in
the program store.

The web configuration writes its targets as `[service-<name>]` sections
(sc2's spelling). Such a section becomes a target at its first key that only
a target has (`role`, `id`, `program`, `folder`, `ports`, `devices`,
`start`, `update-url`, `update-interval`, `healthy-after`). The section is
refused (`target_setting_in_service`) when a service-only key (`directory`,
`control`) came before it. Whether the command's first word must be a
package path (target) or an absolute path (service) is decided when the
section closes. A user configuration holds targets only: a section that
stays a service is refused by `check_user_config` (rule 5).

- **Roles.** `foreground` (at most one: it holds the screen, keyboard and
  pointer) or `server`.
- **Plain separation, nothing added.** Each target runs as uid and gid
  `30000 + id` (`id` 1..999). The supervisor issues ids
  (`runtime/supervisor/config/target_ids.h`): the web page leaves `id` out of
  a new target, and `config_write` adds the lowest id never used before,
  recorded in a ledger on `/data/stx/target_ids` and never issued again. A
  target keeps its id while it exists; an id written that the ledger does
  not give that target is refused (`target_id_not_issued`). Deleting a
  target marks its id deleted; at the next boot the supervisor removes its
  folder once and keeps the id spent, so no later target inherits its
  files. Its folder is `/data/targets/<name>`; the supervisor creates
  it, owned by the target, mode 0700. Its working directory is that folder.
  No target's folder is inside another's.
- **Ports.** Declared ports (`tcp/8080`) are what the run-time surface check
  allows. There is no firewall (answer 24), so the declaration is enforced by
  that check, not by the kernel. Targets are not root, so they cannot bind
  ports below 1024.
- **Devices.** These are for the foreground target only: devfs patterns given
  to its group, mode 0660 ("Devices").
- **Start.** `start = boot` (the default) starts the target after the init
  section and the device rules, foreground first. `manual` waits for the
  control channel.
- **Launch.** The supervisor starts every target as
  `stxpkg run <program> <slot> -- <file> <args>` under the target's uid. stxpkg
  verifies the slot completely (signature, then each file's size, mode, owner
  and SHA-256) and only then calls execve on the package's file. If anything
  fails to check out, it exits 125. The registry never restarts a target that
  exits 125, and the targets layer records it as a refusal ("Programs").

## Packages

These modules are pure: `runtime/package/manifest.h`, `container.h` and
`latest.h`. They are tested on the host (`runtime/package/test/`, bounds in
commit edd7c02).

- **Manifest.** It has one canonical spelling: `stx-manifest 1`, then name,
  version, and one `file <mode> <size> <sha256> <path>` line per file, with
  paths in strictly ascending order. Modes are 0444 or 0555 only. Paths use
  the alphabet `[A-Za-z0-9._/-]` and are at most 200 bytes. `make_package.sh`
  checks the same rules.
- **Container.**
  `STXPKG 1\nmanifest N\n<manifest>signature 64\n<64 bytes><files in manifest order>`,
  with nothing after the last file. The signature is Ed25519 over
  `stxpkg signature 1\n` followed by the manifest. The prefix separates this
  purpose from any other signature made with the same key.
- **Trust keys.** At most 8 raw 32-byte public keys in `/etc/stx/keys`, built
  into the root (answer 18). Changing the keys is a system update.
- **Latest document.** Exactly `stx-latest 1`, `version`, `url` (https), at
  most 1024 bytes. It is not signed, because it only points. The package it
  names is verified, including name and version, so a forged document can at
  worst offer a package that is refused, or withhold an update.
- **Build side.** `disk/make_package.sh` uses openssl genpkey ed25519 and
  `pkeyutl -sign -rawin`, the base system's tools only.
  `disk/build_test_packages.sh` makes the test key inside the build VM,
  packages hello 1/2/3, idle 1 and the system image, then deletes the key. No
  board image trusts the test key.

## Programs

Each program has two slots, a and b. `runtime/package/program_slots.h` holds
the state machine and `store_layout.h` the paths:
`/data/programs/<program>/{state,a,b,incoming,discard}`.

- **State and writers.** There are 11 states (none; good=g with the other
  slot empty, trial(n) for n 0..2, or previous). The supervisor is the only
  writer of `state`. stxpkg writes only the slot that `install_slot` names,
  never the good slot.
- **Events.**
  - installed: a first version is kept at once; otherwise the new version
    goes on trial.
  - early exit on the trial slot: the count goes up; three early exits fall
    back.
  - refused (exit 125): falls back, or, on the good slot with a previous
    version, goes back to the previous one.
  - healthy: the process ran for `healthy-after` seconds (default 30), so the
    trial is kept and the old good slot becomes the previous version.
  - rollback, by request: swaps good and previous, or abandons a trial.
- **Proof.** Host test `test_program_slots.cpp`, exhaustive: every state
  against all 12,207,031 event sequences up to length 10, against a model of
  the two slots. Properties checked:
  - the slot to run holds an installed version that has not failed;
  - an install never writes the good slot;
  - a version that fell back never runs again;
  - at most 3 early starts per trial.
- **Supervisor restarts and stop requests.** An exit after `healthy-after`
  restarts the target on the same slot without changing the state. A stop by
  request counts as healthy for the good-boot rule (sc2's
  `ServiceStopped{by_request}`, owner question 2).
- **State file.** The canonical text is
  `stx-program-state 1\ngood <s>\nother <empty|trial n|previous>\nrefused <version>\n`.
  `refused` records the version that last fell back, so the update service
  does not install it again.

## Updates

Per target with an `update-url`, the update service checks 300 s after the
supervisor starts, then every `update-interval`. The interval defaults to
86400 s and may not be shorter than 300 s. Failed fetches retry after
min(interval, 1 h). Only one job runs at a time, across all targets and the
system.

- **The job.** It is `stxpkg update <program> <slot> <url>`, run as root.
  1. stxpkg fetches the latest document, then the package, each in a child
     dropped to uid 29999 (`stxnet::fetch_identity`).
  2. The fetch uses TLS 1.2 or later, with the chain and name checked
     against `/etc/ssl/cert.pem`. The bundle is built at image build from the
     base system's `/usr/share/certs/trusted`.
  3. Bodies are framed by Content-Length only. A response with
     Transfer-Encoding is refused. At most 3 https redirects are followed.
  4. Nothing is staged: the head is verified first, then each file streams
     into `incoming/` with its hash checked as it arrives.
- **Exit status.** 0 installed; 3 or 6 nothing newer; 4 refused; 5 input or
  output failure; 7 fetch failed. `targets_plan.h` maps these to the next
  check (proved over its whole domain on the host).
- **After an install.** The target is restarted at once, on trial.

## System updates

The system image is a package named `system` with one file, `root.img`. The
image is a whole slot. `stxpkg system-update <slot> <url>` (the update
service, from `[system] update-url`) and `stxpkg system-install <slot> <file>`
(sc2's staged `/user/stx/staging/system.img`) run these steps:
1. Verify the head; the version must be newer than `/etc/stx/version`.
2. Write the image raw to the inactive slot's device
   (`/etc/stx/root-devices`).
   - The image must carry a neutral UFS label, which the build gives as
     `STXROOTNEW`.
   - The copy refuses an image whose superblock names a root slot, before
     the superblock is whole on the disk ("Early exits").
3. fsync, read back, and compare with the signed SHA-256.
4. Set the UFS label to the slot's (`STXROOTA` or `STXROOTB`) with libufs, and
   read the label back.

The supervisor then arms the slot (`arm_system_update`):
1. `tries=0` on the boot-control partition;
2. the slot file `good=<running>`, `try=<new>`, rewritten in place;
3. it asks stxinit to reboot.

Three failed boots return the unit to `good`, as `LAYOUT.md` describes. A boot
that proves good rewrites the slot file with `good` set to itself.

**Slot file.** The slot file is always the canonical 14 bytes
(`good=X\ntry=Y\n\n`, `boot_slots.h`), and the image build writes it in that
form too. `write_slot_file` therefore overwrites it in place with one
`pwrite` and an fsync whenever the existing file has that size. It falls back
to a new file and a rename only when the size differs. On FAT, an in-place
write of the same length changes no directory entry and no cluster chain, so a
power loss leaves either the old 14 bytes or the new ones (one sector). The
rename path relies on msdosfs's rename, which is not atomic on power loss.

## Devices

`runtime/supervisor/device_rules.h` defines devfs ruleset 10, built by
subtraction:
1. One rule hides every node; hidden nodes cannot be opened, even by root.
2. The base list is shown again:
   - the libc basics;
   - pts and ptmx (sshd);
   - bpf (dhclient);
   - `stxshim`;
   - the labelled partitions and their disks (the updater writes slots raw);
   - the console.
3. The foreground target's declared devices are shown with its group, mode
   0660.

Proof: the plan is pure and tested on the host (`test_device_rules.cpp`).
Patterns follow devfs.rules(5), matched by fnmatch(3) with `FNM_PATHNAME`,
so `pts/*` sits beside `pts`.

The surface interface needs `fb0`, `ttyv0` and `input/*` for the foreground
target. The VM's test configuration declares `"ttyv*" fb0 "input/*"`.

## Surface inventory

The appliance's whole surface (files, devices, listeners) is checked three
ways, each against an exact list:

1. **Build time, the root.** `disk/check_root.sh <root> <expected>`, run by
   `build_image.sh` on slot A before it is copied to B.
   - The inventory is exact: no unexpected path and no missing path. Parents
     are included. The list is the manifest plus our files.
   - No setid file, no other-writable file, no device node.
   - Every symlink resolves (except `/etc/resolv.conf`, which points into
     tmpfs).
   - Every ELF's NEEDED libraries are present. The closure is found with
     readelf to a fixed point (at most 16 rounds), never ldd, so amd64 roots
     build in the arm64 VM.
2. **Run time, devices.** `vm/surface.sh`, in the VM image only, lists
   `/dev` at three levels. `vm/check_surface.py` compares every node with
   ruleset 10's base patterns and the declared devices, using fnmatch with
   `FNM_PATHNAME` semantics (host test `test_check_surface.py`).
3. **Run time, listeners.** The same listing includes `sockstat -l`. The
   allowed listeners are:
   - sshd on tcp/22;
   - dhclient's and stxsntp's sockets;
   - each target's declared ports.

   Anything else fails the check.

`vm/c2_m4.sh` runs checks 2 and 3 over the first boot's serial lines.

## Clock

`stxsntp pool.ntp.org` is a service with `restart = never` (answer 25).
- It sends one request at a time, with a nonce in the transmit timestamp.
- It retries with backoff from 2 s to 64 s, within one window.
- The first accepted reply steps `CLOCK_REALTIME` to the transmit time plus
  half the round trip.
- It prints `STXSNTP set ...` or `STXSNTP failed ...`.
- Nothing listens: the socket is connected to the server's address and port,
  then closed.

TLS needs a sane clock. Until stxsntp succeeds, update fetches may fail on
certificate dates; the first update check comes 300 s after start, which
covers the usual case. The pure SNTP module (`runtime/net/sntp.h`) is tested
over all 2^32 seconds and fractions, and all 65,536 header pairs.

## Images and boards

`disk/build_image.sh <url> <disk> <vm|pi4|amd64> [extra]` builds every
board's image from one script. The layout is in `LAYOUT.md`:
- GPT for the VM and amd64;
- MBR with a BSD slice for the Pi;
- partitions: ESP, boot control, roots A and B, `/user` FAT32, `/data` UFS2.

The steps:
1. Fetch the release checksum file and every file it names.
2. Fill the ESP, then boot control.
3. Build root A: the manifest plus the readelf closure, with setid bits
   stripped, plus our programs and `/etc/stx`.
4. Run `check_root.sh` on root A, then copy A to B.
5. Install the factory packages into `/data`. The build VM's `stxpkg`
   (`stxpkg-build`) runs chrooted into the new root.
6. Write `authorized_keys` to `/user`.

amd64 differences:
- the image sits beside the Pi 4's;
- `STXLIB-AMD64` kernel and `boot/loader.env.amd64`, with the EFI console
  plus the serial console;
- built from the release's amd64 base.txz, checked against its MANIFEST
  (`vm/c2_amd64_sysroot.sh`);
- booted under QEMU q35 with OVMF and TCG (`vm/boot_timing.py --arch x86_64`,
  `vm/amd64_image.sh --boot`).

Each board has a checklist in `boards/pi4.md` and `boards/x86.md`, covering:
- what the image assumes;
- the first-boot console lines, in order;
- the checks the first boot owes;
- what the board does not cover in 1.0.

## Surface interface

The library side is branch `claude/appliance-surface` (d2f89ab, from
integration main ecbd252): `include/stxlib/surface.hpp`,
`Capability::display_surface`. It maps one-to-one onto GUI.Forms' SurfaceHost
(cited in the header at GUI.Forms `claude/cp34-web-host` 15013986b). It does
not depend on the WindowCapsule.

Underneath are the stock interfaces:
- `/dev/fb0`, mapped, with a shadow BGRA8 buffer copied out on present
  (BGRX8888 or RGB565);
- `/dev/ttyv0`, in `KD_GRAPHICS` with a raw keyboard, so there is no
  terminal switching;
- `/dev/input/event*` through evdev, mapped to HID usages on page 0x07, the
  gui_forms modifier bits and US-layout text.

The appliance side is only the foreground target's `devices` line and the
ruleset above. The library code issues the system calls under
`src/hosted/freebsd/`, so the supervisor carries none of them.

## Orphan registration

C1 let any child of PID 1 register with the shim as the supervisor. Orphans
that PID 1 adopts are also its children (`kern_exit.c`), so an orphan could
register in the window between a supervisor's death and its restart.

C2 ties registration to an object instead of a pid:
- PID 1 opens `/dev/stxshim` before each supervisor start and hands that
  file to the child alone, at descriptor 3.
- The shim accepts a registration only on a file PID 1 opened, and only once
  per file.
- PID 1's own copy is close-on-exec and is closed right after the fork, and
  the supervisor marks its copy close-on-exec, so no other process ever holds
  such a file.

The rule is pure (`appliance/shim/stxshim_register.h`) and tested on the host
over its whole input domain.

Remedies considered and not taken:
- **Checking the pid PID 1 last forked.** A pid is reused, and the shim
  cannot see PID 1's fork order without new kernel state.
- **A secret in the environment or arguments.** It is readable through
  procstat by the same uid and is inherited by children.
- **Tracking `p_pptr` history.** Needs kernel changes beyond a module.

The open file needs none of these and cannot be forged: only PID 1 creates
one.

## The supervisor interface

This is the agreement with sc2, point by point from sc2's
`stages/C2-web.md` section 7 (`claude/appliance-web` e61a9df). The
coordinator took sc2's Q2-Q4 as built.

1. **Descriptor 3.** Settled with sc2's b57f926 (C2-web.md section 7):
   - in the supervisor, descriptor 3 is the shim's file (close-on-exec);
   - a service whose definition grants control gets its channel as
     descriptor 3 (dup2, or the close-on-exec flag cleared when the channel
     already is 3), then `closefrom(4)`; a service without one calls
     `closefrom(3)` (`config/system_actions.cpp`, `become_service`);
   - init programs, targets and jobs call `closefrom(3)`
     (`become_target`, `targets.cpp` `start_job`); a target's descriptors 1
     and 2 are its output pipe (item 9).
   So in any child, descriptor 3 is the control channel or nothing.
2. **SpawnRequest.** `SpawnRequest` carries `ControlAccess control`, set by
   the registry from the definition (none for targets: a target section
   refuses `control`). There is no separate argument, and confinement is
   gone (owner answer).
3. **User configuration at boot (K18).** Built in
   `runtime/supervisor/config/config_merge.h`, used by the supervisor
   (`choose_effective_config`) and by stxnet alike.
   - The choice: the newest of `/user/stx/config.{0,1}` (sc2's turn files,
     `store/turn_file.h`), then the older one, then the factory configuration
     alone. Each turn is tried in order: it parses, it keeps
     `check_user_config`, it merges, the merged text parses. The first that
     passes all four is used; the console says which, and why any other was
     passed over. A failsafe boot reads no user configuration.
   - The merge, line by line: the factory's `[system]` with the user's
     settings replacing it key by key; the user's `[network]` and `[access]`
     in place of the factory's when the user has them; the factory's
     `[init]` and services; then the user's targets, each given
     `start = boot` when the user's `[init]` holds `start <name>`, else
     `start = manual`. The user's `[init]` is dropped.
   - `check_user_config` gains three rules, appended so the wire codes stay:
     5 `service_not_target` (a user section must be a target), 6
     `target_start_not_allowed` (enabled is said only by the `[init]` start
     line), 7 `access_setting_not_allowed` (`[access]` holds only
     `ssh-key-1`..`ssh-key-8`, each an OpenSSH public key line of a type sshd
     takes).
   - `config_write` is checked against the factory configuration alone,
     never the merged one, so the user's own targets are never "taken".
4. **Syntax.** The parser accepts K11's target keys in `[service-*]`
   sections ("Targets"), so the targets page no longer answers 422. sc2's
   other names in its section 3.3 are accepted as proposed:
   - `[system] hostname` (stxnet reads it);
   - `[network] mode/address/gateway/dns` (the parser and stxnet read them;
     static mode without an interface uses the first Ethernet interface);
   - `[access] ssh-key-1..8` (item 3; written for sshd after the init
     section, "Normal running").
5. **Program and system commands.** `SystemControl`
   (`runtime/supervisor/control_channels.h`) maps them onto `Targets`
   (`runtime/supervisor/targets.h`); `program_answers.h` holds the mapping:

   | sc2 command | Supervisor call |
   |---|---|
   | `program_state` | `status()` |
   | `program_install` (staged file) | `request_install()`, after the staged file is checked to be a regular file |
   | `program_install` (https URL) | `request_update_from()` |
   | `program_rollback` | `rollback()` |
   | `program_check` | `request_update()` |
   | `system_update` (staged image) | `request_system_install()` |
   | `operation_state` | `last_operation()` |

   - Results: `accepted` is ok; `unknown_target` is `unknown_service`;
     `busy` is busy; `nothing_to_do` is `source_unavailable` for a URL
     install and `unavailable` otherwise; `failed` is `unavailable`. A
     staged file that is absent is `source_unavailable`.
   - A finished job's result, from stxpkg's exit status: 0 ok; 3 or 6 ok
     for a look and `unavailable` for an install; 4 `signature_refused`; 7
     `source_unavailable`; anything else, or a signal, `unavailable`.
   - The ProgramRecord: active is the running slot, else the slot the state
     runs next; last good is the good slot; on trial when the active slot is
     the trial's; failed starts are the trial's early exits; versions are
     decimal; `update_available` is always false.
   - URLs are https only, at most 255 bytes (the downloader's limit); a
     longer one answers `source_unavailable`.
6. **Staging paths.** `/user/stx/staging/program.pkg` (at most 64 MiB) and
   `/user/stx/staging/system.img` (at most 128 MiB). stxpkg verifies them as
   it reads, with the same format as a download, and never trusts sc2's
   SHA-256, which is only for display.
7. **Reboot.** As sc2 wrote it: stop every service, `sync()`,
   `reboot(RB_AUTOBOOT)`, or `_exit` so that PID 1 takes over.
8. **A stop by request is healthy.** Built by sc2; the good-boot rule
   accepts it.
9. **Per-target log tail (K17).** Each target's standard output and error
   go into a pipe; the supervisor keeps the newest 4096 bytes per target in
   memory (`target_log.h`, `log_ring.h`), with a line
   `--- start pid=<n> slot=<s>` at each start. No file is written, so no
   log decision is needed and a target cannot fill a disk. The control
   protocol's command 18, `target_log_tail` (read access, names the
   target), answers in the `boot_log_tail` layout; a service that is not a
   target, or a target that wrote nothing, answers with no text. The main
   loop wakes for output (the pipes join the control kqueue) and reads at
   most 64 KiB per target per round. The web side is
   `GET /api/targets/<name>/log` (a session, as every GET), shown by the
   Output button on the Targets page.
   The start of each run is also echoed to the console (coordinator's
   call, 2026-11-15; `console_echo.h`). The first 512 bytes after each
   start, newlines included, go out one line at a time as
   `STXTARGET <name> out <text>`. Lines longer than 100 bytes are split
   into 100-byte pieces, so a console line stays within 160 bytes. Bytes
   outside printable ASCII (tab kept) are shown as `?`. The bound is per
   start, and the registry's restart back-off bounds the starts, so a
   chatty or crash-looping target cannot hold the serial line. Everything,
   echoed or not, goes into the ring. The echo adds no setting: the console
   is already the diagnostic channel, and the C2 scenario reads the
   targets' proof lines there.
10. **FAT32 owner.**
    - `/user` has one owner, the supervisor (root): the web service runs as
      root for now, and staged files are written there.
    - Targets never use `/user`. Their folders are on `/data` (UFS2) under
      their own uids (Q5 option 1).
11. **K14's GET client.** It is mine. `runtime/net/http_head.h` reads
    response heads by Content-Length only and refuses Transfer-Encoding.
    Update servers are ours, so they need no chunked encoding. sc2 needs no
    client.

## Early exits

The owner's direction: treat every call to code we do not control (kernel,
libc, OpenSSL, libufs, targets) as a possible early exit (a crash, a kill, a
power loss). At each such call, one of two forms holds:

- **W:** the invariants are whole before the call;
- **U:** the partial state is safe to unwind or restart through.

The supervisor and stxpkg never allocate from the heap: all their storage is
static or on the stack, with placement new into fixed arrays. So no allocator
unwinds. Memory that libufs and OpenSSL allocate is left to process exit.

| Point | Form | Why it holds |
|---|---|---|
| stxinit starting the supervisor | U | The supervisor is restarted with a fresh shim file; three restarts in one boot take the failsafe path. |
| Supervisor fork/exec of a child | W | The registry updates its own state only after fork returns. A failed exec shows up as an exit status, so nothing is half-recorded. |
| A target or service dying mid-run | W | Its end is an exit status and nothing more. The slot state moves only on the supervisor's own write (below). Mid-protocol on the control channel, EOF is sc2's to handle (its two-turn scheme). |
| A job (update, install) dying | U | It leaves at most `incoming/` or `discard/`. Both are removed by the next install before use, and neither is named by any state. |
| `stxpkg run` | W | It verifies everything before execve. A death before execve is exit status 125 or a signal: a refusal or an early exit, both modelled. |
| Program install: rename the old slot to `discard/`, then `incoming/` to the slot | U | It writes only the non-good slot. A loss between the two renames leaves that slot absent. The state still names it as other/previous, and running it would be a refusal that the state machine falls back from. The good slot is untouched. |
| The program state file | W | A new file, write, fsync, rename, then an fsync of the folder. It is either the old state or the new. |
| `/data` after a power loss | U | `fsck_ffs -p` before the mount (soft updates); a failed fsck fails the mount, and the boot takes the failsafe path. |
| System image write | U | Only the inactive slot is written. Until the slot file names it, it is never booted. Its neutral label means no two file systems share a root label at any point (the copy refuses a slot label before the superblock is whole). |
| Relabel (libufs `sbput`) | U | A torn superblock fails its check hash, so the slot is not a file system and is still not named. |
| `tries=0`, then the slot file | W/U | The slot file is the commit point. Before it: the old slot boots and the new slot is unused. After it: the new slot is tried with the count from 0. Three failed boots return to `good`. |
| The slot file write | W | It is rewritten in place, 14 bytes in one sector, with no directory or FAT change ("System updates"). |
| User configuration turns | U | sc2's two-turn files: a torn newest turn fails to parse, and the older one or the factory configuration is used (item 3). |
| The supervisor's `/user` mount (`mount_user_partition`) | W | It only reads afterwards. A failure means no user configuration: the factory configuration runs, and the init section's own mount line tries again. |
| Reading the turns (`read_turns`) and the K18 choice | W | Reads only, into static buffers; nothing is written. A death here is a supervisor restart that makes the same choice from the same files. stxnet makes the same choice independently. |
| The target id ledger write | W | A new file (O_NOFOLLOW, 0600), write, fsync, rename, then an fsync of `/data/stx`; it is written before the configuration turn, so an id in a stored turn is always in the ledger (an id spent on a write that then fails is simply skipped). A ledger that cannot be read is never taken as empty: issuing fails until it is repaired. |
| Removing a deleted target's folder (fts, physical, one file system) | U | A partial removal leaves files owned by an id that is never issued again. It is tried once (the entry is marked removed either way), so a later target in the same folder never loses files to a retry. |
| `authorized_keys.access` write | W | A new file (O_NOFOLLOW, 0600), write, fsync, rename, then an fsync of `/data/ssh`. It is the old file or the new one; any failure leaves the old one and says so on the console. |
| A target's output pipe | W | The pipe holds no state. When the target dies the read end sees end of file and is closed. When the supervisor dies the target's next write gets SIGPIPE (an exit, modelled like any other); the ring's contents are lost. |
| Control commands (program, system, log tail) | W | Each answer is computed from state the supervisor already holds; a job is recorded only after fork returns. A death mid-request is EOF for the web service. |
| `authorized_keys` copy | U | Copy to `.new`, then rename. A loss leaves the old file or none, and the user partition keeps the source. |
| Surface open | U | `undo_open` reverses each step in order. Modes are set, not toggled, so the next open restores a terminal left in graphics mode. |
| OpenSSL, libufs and resolver calls in stxpkg and stxsntp | U | They run in short-lived processes that write nothing until their checks pass, so an early exit is a failed job. |

## Known limits

- A merged configuration larger than 16384 bytes (`config_size_max`) is not
  used: the older turn, or the factory configuration, runs instead, and the
  console says so.
- Target output is kept in memory only, the newest 4096 bytes per target. A
  supervisor restart loses it, and a target that outlives the old
  supervisor's pipe gets SIGPIPE on its next write.
- A target that writes faster than the supervisor reads (64 KiB per round)
  waits on its full pipe; without the control kqueue the pipes are read
  once a second.
- `update_available` in the ProgramRecord is always false: the supervisor
  looks for updates only by installing them.
- The ledger keeps a target's folder as it was when its id was issued: a
  target whose folder setting later changes leaves the new folder in place
  when it is deleted (the old one is removed).

- Services are not adopted after a supervisor restart: the new supervisor
  starts them again, and the old processes are PID 1's to reap.
- Targets cannot bind ports below 1024.
- fsck time after a power cut grows with `/data`. It is measured per board in
  the checklists.
- `/dev/fb0` needs fbd compiled with vt (`check_config.py`).
- The 32-bit pixel channel order is assumed to be BGRX until the drawing
  test target checks it on each board.
- The drawing test target is not written; it needs the library's FreeBSD
  family (phase II).
- `STXLIB-AMD64` needs its `check_config.py` run before its first build.

## Open decisions

These go to the coordinator as numbered options, recommendation first.

1. **`/data` size.**
   1. Grow `/data` to the medium on first boot (gpart and growfs, once,
      before the mount). (Recommended: cards and disks vary.)
   2. Keep the build's fixed size.
2. **Who holds the signing key.**
   1. The owner, offline, with packages signed on their machine.
      (Recommended.)
   2. The M4 build host, under `/Volumes/Storage`.
3. **Partition sizes.**
   1. Measure in the granted block (`STXROOT ... used_kb avail_kb`), then set
      root slots to twice the used size, rounded up to 32 MiB.
      (Recommended.)
   2. Keep 64 MiB per root slot.
4. **The amd64 download.** FreeBSD 15.2-BETA1 amd64 `base.txz` and its
   `MANIFEST`, from download.freebsd.org
   `releases/amd64/amd64/15.2-BETA1/`. The size, about 200 MB, is to be read
   from the mirror before asking.
   1. Ask the owner for it.
   2. Defer the amd64 image.
5. **OVMF.** Confirm on the M4 that Homebrew's QEMU ships
   `edk2-x86_64-code.fd` and `edk2-i386-vars.fd`. Nothing is downloaded if
   they are present.
