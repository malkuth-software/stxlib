#pragma once

// STXlib's own outline-atomics helpers on Linux and FreeBSD aarch64
// (src/hosted/elf/outline_atomics_aarch64.cpp; owner, 2026-11-22, projects
// tracks/stxlib/own_outline_atomics.md).
//
// Clang and GCC compile an atomic read-modify-write on those hosts
// (-moutline-atomics, their default) into a call to a helper named
// __aarch64_<operation><size>_<ordering>, which the toolchain's runtime
// (libgcc or compiler-rt) supplies and which asks the C library's getauxval
// whether the CPU has the Large System Extensions (LSE, Armv8.1 atomics).
// STXlib supplies every one of those helpers itself, so its atomics depend
// on no host library: they read STXlib's own memo, below, and run the LSE
// instruction or the load-exclusive/store-exclusive loop that every
// AArch64 CPU has (the Raspberry Pi 4's Cortex-A72 has no LSE).
//
// The memo is one byte per module (hidden, like the toolchain's
// __aarch64_have_lse_atomics): 0 until the module's first helper call
// detects, then 1 (no LSE) or 3 (LSE). Detection is lazy and happens
// inside that first call, so no constructor runs and no host runtime is
// involved; until it has run the helpers take the loop, which is correct
// on every CPU. It is a write-once memo in the sense of AGENTS.md: derived
// from the machine, not from mutable state, so every module derives the
// same value, and two threads racing the first detection store the same
// byte.
//
// Detection reads the CPU's ID register ID_AA64ISAR0_EL1 (Atomic, bits
// 23:20, 2 or more for LSE) with mrs. User space cannot read it directly;
// the kernel traps the read and returns its sanitised system-wide value.
// - Linux has done so since 4.11 and says so with HWCAP_CPUID (bit 11 of
//   AT_HWCAP; Documentation/arch/arm64/cpu-feature-registers.rst). On an
//   older kernel the read is an undefined instruction, so the helper first
//   reads AT_HWCAP from /proc/self/auxv with its own system calls (openat,
//   read, close: no C library, no getauxval) and assumes no LSE when the
//   file cannot be read or the bit is clear.
// - FreeBSD has emulated the read since 12.0 (sys/arm64/arm64/identcpu.c,
//   user_mrs_handler); STXlib's FreeBSD is 15.2, so the helper reads it
//   directly.
// Apple's arm64 and Windows need no helpers: Apple's compilers inline every
// atomic for arm64 (every Apple arm64 CPU has LSE), and Windows' toolchains
// inline them for the target the build names. simd.hpp's Features::lse
// says what holds on each host.

#include "stxlib/types.hpp"

#if defined(__aarch64__) && (defined(__linux__) || defined(__FreeBSD__))
// The hosts STXlib builds the helpers for; CMakeLists.txt compiles
// outline_atomics_aarch64.cpp under the same condition (stxlib_outline_atomics).
#define STXLIB_OUTLINE_ATOMICS 1
#endif

#if defined(STXLIB_OUTLINE_ATOMICS)

// The module's memo: 0 (not yet detected), 1 (detected, no LSE) or 3
// (detected, LSE); bit 1 is what the helpers test. Defined in assembly. Only
// tests write it, to force one path or to send the next call through
// detection again; storing 3 on a CPU without LSE makes the next helper
// call an undefined instruction.
extern "C" __attribute__((visibility("hidden"))) u8 stxlib_outline_atomics_memo;

// Whether the CPU has LSE, detecting first if this module has not yet
// (the same detection and memo as the helpers). An ordinary AAPCS64
// function defined in assembly; no early exit (it calls nothing but the
// kernel, and only on Linux).
extern "C" __attribute__((visibility("hidden"))) bool stxlib_outline_atomics_lse() noexcept;

#endif
