# appliance/

The FreeBSD appliance (phase II; GUI.Forms takes it up at its 2.0). Plans and
decisions live on the notes branch: `planning/FORWARD_PROGRAMME.md` sections
5-8, `planning/stages/A0-*.md`, `planning/stages/C-appliance-design.md`, and
`decisions/2026-11-13-survey-round-4.md`.

| Path | What |
|---|---|
| `toolchain.env` | the one toolchain variable (LLVM pin) every appliance build reads |
| `vm/` | C1: the FreeBSD arm64 VM on the M4, its preparation and boot-stage timing |
| `kernel/` | kernel configurations, starting with the C1 draft `STXLIB-VM` |
| `shim/` | the kernel shim module (later in C1) |

The appliance's first programs live under `runtime/` at the repository root,
since they issue system calls (AGENTS.md: only `src/hosted/`, `runtime/` and
`appliance/shim/` may): PID 1 (`stxinit`) in `runtime/init/`, the supervisor
(`stxsupervisor`, first form) in `runtime/supervisor/`, the boot
configuration's format, parser, init-section executor and services
registry in `runtime/supervisor/config/` (host tests in its `test/`), what both programs
share in `runtime/common/`. `runtime/build_programs.sh` builds them against the
sysroot.

Host scripts here are Python 3, standard library only, and run on the M4
through `bin/m4sbrun` on a granted run. They are not part of the library and
issue no system calls on the appliance. Bulky files go under
`/Volumes/Storage/stxlib_workers/appliance/` on the M4 (`vm/vmlayout.py`).
