// One tier of the walk's kernels. walk_portable.cpp, walk_exact128.cpp and
// walk_fused128.cpp each define STXLIB_FFT_WALK_TIER (0 exact_portable, 1
// exact128, 2 fused128; plan_state.hpp, WalkTier) and STXLIB_FFT_WALK_VARIANT
// (the namespace), then include this file, which compiles the walk
// (walk.inc), the straight-line kernels (walk_small_f64.inc,
// walk_small_f32.inc) and the tier's policies (walk_lanes.inc) into
// stxlib::fft::detail::STXLIB_FFT_WALK_VARIANT and defines its WalkKernels.
//
// x86-64's fused128 code is compiled for SSE with FMA3 through an attribute
// ("fma") pushed after the last #include and popped at the end, as
// variant.inc does for avx2_fma, so no shared inline code carries those
// instructions into another tier. It is run only where simd::fma128_at says
// the machine has FMA3. No tier uses vectors wider than 128 bits.

#if !defined(STXLIB_FFT_WALK_TIER) || !defined(STXLIB_FFT_WALK_VARIANT)
#error "walk_variant.inc needs STXLIB_FFT_WALK_TIER and STXLIB_FFT_WALK_VARIANT"
#endif

#include "plan_state.hpp"

#include "stxlib/types.hpp"

#include <cmath>

#if STXLIB_FFT_WALK_TIER >= 1
#if defined(__x86_64__) || defined(_M_X64)
#define STXLIB_FFT_WALK_SSE 1
#include <emmintrin.h>
#if STXLIB_FFT_WALK_TIER == 2
#include <immintrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define STXLIB_FFT_WALK_NEON 1
#include <arm_neon.h>
#elif defined(__wasm_simd128__) && STXLIB_FFT_WALK_TIER == 1
#define STXLIB_FFT_WALK_WASM 1
#include <wasm_simd128.h>
#else
#error "walk_variant.inc: this tier needs x86-64, arm64 or wasm SIMD128 (exact128 only)"
#endif
#endif

#if STXLIB_FFT_WALK_TIER == 2 && defined(STXLIB_FFT_WALK_SSE)
#if defined(__clang__)
#define STXLIB_FFT_WALK_TARGET_PUSH \
    _Pragma("clang attribute push(__attribute__((target(\"fma\"))), apply_to = function)")
#define STXLIB_FFT_WALK_TARGET_POP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define STXLIB_FFT_WALK_TARGET_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"fma\")")
#define STXLIB_FFT_WALK_TARGET_POP _Pragma("GCC pop_options")
#else
#define STXLIB_FFT_WALK_TARGET_PUSH
#define STXLIB_FFT_WALK_TARGET_POP
#endif
#else
#define STXLIB_FFT_WALK_TARGET_PUSH
#define STXLIB_FFT_WALK_TARGET_POP
#endif

STXLIB_FFT_WALK_TARGET_PUSH

