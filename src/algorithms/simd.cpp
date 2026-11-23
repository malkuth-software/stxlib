// The process's SIMD level (include/stxlib/simd.hpp): CPU detection on
// x86-64 with CPUID and XGETBV, NEON as the arm64 baseline, and the
// once-per-process selection with its overrides. Ported from stx_auxlib's
// src/simd.cpp at b6e330a; SIMD128 for the Emscripten family (owner
// decision 2026-11-15). Changes: the selection word is
// ProcessState::simd_selection in stxlib_state (one per process, not one
// per module); STX_SIMD_LEVEL is read through stxlib::read_variable; the
// detection is not cached (it is a pure function of the machine, run once
// per selection and on each detected_features call). CPUID and XGETBV are
// instructions, not host interfaces, so they stay here (<cpuid.h> is the
// compiler's header). arm64's LSE flag (simd.hpp, Features::lse) comes, on
// Linux and FreeBSD, from the detection of STXlib's outline-atomics helpers
// (hosted/outline_atomics_backend.hpp): an mrs of ID_AA64ISAR0_EL1, which
// the kernel emulates, so it too is an instruction; the helpers keep the
// result, so it is read once per module rather than on each call.

#include "stxlib/simd.hpp"

#include "state/state.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/text.hpp"

#include "hosted/outline_atomics_backend.hpp"

#include <atomic>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#define STXLIB_SIMD_X86_64 1
#endif

namespace stxlib::simd {

namespace {

#if defined(STXLIB_SIMD_X86_64)
// CPUID leaf 1, ECX and EDX bits; leaf 7 subleaf 0, EBX bit.
constexpr unsigned int leaf1_edx_sse2 = 1U << 26U;
constexpr unsigned int leaf1_ecx_fma = 1U << 12U;
constexpr unsigned int leaf1_ecx_osxsave = 1U << 27U;
constexpr unsigned int leaf1_ecx_avx = 1U << 28U;
constexpr unsigned int leaf7_ebx_avx2 = 1U << 5U;
// XCR0: the operating system saves the SSE (bit 1) and AVX (bit 2) state.
constexpr u64 xcr0_sse_and_avx = 0x6U;

// XCR0, read only after CPUID reports OSXSAVE (XGETBV faults otherwise).
u64 read_xcr0() noexcept {
    unsigned int low = 0;
    unsigned int high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0U));
    const u64 result = (static_cast<u64>(high) << 32U) | low;
    return result;
}

Features detect() noexcept {
    Features features{};
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(1U, &eax, &ebx, &ecx, &edx) == 0) return features;
    features.sse2 = (edx & leaf1_edx_sse2) != 0U;
    const bool osxsave = (ecx & leaf1_ecx_osxsave) != 0U;
    const bool avx_state = osxsave && (read_xcr0() & xcr0_sse_and_avx) == xcr0_sse_and_avx;
    features.avx = avx_state && (ecx & leaf1_ecx_avx) != 0U;
    // FMA3 uses the AVX registers, so it needs the same saved state.
    features.fma = features.avx && (ecx & leaf1_ecx_fma) != 0U;
    unsigned int leaf7_eax = 0;
    unsigned int leaf7_ebx = 0;
    unsigned int leaf7_ecx = 0;
    unsigned int leaf7_edx = 0;
    if (__get_cpuid_count(7U, 0U, &leaf7_eax, &leaf7_ebx, &leaf7_ecx, &leaf7_edx) != 0) {
        features.avx2 = features.avx && (leaf7_ebx & leaf7_ebx_avx2) != 0U;
    }
    return features;
}
#else
#if defined(__aarch64__) || defined(_M_ARM64)
// Features::lse, host by host as simd.hpp states it.
bool arm64_lse() noexcept {
#if defined(STXLIB_OUTLINE_ATOMICS)
    const bool result = stxlib_outline_atomics_lse();
#elif defined(__APPLE__) || defined(__ARM_FEATURE_ATOMICS)
    const bool result = true;
#else
    const bool result = false;
#endif
    return result;
}
#endif

