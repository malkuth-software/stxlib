// stx_fft's vector128 tier (BFFT's BRUUN_LEVEL 1): SSE2 on x86-64, NEON on
// arm64, both the architecture's baseline. Empty elsewhere.

#include "plan_state.hpp"

#if defined(STXLIB_FFT_TIER_V128)
#define STXLIB_FFT_LEVEL 1
#define STXLIB_FFT_VARIANT v128
#include "variant.inc"
#endif
