// The kernel table for the process's level (kernels.hpp).

#include "kernels.hpp"

namespace stxlib::conv::detail {

const Kernels& kernels_for(const simd::Level level) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    if (level == simd::Level::avx2_fma) return avx2_fma::kernels();
    if (level == simd::Level::vector128) return v128::kernels();
#elif defined(__aarch64__) || defined(_M_ARM64)
    if (level == simd::Level::vector128 || level == simd::Level::avx2_fma) return v128::kernels();
#endif
    return scalar::kernels();
}

} // namespace stxlib::conv::detail
