// One SIMD tier of stx_fft's kernels. kernels_scalar.cpp, kernels_v128.cpp
// and kernels_avx2_fma.cpp each define STXLIB_FFT_LEVEL (BFFT's BRUUN_LEVEL:
// 0 scalar, 1 SSE2 or NEON, 2 AVX2) and STXLIB_FFT_VARIANT (the namespace),
// then include this file, which compiles the four kernels for that tier
// (DIF, DIT, BODFT, DIP) into namespace
// stxlib::fft::detail::STXLIB_FFT_VARIANT and defines its Kernels table
// (plan_state.hpp).
//
// The avx2_fma tier's code is compiled for that target through an attribute
// pushed after the last #include of a header and popped at the end, as the
// math's variant.hpp does: no standard or STXlib header is inside it, so no
// out-of-line copy of shared inline code can carry AVX instructions into a
// lower tier. The .inc files included inside it hold only this directory's
// kernel definitions, all with internal linkage.
//
// Floating point: no fused multiply-add appears anywhere here (the
// transform described in dif.inc spelled BFFT's out), and the build has
// contraction off, so every tier rounds each output as the scalar tier does.
// The one exception is never a plan's: stxlib_fft_bench compiles this file
// a fourth time with STXLIB_FFT_FUSED (tests/fft_fused_kernels.cpp), where
// lanes.inc fuses each vector multiply into the add or subtract that takes
// it, to measure what the rule costs.

#if !defined(STXLIB_FFT_LEVEL) || !defined(STXLIB_FFT_VARIANT)
#error "variant.inc needs STXLIB_FFT_LEVEL and STXLIB_FFT_VARIANT"
#endif

#include "plan_state.hpp"

#include "stxlib/types.hpp"

#include <bit>
#include <cstddef>

#if STXLIB_FFT_LEVEL >= 1
#if defined(__x86_64__) || defined(_M_X64)
#define STXLIB_FFT_X86 1
#include <emmintrin.h>
#if STXLIB_FFT_LEVEL >= 2
#include <immintrin.h>
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
#define STXLIB_FFT_NEON 1
#include <arm_neon.h>
#else
#error "STXLIB_FFT_LEVEL >= 1 needs x86-64 or arm64"
#endif
#endif

#if STXLIB_FFT_LEVEL >= 2
#if defined(__clang__)
#define STXLIB_FFT_TARGET_PUSH \
    _Pragma("clang attribute push(__attribute__((target(\"avx2,fma\"))), apply_to = function)")
#define STXLIB_FFT_TARGET_POP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define STXLIB_FFT_TARGET_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"avx2,fma\")")
#define STXLIB_FFT_TARGET_POP _Pragma("GCC pop_options")
#else
// MSVC compiles AVX2 intrinsics in any function.
#define STXLIB_FFT_TARGET_PUSH
#define STXLIB_FFT_TARGET_POP
#endif
#else
#define STXLIB_FFT_TARGET_PUSH
#define STXLIB_FFT_TARGET_POP
#endif

// BFFT's BRUUN_ALWAYS_INLINE.
#if defined(__GNUC__) || defined(__clang__)
#define STXLIB_FFT_INLINE inline __attribute__((always_inline))
#else
#define STXLIB_FFT_INLINE __forceinline
#endif

STXLIB_FFT_TARGET_PUSH

namespace stxlib::fft::detail::STXLIB_FFT_VARIANT {

namespace {

// BFFT's ilog2_pow2 and bfft_ctz_u32 (n and x are powers of two here).
inline int ilog2_pow2(int n) noexcept { return std::countr_zero(static_cast<unsigned>(n)); }
inline unsigned ctz_u32(unsigned x) noexcept { return static_cast<unsigned>(std::countr_zero(x)); }

#include "lanes.inc"

#include "dif.inc"

#include "bodft.inc"

#include "dip.inc"

#include "dit.inc"

void dif_forward64(const DifTables& tables, const double* input, Complex64* output, double* work,
                   Complex64* native) noexcept {
    const DifKernel kernel(tables);
    kernel.forward_standard(input, output, work, native);
}

void dif_inverse64(const DifTables& tables, const Complex64* input, double* output) noexcept {
    const DifKernel kernel(tables);
    kernel.inverse(input, output);
}

void dif_forward32(const DifTables& tables, const float* input, Complex32* output, float* work) noexcept {
    const DifKernel kernel(tables);
    kernel.forward_standard_f32(input, output, work);
}

void dif_inverse32(const DifTables& tables, const Complex32* input, float* output) noexcept {
    const DifKernel kernel(tables);
    kernel.inverse_f32(input, output);
}

using Bodft64 = BodftKernel<Complex64, double, BodftLevel64>;
using Bodft32 = BodftKernel<Complex32, float, BodftLevel32>;

void bodft_forward64(const BodftTables& tables, const double* input, Complex64* output) noexcept {
    const Bodft64 kernel(tables, tables.level64, tables.root4, tables.scratch64);
    kernel.forward(input, output);
}

void bodft_inverse64(const BodftTables& tables, const Complex64* input, double* output) noexcept {
    const Bodft64 kernel(tables, tables.level64, tables.root4, tables.scratch64);
    kernel.inverse(input, output);
}

void bodft_forward32(const BodftTables& tables, const float* input, Complex32* output) noexcept {
    const Bodft32 kernel(tables, tables.level32, tables.root4f, tables.scratch32);
    kernel.forward(input, output);
}

void bodft_inverse32(const BodftTables& tables, const Complex32* input, float* output) noexcept {
    const Bodft32 kernel(tables, tables.level32, tables.root4f, tables.scratch32);
    kernel.inverse(input, output);
}

void dip_forward64(const DipTables& tables, const double* input, Complex64* output) noexcept {
    const DipKernel kernel(tables);
    kernel.forward_standard(input, output, tables.work);
}

void dip_inverse64(const DipTables& tables, const Complex64* input, double* output) noexcept {
    const DipKernel kernel(tables);
    kernel.inverse_standard(input, output, tables.work);
}

void dit_forward64(const DitTables& tables, const double* input, Complex64* output, double* work) noexcept {
    const DitKernel kernel(tables);
    kernel.forward64(input, output, work);
}

void dit_inverse64(const DitTables& tables, const Complex64* input, double* output, double* work) noexcept {
    const DitKernel kernel(tables);
    kernel.inverse64(input, output, work);
}

void dit_forward32(const DitTables& tables, const float* input, Complex32* output, float* work) noexcept {
    const DitKernel kernel(tables);
    kernel.forward32(input, output, work);
}

void dit_inverse32(const DitTables& tables, const Complex32* input, float* output, float* work) noexcept {
    const DitKernel kernel(tables);
    kernel.inverse32(input, output, work);
}

} // namespace

const Kernels kernels = {
    .dif_forward64 = &dif_forward64,
    .dif_inverse64 = &dif_inverse64,
    .dif_forward32 = &dif_forward32,
    .dif_inverse32 = &dif_inverse32,
    .bodft_forward64 = &bodft_forward64,
    .bodft_inverse64 = &bodft_inverse64,
    .bodft_forward32 = &bodft_forward32,
    .bodft_inverse32 = &bodft_inverse32,
    .dip_forward64 = &dip_forward64,
    .dip_inverse64 = &dip_inverse64,
    .dit_forward64 = &dit_forward64,
    .dit_inverse64 = &dit_inverse64,
    .dit_forward32 = &dit_forward32,
    .dit_inverse32 = &dit_inverse32,
};

} // namespace stxlib::fft::detail::STXLIB_FFT_VARIANT

STXLIB_FFT_TARGET_POP
