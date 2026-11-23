// The fused measuring variant of stx_fft's kernels, for stxlib_fft_bench
// only (never the library, never a plan made by make): variant.inc compiled
// with STXLIB_FFT_FUSED, where lanes.inc fuses each vector multiply into
// the add or subtract that takes it, the multiply-adds BFFT itself spells.
// The benchmark swaps it into a plan (PlanAccess::use_kernels) and reports
// the time it saves and how far its bins move from the unfused tier's.
//
// arm64: the NEON vector128 tier (v128_fused). x86-64: the avx2_fma tier
// (avx2_fused), run only where the machine has AVX2 and FMA. Elsewhere
// this file is empty.

#include "algorithms/fft/plan_state.hpp"

#if defined(__aarch64__) || defined(_M_ARM64)
namespace stxlib::fft::detail::v128_fused {
extern const Kernels kernels;
} // namespace stxlib::fft::detail::v128_fused
#define STXLIB_FFT_LEVEL 1
#define STXLIB_FFT_VARIANT v128_fused
#define STXLIB_FFT_FUSED 1
#include "algorithms/fft/variant.inc"
#elif defined(__x86_64__) || defined(_M_X64)
namespace stxlib::fft::detail::avx2_fused {
extern const Kernels kernels;
} // namespace stxlib::fft::detail::avx2_fused
#define STXLIB_FFT_LEVEL 2
#define STXLIB_FFT_VARIANT avx2_fused
#define STXLIB_FFT_FUSED 1
#include "algorithms/fft/variant.inc"
#endif
