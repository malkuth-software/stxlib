// log, log2 and log10 (declared, with their contracts, in
// stxlib/math.hpp). The core is in log_core.hpp.

#include "stxlib/math.hpp"

#include "log_core.hpp"

#include <limits>

#include "variant_functions.hpp"

#include "paths.hpp"
#include "region_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

// The logarithm of every double the fast path rejects. The base is a
// template parameter so each public function compiles with its constants
// folded (a run-time base cost ~2 ns a call).
template <const LogBase& base>
double log_outside(const double x) noexcept {
    if (x != x) return x + x;
    if (x == 0.0) return -std::numeric_limits<double>::infinity();
    if (x < 0.0) return std::numeric_limits<double>::quiet_NaN();
    if (x == std::numeric_limits<double>::infinity()) return x;
    // Positive subnormal: scale into the normal range exactly.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x * two_to_52);
    double k = 0.0;
    double head = 0.0;
    double tail = 0.0;
    log_z(bits, 52.0, k, head, tail);
    const double result = log_combine(base, k, head, tail);
    return result;
}

template <const LogBase& base>
double log_any(const double x) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    if (!log_fast_path(bits)) return log_outside<base>(x);
    double k = 0.0;
    double head = 0.0;
    double tail = 0.0;
    log_z(bits, 0.0, k, head, tail);
    const double result = log_combine(base, k, head, tail);
    return result;
}

// Float inputs are normal doubles (or zero, negative, infinite, NaN): the
// cheap path when its rounding is certain, otherwise the accurate path.
//
// The accurate float logarithm, out of line for the fast path's tail call.
// A positive normal finite x takes the region kernel (region.cpp: a
// double-double within 2^-62, rounded once; correctly rounded for every
// positive normal float, proved by tools/math/region_exhaustive.py). Until
// R3 this path rounded the double result to float, a second rounding: it
// misrounded log at the five floats whose logarithm lies so near a float
// midpoint that the double result rounded onto it or past it (0x1.827a74p-7, 0x1.2f1fd6p+3, 0x1.bacb4ap+25,
// 0x1.b121a6p+76, 0x1.6351d8p+95; found by the hardened screens and the
// region comparison on the M4), and modelled to misround log10 at
// 0x1.fddcf4p-98 (tools/math/region_exhaustive.py --double-rounding; log2:
// none). Zero, negative, infinite, NaN and subnormal
// x keep the double result rounded once: exact or special there, except a
// subnormal x (read unflushed only by the variant function; the public
// function flushes it to -infinity).
template <const LogBase& base>
float region_log_float(const float x) noexcept {
    float result = 0.0f;
    if constexpr (&base == &natural) {
        result = region::log(x);
    } else if constexpr (&base == &binary) {
        result = region::log2(x);
    } else {
        result = region::log10(x);
    }
    return result;
}

template <const LogBase& base>
STXLIB_MATH_COLD float log_float_accurate(const float x) noexcept {
    if (x >= 0x1p-126f && x <= std::numeric_limits<float>::max()) {
        const float result = region_log_float<base>(x);
        return result;
    }
    const double result = log_any<base>(detail::widen(x));
    return static_cast<float>(result);
}

template <const LogBase& base>
float log_any(const float x) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(detail::widen(x));
    if (log_fast_path(bits)) {
        double value = 0.0;
        double error = 0.0;
        log_cheap(base, bits, value, error);
        float fast = 0.0f;
        if (float_if_certain(value, error, fast)) return fast;
    }
    const float result = log_float_accurate<base>(x);
    return result;
}

} // namespace
} // namespace detail

double log(const double x) noexcept {
    const double result = detail::log_any<detail::natural>(x);
    return result;
}

double log2(const double x) noexcept {
    const double result = detail::log_any<detail::binary>(x);
    return result;
}

double log10(const double x) noexcept {
    const double result = detail::log_any<detail::decimal>(x);
    return result;
}

namespace today {

float log(const float x) noexcept {
    const float result = detail::log_any<detail::natural>(x);
    return result;
}

float log2(const float x) noexcept {
    const float result = detail::log_any<detail::binary>(x);
    return result;
}

float log10(const float x) noexcept {
    const float result = detail::log_any<detail::decimal>(x);
    return result;
}

} // namespace today

// The float variant functions: the path STXLIB_MATH_REGION_LOG_FLOAT
// chooses (paths.hpp). A subnormal x takes today's kernel, which reads it
// unflushed; no logarithm result is subnormal. Both paths are correctly
// rounded for every float, so they agree at every normal x.
float log(const float x) noexcept {
#if STXLIB_MATH_REGION_LOG_FLOAT
    if (paths::not_subnormal(x)) return region::log(x);
#endif
    const float result = today::log(x);
    return result;
}

float log2(const float x) noexcept {
#if STXLIB_MATH_REGION_LOG_FLOAT
    if (paths::not_subnormal(x)) return region::log2(x);
#endif
    const float result = today::log2(x);
    return result;
}

float log10(const float x) noexcept {
#if STXLIB_MATH_REGION_LOG_FLOAT
    if (paths::not_subnormal(x)) return region::log10(x);
#endif
    const float result = today::log10(x);
    return result;
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
