# appliance/shim: the STXlib kernel shim

The shim is the appliance's small kernel part (C-appliance-design.md,
questions 1 and 3). It is compiled into the appliance kernel: STXLIB-VM
names it with config(8)'s `files` directive and `device stxshim`, and
`appliance/kernel/build_kernel.sh` writes the files list that points at
`stxshim.c`. No FreeBSD source is patched, and the kernel is built with
NO_MODULES, so the shim is never a loadable module.

| File | What |
|---|---|
| `stxshim.h` | the interface: `/dev/stxshim`, `STXSHIM_REGISTER`, `STXSHIM_TERMINATE`, the result |
| `stxshim_terminate.h` | the target rule and the terminate sequence, free of kernel calls |
| `stxshim.c` | the kernel side: device, registration, death watch, process passes, sync |
| `test/test_stxshim_terminate.c` | host test of the sequence and the rule (bounded exhaustive) |
| `test/run_host_tests.sh` | builds and runs it on the M4 under ASan and UBSan |

## First form (C1, step S5)

- **Registration.** The supervisor opens `/dev/stxshim` and registers. Only
  a child of PID 1 may register, and only while no other live process is
  registered. The shim sets P_PROTECTED on it (protected from the
  out-of-memory killer; its children are not).
- **Death watch.** When the registered process exits, by any cause, the
  registration ends and the kernel prints `stxshim: supervisor exited
  pid=<n>`. PID 1 respawns the supervisor, which registers again.
- **Terminate** (owner, question 3, option 1). On the registered
  supervisor's request: SIGTERM to every process but PID 1, the supervisor
  and kernel processes; at most 1 s of grace, ended early when none is
  left; then SIGKILL, repeated each 10 ms poll until none is left (at most
  100 rounds); then one sync as by sync(2); nothing is unmounted. The kernel
  prints `stxshim: terminate term=<n> kill=<n> rounds=<n> remaining=<n>
  ms=<n>`; the supervisor gets the same counts and restarts its services by
  their registration. In the first form the supervisor asks on SIGUSR1, a
  test trigger.
- **Counters**: `sysctl kern.stxshim` (supervisor_pid, registrations,
  supervisor_exits, terminations).

## Not in the first form

- The reserved chord (Ctrl+Alt+F4) read below the console, display and
  input ownership, and the reserved memory: C2, with the framebuffer and
  input host. The C1 VM has only a serial console.
- Killing a supervisor that does not answer within a bounded time after the
  chord: C2, with the chord.
- Real-time priority and locked memory for the supervisor: C2, with the
  contention measurements (`runtime/supervisor/stxsupervisor.cpp`).

## Known limits

- "Child of PID 1" includes orphans that PID 1 adopted: the kernel records
  no difference (`proc_reparent` sets the adoptive parent as the real
  parent, `sys/kern/kern_exit.c:1657-1677`). An orphan could register in the
  window between a supervisor's death and its respawn. Closing it (PID 1
  names the supervisor to the shim, or the supervisor becomes the reaper of
  its services) is a choice for C2.
- The sync starts the writes and does not wait for them, as sync(2) does
  not.
- A process the kernel cannot kill (waiting uninterruptibly on a device)
  stays; the sequence still ends within its bounds and reports it in
  `remaining`.

## Tests

- Host: `sh appliance/shim/test/run_host_tests.sh` (M4). The target rule
  over every combination of a pid in {-1, 0, 1, 2, 3, 99999}, the three
  flags and four supervisor values; the sequence over every set of 0 to 3
  processes with 6 SIGTERM and 3 SIGKILL behaviours, forking a new target on
  SIGTERM or not (12,350 runs).
- VM: `appliance/vm/c1_s5.sh` (the scenario is described in the script).
