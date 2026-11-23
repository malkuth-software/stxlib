// One SIMD tier of stxlib::conv's kernels. kernels_scalar.cpp, kernels_v128.cpp and
// kernels_avx2_fma.cpp define STXLIB_CONV_LEVEL (0 scalar, 1 SSE2 or NEON, 2 AVX2) and
// STXLIB_CONV_VARIANT (the namespace), then include this file, which compiles the kernels for
// that tier into stxlib::conv::detail::STXLIB_CONV_VARIANT and defines its table.
//
// The avx2_fma tier is compiled for that target through an attribute pushed after the last
// #include and popped at the end (stx_fft's variant.inc, the math's variant.hpp): no standard
// or STXlib header is inside it, so no shared inline code carries AVX instructions into a lower
// tier. The .inc files hold only this directory's kernel definitions, with internal linkage.
//
// Floating point: no fused multiply-add appears; the build has contraction off; min and max
// are compare-select (lanes.inc), so every tier rounds and selects as the scalar tier does.

#if !defined(STXLIB_CONV_LEVEL) || !defined(STXLIB_CONV_VARIANT)
#error "variant.inc needs STXLIB_CONV_LEVEL and STXLIB_CONV_VARIANT"
#endif

#include "kernels.hpp"

#include "stxlib/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#if STXLIB_CONV_LEVEL >= 1
#if defined(__x86_64__) || defined(_M_X64)
#define STXLIB_CONV_X86 1
#include <emmintrin.h>
#if STXLIB_CONV_LEVEL >= 2
#include <immintrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define STXLIB_CONV_NEON 1
#include <arm_neon.h>
#elif defined(__wasm_simd128__)
// WebAssembly SIMD128 (Clang defines __wasm_simd128__ under -msimd128): f64x2 arithmetic is
// IEEE 754 correctly rounded, with no fused multiply-add, so the lanes agree with the scalar
// tier bit for bit as NEON's and SSE2's do.
#define STXLIB_CONV_WASM 1
#include <wasm_simd128.h>
#else
#error "STXLIB_CONV_LEVEL >= 1 needs x86-64, arm64 or wasm SIMD128"
#endif
#endif

#if STXLIB_CONV_LEVEL >= 2
#if defined(__clang__)
#define STXLIB_CONV_TARGET_PUSH _Pragma("clang attribute push(__attribute__((target(\"avx2,fma\"))), apply_to = function)")
#define STXLIB_CONV_TARGET_POP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define STXLIB_CONV_TARGET_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"avx2,fma\")")
#define STXLIB_CONV_TARGET_POP _Pragma("GCC pop_options")
#else
#define STXLIB_CONV_TARGET_PUSH
#define STXLIB_CONV_TARGET_POP
#endif
#else
#define STXLIB_CONV_TARGET_PUSH
#define STXLIB_CONV_TARGET_POP
#endif

#if defined(__GNUC__) || defined(__clang__)
#define STXLIB_CONV_INLINE inline __attribute__((always_inline))
#else
#define STXLIB_CONV_INLINE __forceinline
#endif

STXLIB_CONV_TARGET_PUSH

namespace stxlib::conv::detail::STXLIB_CONV_VARIANT {

namespace {

constexpr std::size_t lanes_channels = 4U;

#include "lanes.inc"

#include "contest.inc"

#include "preview.inc"

#include "canon.inc"

} // namespace

const Kernels& kernels() noexcept {
    static constexpr Kernels table{pass_a, pass_b, row_cells, tile_windows, vertical_linear, vertical_admitted,
                               g_coefficients, node_windows, strip_q, q_at, flux_points, moments_contract};
    return table;
}

} // namespace stxlib::conv::detail::STXLIB_CONV_VARIANT

STXLIB_CONV_TARGET_POP

#undef STXLIB_CONV_TARGET_PUSH
#undef STXLIB_CONV_TARGET_POP
