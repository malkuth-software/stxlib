// stxlib::conv's avx2_fma tier: AVX2 on x86-64, compiled for that target through
// variant.inc's attribute and run only when simd::selected_level() is avx2_fma. The tier's name
// is simd.hpp's; no fused multiply-add is used. Empty elsewhere.

#include "kernels.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#define STXLIB_CONV_LEVEL 2
#define STXLIB_CONV_VARIANT avx2_fma
#include "variant.inc"
#endif
