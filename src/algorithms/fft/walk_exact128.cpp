// The PSFT's exact build on 128-bit vectors (WalkTier::exact128): SSE2 on
// x86-64 (the tier of a CPU without FMA3), SIMD128 on wasm, and NEON on arm64
// (reached only through PlanAccess, for tests). Empty elsewhere.

#include "plan_state.hpp"

#if defined(STXLIB_FFT_WALK_EXACT128)
#define STXLIB_FFT_WALK_TIER 1
#define STXLIB_FFT_WALK_VARIANT walk_exact128
#include "walk_variant.inc"
#endif
