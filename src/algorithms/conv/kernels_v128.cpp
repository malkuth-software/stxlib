// stxlib::conv's vector128 tier: SSE2 on x86-64, NEON on arm64, SIMD128 on wasm (built with
// -msimd128). Empty elsewhere.

#include "kernels.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__) || defined(_M_ARM64) || defined(__wasm_simd128__)
#define STXLIB_CONV_LEVEL 1
#define STXLIB_CONV_VARIANT v128
#include "variant.inc"
#endif
