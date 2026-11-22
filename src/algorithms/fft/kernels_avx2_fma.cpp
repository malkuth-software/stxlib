// stx_fft's avx2_fma tier (BFFT's BRUUN_LEVEL 2): AVX2 on x86-64, compiled
// for that target through variant.inc's attribute, run only when
// simd::selected_level() is avx2_fma. Empty elsewhere. The tier's name is
// simd.hpp's; the kernels use no fused multiply-add (variant.inc).

#include "plan_state.hpp"

#if defined(STXLIB_FFT_TIER_AVX2)
#define STXLIB_FFT_LEVEL 2
#define STXLIB_FFT_VARIANT avx2_fma
#include "variant.inc"
#endif
