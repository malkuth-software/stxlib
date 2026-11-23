// The PSFT's exact build on plain lanes (WalkTier::exact_portable): compiled
// on every target; the scalar tier's walk, and wasm's without SIMD128.

#define STXLIB_FFT_WALK_TIER 0
#define STXLIB_FFT_WALK_VARIANT walk_portable
#include "walk_variant.inc"
