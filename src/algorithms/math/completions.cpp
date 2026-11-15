// exp2, expm1 and log1p (declared, with their contracts, in stxlib/math.hpp;
// roadmap R1). The cores are in completions_core.hpp. Each function is one
// pass per input region: a compare or two choose the region, and no region
// has a rounding test with an accurate fallback. Float results come from
// the same double-double as the double ones, rounded once to float.

#include "stxlib/math.hpp"

#include "completions_core.hpp"

#include <limits>

#include "variant_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

// exp2's fast path, 2^-54 <= |x| <= 1022: 2^exponent is normal (exponent
// in [-1022, 1022]) and the scaled result neither overflows nor falls below
// 2^-1022. Below 2^-54, 2^x rounds to 1 (and the core's smallest terms
// would leave the normal range for x below about 2^-970).
constexpr std::uint64_t exp2_tiny_bits = 0x3c90000000000000ULL;  // 2^-54
constexpr std::uint64_t exp2_fast_bits = 0x408ff00000000000ULL;  // 1022.0

// The rest of exp2(double): NaN, |x| < 2^-54 (1), infinities, overflow
// (2^1024 and above), results below 2^-1022 (scale_wide rounds them on the
// subnormal grid; flush.cpp's rule then returns +0) and zero below 2^-1075.
double exp2_outside(const double x) noexcept {
    if (x != x) return x + x;
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    if (magnitude < exp2_tiny_bits) return 1.0;
    if (x >= 1024.0) return std::numeric_limits<double>::infinity();
    if (x < -1075.0) return 0.0;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    region_exp2(x, 0.0, head, tail, exponent);
    const double result = scale_wide(head, tail, exponent);
    return result;
}

// The rest of exp2(float): NaN, x >= 128 (infinity) and x <= -150 (2^x <=
// 2^-150, half the smallest subnormal: +0, a tie at -150 going to even).
float exp2_float_outside(const float value) noexcept {
    const double x = widen(value);
    if (x != x) return value + value;
    if (x >= 128.0) return std::numeric_limits<float>::infinity();
    return 0.0f;
}

// expm1's regions (completions_core.hpp): one test each. Modular by
// design: tiny <= magnitude < series as one unsigned compare.
STXLIB_MODULAR bool expm1_series_region(const std::uint64_t magnitude) noexcept {
    const bool result = magnitude - expm1_tiny_bits < expm1_series_bits - expm1_tiny_bits;
    return result;
}

bool expm1_exp_region(const double x, const std::uint64_t magnitude, const double high) noexcept {
    const bool result = magnitude >= expm1_series_bits && x <= high && x >= expm1_minus_one;
    return result;
}

// The rest of expm1(double): NaN, |x| < 2^-54 (x itself, sign of zero kept),
// x < -38 (-1), and x > 45 (e^x, overflowing above ln(DBL_MAX)).
double expm1_outside(const double x) noexcept {
    if (x != x) return x + x;
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    if (magnitude < expm1_tiny_bits) return x;
    if (x < expm1_minus_one) return -1.0;
    if (x > 709.79) return std::numeric_limits<double>::infinity();
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_parts(x, head, tail, exponent);
    const double result = scale_wide(head, tail, exponent);
    return result;
}

// Float expm1 takes the exp region up to 89 (above ln(FLT_MAX) = 88.72 the
// rounding gives infinity), where 2^exponent is still far inside double.
constexpr double expm1_float_high = 89.0;

float expm1_float_outside(const float value) noexcept {
    const double x = widen(value);
    if (x != x) return value + value;
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    if (magnitude < expm1_tiny_bits) return value;
    if (x < expm1_minus_one) return -1.0f;
    return std::numeric_limits<float>::infinity();
}

bool log1p_fast_path(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    const bool result = magnitude >= log1p_tiny_bits && x > -1.0 && x < std::numeric_limits<double>::infinity();
    return result;
}

