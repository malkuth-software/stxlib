# Kernel configurations

- `STXLIB-GENERIC`: FreeBSD's arm64 `GENERIC` at the pin plus `TSLOG`, built
  with our toolchain. The known-good baseline every cut is measured against.
- `STXLIB-VM`: the appliance kernel for the C1 VM, written as
  `include "GENERIC"` followed by `nodevice` and `nooptions` lines, in
  numbered batches, each line with its reason (coordinator, 2026-11-13).
  The earlier build-up draft (history up to `dd95cde`) records which
  `std.*` entries standard files need.

## Tools

- `build_kernel.sh [KERNCONF]`: builds one configuration on the M4 with
  FreeBSD's `tools/build/make.py`, the cross toolchain from
  `../toolchain.env` (version checked) and `XLD` set to lld, running
  `kernel-toolchain buildkernel` with `NO_MODULES`, as FreeBSD's macOS CI
  does (`.github/workflows/cross-bootstrap-tools.yml:67-70`). `KERNCONFDIR`
  is this directory (`Makefile.inc1:1731`); config(8) also searches
  `sys/arm64/conf`, so `include "GENERIC"` resolves there
  (`Makefile.inc1:1809-1813`). Objects, logs and outputs go under
  `/Volumes/Storage/stxlib_workers/appliance/`. On success it writes
  `kernels/<KERNCONF>/` with `kernel`, `kernel.debug`, `kernel.sha256` and
  `kernel.size` (file size, `llvm-size` sections, PT_LOAD sums).
- `check_config.py <sys> <config>`: a static check before a build. It
  evaluates the configuration as config(8) does, selects the files
  `conf/files*` would compile, and lists those that reach a generated
  `*_if.h` whose `.m` is not selected (the `clknode_if.h` failure). It
  follows includes under `#if` too, so compare a cut with `STXLIB-GENERIC`
  and read only the lines the cut adds. It does not find undeclared
  functions or missing link symbols. It finds the `ce5fa24` failure.
- `../vm/c1_step2_kernel.sh [KERNCONF]`: makes a fresh VM `k-<kernconf>`,
  installs the kernel from the stock one, times up to three boots that must
  show its ident (stopping at the first failure, with the vCPU registers
  saved if it hangs), and dumps TSLOG. `../vm/diag_hang.sh` repeats one
  short boot and maps the saved PC and X30 to functions.
