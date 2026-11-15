// sin and cos (declared, with their contracts, in stxlib/math.hpp). The
// core is in sincos_core.hpp.

#include "stxlib/math.hpp"

#include "sincos_core.hpp"

#include <bit>
#include <cstdint>
#include <limits>

#include "variant_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

// The rounded remainder r and table index for the float kernel, for
// 2^-27 <= |x| <= 2^32.
double float_remainder(const double x, std::uint64_t& index) noexcept {
    const double q = sincos_step(x, index);
    if (sincos_fast_path(x)) {
        const double r = sincos_remainder(x, q);
        return r;
    }
    double r = 0.0;
    double r_lo = 0.0;
    sincos_remainder_medium(x, q, r, r_lo);
    return r;
}

// Floats at which the double kernel below cannot round correctly: sin or
// cos lies closer to a float midpoint than the kernel's error (2^-52.65
// absolute), on the far side from the kernel's double. The proof
// (tools/math/sincos_float_screen.py) checks every float 2^-27 <= |x| <=
// 2^32 and finds exactly these. Each entry holds |x|'s bits and the
// correctly rounded sin and cos of |x| (sin(-x) = -sin x, cos(-x) = cos x).
struct FloatSinCosException final {
    std::uint32_t magnitude_bits;
    float s;
    float c;
};

inline constexpr FloatSinCosException float_sincos_exceptions[] = {
    // |x| = 9830.3984375: sin x lies 2^-54 (relative) past the midpoint
    // -0x1.63f4bbp-2, which the kernel's double equals exactly.
    {0x46199998u, -0x1.63f4bap-2f, -0x1.e01216p-1f},
};

// Replaces s and c with the stored values at an exception; a compare per
// entry, no branch the kernel's chain waits for.
void float_sincos_exception(const float angle, float& s, float& c) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(angle);
    const std::uint32_t magnitude = bits & 0x7fffffffu;
    for (const FloatSinCosException& entry : float_sincos_exceptions) {
        if (magnitude == entry.magnitude_bits) {
            s = (bits >> 31) != 0 ? -entry.s : entry.s;
            c = entry.c;
        }
    }
}

} // namespace
} // namespace detail

// sin and cos of a float (contract in stxlib/math.hpp). Double
// intermediate within 2^-52.65 absolute (tools/math/sincos_float_screen.py
// step 1), rounded once to float; correctly rounded at every float but the
// exceptions above, which return their stored values.
void sincos(const float angle, float& s, float& c) noexcept {
    const double x = detail::widen(angle);
    if (!detail::sincos_fast_path(x) && !detail::sincos_medium_path(x)) {
        if (std::fabs(x) < detail::sincos_tiny_limit) {
            s = angle;
            c = 1.0f;
        } else {
            // |x| > 2^32, infinities and NaN: refused (NaN propagates).
            const float refused = x != x ? angle + angle : std::numeric_limits<float>::quiet_NaN();
            s = refused;
            c = refused;
        }
        return;
    }
    std::uint64_t index = 0;
    const double r = detail::float_remainder(x, index);
    const double r2 = r * r;
    const double sin_r = r + r * (r2 * (detail::sin_3 + r2 * detail::sin_5));
    const double cos_r_minus_1 =
        (r2 * r2) * (detail::cos_4 + r2 * detail::cos_6) - r2 * detail::cos_half;
    const detail::SinCosEntry& entry = detail::sincos_table_256[index];
    const double sin_correction = entry.s * cos_r_minus_1 + entry.c * sin_r;
    const double cos_correction = entry.c * cos_r_minus_1 - entry.s * sin_r;
    s = static_cast<float>(entry.s + sin_correction);
    c = static_cast<float>(entry.c + cos_correction);
    detail::float_sincos_exception(angle, s, c);
}

// sin and cos of a double (contract in stxlib/math.hpp).
void sincos(const double x, double& s, double& c) noexcept {
    double sin_head = 0.0;
    double sin_tail = 0.0;
    double cos_head = 0.0;
    double cos_tail = 0.0;
    if (detail::sincos_fast_path(x)) {
        detail::sincos_wide(x, sin_head, sin_tail, cos_head, cos_tail);
    } else if (detail::sincos_medium_path(x)) {
        detail::sincos_wide_medium(x, sin_head, sin_tail, cos_head, cos_tail);
    } else if (std::fabs(x) < detail::sincos_tiny_limit) {
        s = x;
        c = 1.0;
        return;
    } else {
        // |x| > 2^32, infinities and NaN: refused (NaN propagates).
        const double refused = x != x ? x + x : std::numeric_limits<double>::quiet_NaN();
        s = refused;
        c = refused;
        return;
    }
    s = sin_head + sin_tail;
    c = cos_head + cos_tail;
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