// The rest of log1p: NaN, |x| < 2^-54 (x itself), -1 (-infinity), below -1
// (NaN) and +infinity.
double log1p_outside(const double x) noexcept {
    if (x != x) return x + x;
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    if (magnitude < log1p_tiny_bits) return x;
    if (x == -1.0) return -std::numeric_limits<double>::infinity();
    if (x < -1.0) return std::numeric_limits<double>::quiet_NaN();
    return x;  // +infinity
}

} // namespace
} // namespace detail

// 2^x for a double (contract in stxlib/math.hpp). Modular by design: the
// fast-path range as one unsigned compare.
STXLIB_MODULAR double exp2(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & detail::magnitude_mask;
    if (magnitude - detail::exp2_tiny_bits <= detail::exp2_fast_bits - detail::exp2_tiny_bits) {
        double head = 0.0;
        double tail = 0.0;
        std::int64_t exponent = 0;
        detail::region_exp2(x, 0.0, head, tail, exponent);
        const double y = head + tail;
        const double result = y * detail::power_of_two(exponent);
        return result;
    }
    const double result = detail::exp2_outside(x);
    return result;
}

// 2^x for a float: -150 < x < 128 gives exponent in [-150, 128], so both
// scalings are exact and one rounding to float finishes.
float exp2(const float value) noexcept {
    const double x = detail::widen(value);
    if (x > -150.0 && x < 128.0) {
        double head = 0.0;
        double tail = 0.0;
        std::int64_t exponent = 0;
        detail::region_exp2(x, 0.0, head, tail, exponent);
        const double scale = detail::power_of_two(exponent);
        const float result = detail::round_wide_to_float(head * scale, tail * scale);
        return result;
    }
    const float result = detail::exp2_float_outside(value);
    return result;
}

// e^x - 1 for a double (contract in stxlib/math.hpp).
double expm1(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & detail::magnitude_mask;
    double hi = 0.0;
    double lo = 0.0;
    if (detail::expm1_series_region(magnitude)) {
        detail::expm1_series(x, hi, lo);
        const double result = hi + lo;
        return result;
    }
    if (detail::expm1_exp_region(x, magnitude, detail::expm1_large)) {
        detail::expm1_exp(x, hi, lo);
        const double result = hi + lo;
        return result;
    }
    const double result = detail::expm1_outside(x);
    return result;
}

// e^x - 1 for a float.
float expm1(const float value) noexcept {
    const double x = detail::widen(value);
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & detail::magnitude_mask;
    double hi = 0.0;
    double lo = 0.0;
    if (detail::expm1_series_region(magnitude)) {
        detail::expm1_series(x, hi, lo);
        const float result = detail::round_wide_to_float(hi, lo);
        return result;
    }
    if (detail::expm1_exp_region(x, magnitude, detail::expm1_float_high)) {
        detail::expm1_exp(x, hi, lo);
        const float result = detail::round_wide_to_float(hi, lo);
        return result;
    }
    const float result = detail::expm1_float_outside(value);
    return result;
}

// log(1 + x) for a double (contract in stxlib/math.hpp).
double log1p(const double x) noexcept {
    if (detail::log1p_fast_path(x)) {
        double hi = 0.0;
        double lo = 0.0;
        detail::log1p_wide(x, hi, lo);
        const double result = hi + lo;
        return result;
    }
    const double result = detail::log1p_outside(x);
    return result;
}

// log(1 + x) for a float.
float log1p(const float value) noexcept {
    const double x = detail::widen(value);
    if (detail::log1p_fast_path(x)) {
        double hi = 0.0;
        double lo = 0.0;
        detail::log1p_wide(x, hi, lo);
        const float result = detail::round_wide_to_float(hi, lo);
        return result;
    }
    const float result = static_cast<float>(detail::log1p_outside(x));  // exact: x, +-infinity or NaN
    return result;
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
