#pragma once

// The kernel variant a translation unit of src/algorithms/math/ compiles. Every kernel
// source is compiled once per variant the build offers, each into its own
// namespace stxlib::math::STXLIB_MATH_VARIANT, and dispatch.cpp
// calls the variant the process's SIMD level selects (stxlib/simd.hpp).
//
//   v128      x86-64 baseline (SSE2): Dekker's exact product, legacy SSE
//             encoding. Selected at the scalar and vector128 levels, since
//             SSE2 is the scalar floating point of x86-64.
//   avx2_fma  x86-64 AVX2 + FMA3: the fused exact product and VEX encoding.
//             Its code is compiled for that target through the attribute the
//             STXLIB_MATH_TARGET_PUSH / _POP pair wraps around each file's
//             own definitions (never around an #include), so the standard
//             library's inline functions it calls are still emitted for the
//             baseline: no out-of-line copy of shared code can carry AVX
//             instructions into the v128 path.
//   native    every other build (arm64, where FMA is baseline; the Dekker
//             test build): one variant, called directly.
//
// Every variant returns the same bits: two_product is exact in both forms,
// and multiply_add serves only certified paths (kernel_support.hpp).
//
// The build defines STXLIB_MATH_VARIANT, and STXLIB_MATH_AVX2_FMA for
// the avx2_fma variant.

#if !defined(STXLIB_MATH_VARIANT)
#error "src/algorithms/math/ compiles only with STXLIB_MATH_VARIANT defined (CMakeLists.txt)"
#endif

#if defined(STXLIB_MATH_AVX2_FMA)
#if !defined(STXLIB_MATH_DEKKER)
#define STXLIB_MATH_FMA 1
#endif
#define STXLIB_MATH_VEX 1
#if defined(__clang__)
#define STXLIB_MATH_TARGET_PUSH \
    _Pragma("clang attribute push(__attribute__((target(\"avx2,fma\"))), apply_to = function)")
#define STXLIB_MATH_TARGET_POP _Pragma("clang attribute pop")
#elif defined(__GNUC__)
#define STXLIB_MATH_TARGET_PUSH _Pragma("GCC push_options") _Pragma("GCC target(\"avx2,fma\")")
#define STXLIB_MATH_TARGET_POP _Pragma("GCC pop_options")
#else
#error "the avx2_fma variant needs Clang or GCC"
#endif
#else
#if !defined(STXLIB_MATH_DEKKER) && \
    (defined(__FP_FAST_FMA) || defined(__ARM_FEATURE_FMA) || defined(__FMA__))
#define STXLIB_MATH_FMA 1
#endif
#if defined(__AVX__)
#define STXLIB_MATH_VEX 1
#endif
#define STXLIB_MATH_TARGET_PUSH
#define STXLIB_MATH_TARGET_POP
#endif