namespace stxlib::fft::detail::STXLIB_FFT_WALK_VARIANT {

namespace {

#include "walk_lanes.inc"

#include "walk.inc"

#include "walk_small_f64.inc"

#include "walk_small_f32.inc"

// The straight-line kernels by size (the plan sets straight_line only for
// the sizes they cover: 16 .. 128 in float64, 16 .. 256 in float32).
void small_forward64(const WalkTables& tables, const double* const x, double* const X) noexcept {
    using P = WalkPolicies<double>::S;
    const double* const C = tables.twiddles64;
    switch (tables.n) {
    case 16:
        small_fwd_f64_16<P>(x, X, C);
        return;
    case 32:
        small_fwd_f64_32<P>(x, X, C);
        return;
    case 64:
        small_fwd_f64_64<P>(x, X, C);
        return;
    default:
        small_fwd_f64_128<P>(x, X, C);
        return;
    }
}

void small_magnitude64(const WalkTables& tables, const double* const x, double* const m) noexcept {
    using P = WalkPolicies<double>::S;
    const double* const C = tables.twiddles64;
    switch (tables.n) {
    case 16:
        small_mag_f64_16<P>(x, m, C);
        return;
    case 32:
        small_mag_f64_32<P>(x, m, C);
        return;
    case 64:
        small_mag_f64_64<P>(x, m, C);
        return;
    default:
        small_mag_f64_128<P>(x, m, C);
        return;
    }
}

void small_inverse64(const WalkTables& tables, const double* const X, double* const y) noexcept {
    using P = WalkPolicies<double>::S;
    const double* const C = tables.twiddles64;
    switch (tables.n) {
    case 16:
        small_inv_f64_16<P>(X, y, C);
        return;
    case 32:
        small_inv_f64_32<P>(X, y, C);
        return;
    case 64:
        small_inv_f64_64<P>(X, y, C);
        return;
    default:
        small_inv_f64_128<P>(X, y, C);
        return;
    }
}

void small_forward32(const WalkTables& tables, const float* const x, float* const X) noexcept {
    using P = WalkPolicies<float>::S;
    const float* const C = tables.twiddles32;
    switch (tables.n) {
    case 16:
        small_fwd_f32_16<P>(x, X, C);
        return;
    case 32:
        small_fwd_f32_32<P>(x, X, C);
        return;
    case 64:
        small_fwd_f32_64<P>(x, X, C);
        return;
    case 128:
        small_fwd_f32_128<P>(x, X, C);
        return;
    default:
        small_fwd_f32_256<P>(x, X, C);
        return;
    }
}

void small_magnitude32(const WalkTables& tables, const float* const x, float* const m) noexcept {
    using P = WalkPolicies<float>::S;
    const float* const C = tables.twiddles32;
    switch (tables.n) {
    case 16:
        small_mag_f32_16<P>(x, m, C);
        return;
    case 32:
        small_mag_f32_32<P>(x, m, C);
        return;
    case 64:
        small_mag_f32_64<P>(x, m, C);
        return;
    case 128:
        small_mag_f32_128<P>(x, m, C);
        return;
    default:
        small_mag_f32_256<P>(x, m, C);
        return;
    }
}

void small_inverse32(const WalkTables& tables, const float* const X, float* const y) noexcept {
    using P = WalkPolicies<float>::S;
    const float* const C = tables.twiddles32;
    switch (tables.n) {
    case 16:
        small_inv_f32_16<P>(X, y, C);
        return;
    case 32:
        small_inv_f32_32<P>(X, y, C);
        return;
    case 64:
        small_inv_f32_64<P>(X, y, C);
        return;
    case 128:
        small_inv_f32_128<P>(X, y, C);
        return;
    default:
        small_inv_f32_256<P>(X, y, C);
        return;
    }
}

// Complex64 and Complex32 are (re, im) with no padding (fft.hpp), so a
// spectrum is 2 (N/2 + 1) reals in a row.
static_assert(sizeof(Complex64) == 2 * sizeof(double), "Complex64 is two doubles");
static_assert(sizeof(Complex32) == 2 * sizeof(float), "Complex32 is two floats");

void forward64(const WalkTables& tables, const double* const x, Complex64* const X, double* const work) noexcept {
    double* const out = reinterpret_cast<double*>(X);
    if (tables.straight_line) {
        small_forward64(tables, x, out);
        return;
    }
    walk_forward<double>(tables, x, out, work);
}

void inverse64(const WalkTables& tables, const Complex64* const X, double* const y, double* const work) noexcept {
    const double* const in = reinterpret_cast<const double*>(X);
    if (tables.straight_line) {
        small_inverse64(tables, in, y);
        return;
    }
    walk_inverse<double>(tables, in, y, work);
}

void magnitude64(const WalkTables& tables, const double* const x, double* const m, double* const work) noexcept {
    if (tables.straight_line) {
        small_magnitude64(tables, x, m);
        return;
    }
    walk_magnitude<double>(tables, x, m, work);
}

void forward32(const WalkTables& tables, const float* const x, Complex32* const X, float* const work) noexcept {
    float* const out = reinterpret_cast<float*>(X);
    if (tables.straight_line) {
        small_forward32(tables, x, out);
        return;
    }
    walk_forward<float>(tables, x, out, work);
}

void inverse32(const WalkTables& tables, const Complex32* const X, float* const y, float* const work) noexcept {
    const float* const in = reinterpret_cast<const float*>(X);
    if (tables.straight_line) {
        small_inverse32(tables, in, y);
        return;
    }
    walk_inverse<float>(tables, in, y, work);
}

void magnitude32(const WalkTables& tables, const float* const x, float* const m, float* const work) noexcept {
    if (tables.straight_line) {
        small_magnitude32(tables, x, m);
        return;
    }
    walk_magnitude<float>(tables, x, m, work);
}

} // namespace

const WalkKernels kernels = {
    .forward64 = &forward64,
    .inverse64 = &inverse64,
    .magnitude64 = &magnitude64,
    .forward32 = &forward32,
    .inverse32 = &inverse32,
    .magnitude32 = &magnitude32,
};

} // namespace stxlib::fft::detail::STXLIB_FFT_WALK_VARIANT

STXLIB_FFT_WALK_TARGET_POP
