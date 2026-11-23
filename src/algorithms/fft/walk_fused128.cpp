// The PSFT's fused build on 128-bit vectors (WalkTier::fused128): NEON on
// arm64, SSE with FMA3 on x86-64 (compiled for that target by walk_variant.inc's
// attribute, run only where simd::fma128_at holds). Empty elsewhere.

#include "plan_state.hpp"

#if defined(STXLIB_FFT_WALK_FUSED128)
#define STXLIB_FFT_WALK_TIER 2
#define STXLIB_FFT_WALK_VARIANT walk_fused128
#include "walk_variant.inc"
#endif
