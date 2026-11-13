#pragma once

// The process's SIMD level: which instruction-set tier a library's kernels
// run at, chosen once per process from what the CPU and the operating system
// support. STXlib's own kernels (math.hpp) select through it, and it is
// meant for every other kernel library of the program (stx_conv, stx_skia):
// one detection, one choice, one override. Ported from stx_auxlib's
// include/stx_auxlib/simd.hpp at b6e330a; the selection now lives in
// stxlib_state, so every module of the program shares one choice.
//
// The tiers are BFFT's (bruun_simd_backend.hpp, BRUUN_LEVEL): a scalar
// reference, one 128-bit tier (SSE2 on x86-64, NEON on arm64, both the
// architecture's baseline), and AVX2 with FMA at 256 bits. There is no AVX
// without AVX2, no SSE4 tier and no AVX-512 tier (owner, 2026-11-01). A
// library compiles one kernel variant per tier it offers and calls the
// selected one, so one binary runs at the best tier of each machine.
//
// Selection is fixed at the first call to selected_level() and never changes
// afterwards; a library resolves its kernel table from it once, so no call
// checks the CPU again.
//
// Every variant of a kernel gives the same bits as the scalar reference,
// or stays within the bound its design certifies; the forced levels exist so
// that tests run every variant on one machine and compare them.

#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib::simd {

// Ordered: a higher level can run every lower level's code.
enum class Level : u8 {
    // Portable code only; on x86-64 that still means SSE2 scalar arithmetic.
    scalar = 0,
    // 128-bit vectors: SSE2 (x86-64) or NEON (arm64).
    vector128 = 1,
    // 256-bit AVX2 with FMA3 (x86-64 only). Requires the CPU's AVX2, FMA
    // and OSXSAVE flags and the operating system's saving of the vector
    // registers (XCR0 bits 1 and 2).
    avx2_fma = 2,
};

// The CPU features the levels are made of, as detected: hardware support
// and, for AVX state, the operating system's.
struct Features final {
    bool sse2{false};
    bool neon{false};
    bool avx{false};
    bool avx2{false};
    bool fma{false};
};

// What this CPU and operating system support (CPUID and XGETBV on x86-64;
// NEON as arm64's baseline). Computed on each call; a pure function of the
// machine.
[[nodiscard]] Features detected_features() noexcept;

// The highest level this machine can run and this build can execute.
[[nodiscard]] Level hardware_level() noexcept;

// The level every library uses for the rest of the process: the hardware
// level, lowered by a request. Fixed at the first call, in any module.
//
// Requests, in order: request_level() before the first call, then the
// environment variable STX_SIMD_LEVEL (scalar, vector128 or avx2_fma) read
// at the first call through stxlib::read_variable. A request above the
// hardware level is lowered to it: a test that forces avx2_fma on an
// SSE2-only machine runs vector128, and can see that from selected_level().
// An unrecognised or unreadable value is ignored.
[[nodiscard]] Level selected_level() noexcept;

// Asks for `level` before selection happens; a later request replaces an
// earlier one. Returns false, and changes nothing, once selected_level() has
// fixed the level (a request racing the first selection either lands before
// it and is honoured, or is refused), or for a value outside Level. A
// program-level override for tests and benchmarks; libraries never call it.
[[nodiscard]] bool request_level(Level level) noexcept;

// "scalar", "sse2-128", "neon-128" or "avx2-fma-256" (BFFT's names).
[[nodiscard]] TextView level_name(Level level) noexcept;

} // namespace stxlib::simd