Features detect() noexcept {
    Features features{};
#if defined(__aarch64__) || defined(_M_ARM64)
    // Advanced SIMD is part of the AArch64 baseline.
    features.neon = true;
    features.lse = arm64_lse();
#endif
#if defined(__wasm_simd128__)
    // Clang defines __wasm_simd128__ under -msimd128
    // (clang/lib/Basic/Targets/WebAssembly.cpp:121 on llvm main, read
    // 2026-11-15; not at the LLVM revision emsdk 6.0.13 ships, which was not
    // read). The engine validated this module's SIMD instructions before
    // running it, so the feature is present (simd.hpp, Features::simd128).
    features.simd128 = true;
#endif
    return features;
}
#endif

Level level_of(const Features& features) noexcept {
#if defined(STXLIB_SIMD_X86_64)
    if (features.avx2 && features.fma) return Level::avx2_fma;
    if (features.sse2) return Level::vector128;
#else
    if (features.neon || features.simd128) return Level::vector128;
#endif
    return Level::scalar;
}

// The selection state, one atomic word so that a request and the selection
// cannot disagree: `unset`, a request (requested_offset + level) or the
// selected level itself (0 to 2). A request can replace a request; nothing
// replaces a selection.
constexpr i32 unset = state::simd_unset;
constexpr i32 requested_offset = 16;

bool is_selected(const i32 value) noexcept {
    const bool result = value >= static_cast<i32>(Level::scalar) && value <= static_cast<i32>(Level::avx2_fma);
    return result;
}

bool is_requested(const i32 value) noexcept {
    const bool result = value >= requested_offset + static_cast<i32>(Level::scalar) &&
                        value <= requested_offset + static_cast<i32>(Level::avx2_fma);
    return result;
}

// STX_SIMD_LEVEL, read through STXlib's environment service, which
// serializes with set_variable (the host's getenv does not). The limit
// refuses any longer value before it is copied.
i32 level_from_environment() noexcept {
    constexpr usize value_limit = 15;
    const OperationResult<Utf8String> value = read_variable("STX_SIMD_LEVEL", value_limit);
    if (value.failed()) return unset;
    const TextView text = (*value.value()).view();
    if (text == TextView("scalar")) return static_cast<i32>(Level::scalar);
    if (text == TextView("vector128")) return static_cast<i32>(Level::vector128);
    if (text == TextView("avx2_fma")) return static_cast<i32>(Level::avx2_fma);
    return unset;
}

} // namespace

Features detected_features() noexcept {
    const Features result = detect();
    return result;
}

Level hardware_level() noexcept {
    const Level result = level_of(detect());
    return result;
}

Level selected_level() noexcept {
    std::atomic<i32>& selection = state::current_process().simd_selection;
    i32 current = selection.load(std::memory_order_acquire);
    // Loops only when another thread changed the state in between: a newer
    // request is honoured, a selection is adopted.
    while (!is_selected(current)) {
        const i32 hardware = static_cast<i32>(hardware_level());
        const i32 wanted = is_requested(current) ? current - requested_offset : level_from_environment();
        const i32 chosen = (wanted == unset || wanted > hardware) ? hardware : wanted;
        if (selection.compare_exchange_weak(current, chosen, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return static_cast<Level>(chosen);
        }
    }
    return static_cast<Level>(current);
}

bool request_level(const Level level) noexcept {
    const i32 value = static_cast<i32>(level);
    if (!is_selected(value)) return false; // not a Level enumerator
    std::atomic<i32>& selection = state::current_process().simd_selection;
    i32 current = selection.load(std::memory_order_acquire);
    while (!is_selected(current)) {
        if (selection.compare_exchange_weak(current, requested_offset + value, std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

bool fma128_at(const Level level) noexcept {
    if (level == Level::scalar) return false;
#if defined(STXLIB_SIMD_X86_64)
    const bool result = detect().fma;
    return result;
#elif defined(__aarch64__) || defined(_M_ARM64)
    return true;
#else
    return false;
#endif
}

TextView level_name(const Level level) noexcept {
    switch (level) {
    case Level::scalar:
        return "scalar";
    case Level::vector128:
#if defined(__aarch64__) || defined(_M_ARM64)
        return "neon-128";
#elif defined(__wasm__)
        return "wasm-simd-128";
#else
        return "sse2-128";
#endif
    case Level::avx2_fma:
        return "avx2-fma-256";
    }
    return "scalar";
}

} // namespace stxlib::simd
