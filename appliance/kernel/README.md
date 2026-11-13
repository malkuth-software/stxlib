# Kernel configurations

`STXLIB-VM` is the C1 draft: the smallest arm64 kernel that should boot the
stock 15.2 userland in QEMU's virt machine, keeping what the appliance's
design needs and timing every stage with `TSLOG`. Each line is taken from
`sys/arm64/conf/std.*` at the pin, cited in the file; the file also lists
what is left out and why. It has not been built.

## Building it (later in C1, on a granted run; not part of step 1)

On the M4, outside the repository, with the FreeBSD source at the pinned
commit (the integrator's shallow clone under `/Volumes/Storage`) and the
toolchain from `appliance/toolchain.env`:

```
. <repo>/appliance/toolchain.env
cd <freebsd-src at fb9033d2876d>
MAKEOBJDIRPREFIX=/Volumes/Storage/stxlib_workers/appliance/obj \
  tools/build/make.py --cross-bindir="$STXLIB_LLVM_BINDIR" \
  TARGET=arm64 TARGET_ARCH=aarch64 \
  KERNCONF=STXLIB-VM KERNCONFDIR=<repo>/appliance/kernel \
  XLD="$STXLIB_LLD_BINDIR/ld.lld" \
  buildkernel
```

`make.py` builds FreeBSD on macOS given a cross toolchain
(`tools/build/make.py:31-42`); `KERNCONFDIR` lets the configuration live
outside the source tree (`Makefile.inc1:1731`). Whether `XLD` is honoured
alongside `--cross-bindir`, and whether `buildkernel` needs a prior
`kernel-toolchain` step on a macOS host, are unconfirmed: the first granted
build answers both. The kernel is then copied into the VM's overlay (a
procedure for a later step) and booted with the same timing script.
