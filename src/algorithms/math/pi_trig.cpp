// Trigonometric functions in half-turns and cancellation-safe cosine losses.
#include "atan_table128.hpp"
#include "sincos_core.hpp"
#include "variant_functions.hpp"

#include <limits>

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {
namespace detail {
namespace {

struct Rotation final {
    Wide sine;
    Wide cosine;
};

// Product with a two-part pi. Inputs on the table path are at least 2^-100
// unless zero (a nonzero offset from a nonzero cell centre is >= 2^-61).
Wide times_pi(const double x) noexcept {
    Wide p{};
    two_product(x, atan_pi_hi, p.hi, p.lo);
    const double low = p.lo + x * atan_pi_lo;
    Wide result{};
    fast_two_sum(p.hi, low, result.hi, result.lo);
    return result;
}

// Anchors rounded to multiples of 2^-52 and slopes to multiples of
// 2^-22. With d1 on the 2^-30 grid the head plus its linear increment
// is exactly representable, even when the sum crosses a binade.
struct HalfTurnCell final {
    double sine, sine_low, cosine, cosine_low;
    double sine_slope, sine_slope_low, cosine_slope, cosine_slope_low;
};
alignas(64) constexpr HalfTurnCell half_turn_cells[] = {
#include "pi_slopes.inc"
};
static_assert(sizeof(HalfTurnCell) == 64);

Rotation rotate_half_turn(const std::uint64_t index, const double d) noexcept {
    const HalfTurnCell& cell = half_turn_cells[index];
    const double square = d * d;
    constexpr double a2 = -0x1.3bd3cc9be45dep+2; // (-1)^1 pi^2/2!
    constexpr double a3 = -0x1.4abbce625be53p+2; // (-1)^1 pi^3/3!
    constexpr double a4 = 0x1.03c1f081b5ac4p+2; // (-1)^2 pi^4/4!
    constexpr double a5 = 0x1.466bc6775aae2p+1; // (-1)^2 pi^5/5!
    constexpr double a6 = -0x1.55d3c7e3cbffap+0; // (-1)^3 pi^6/6!
    constexpr double a7 = -0x1.32d2cce62bd86p-1; // (-1)^3 pi^7/7!
    constexpr double a8 = 0x1.e1f506891babbp-3; // (-1)^4 pi^8/8!
    const double cubic = (d * square) * (a3 + square * (a5 + square * a7));
    const double loss = square * (a2 + square * (a4 + square * (a6 + square * a8)));
    // A zero anchor needs a relative split as d tends to zero. Its slope
    // times the 26-bit head is exact. The other coordinate has zero slope.
    const double d1 = (index & 63u) == 0 ? cut_26(d) : (d + 0x1.8p22) - 0x1.8p22;
    const double d2 = d - d1;
    Rotation result{};
    result.sine.hi = cell.sine + cell.sine_slope * d1;
    result.cosine.hi = cell.cosine + cell.cosine_slope * d1;
    const double sine_cross = cell.sine_slope * d2 + cell.sine_slope_low * d;
    const double cosine_cross = cell.cosine_slope * d2 + cell.cosine_slope_low * d;
    result.sine.lo = cell.sine_low + (sine_cross + (cell.cosine * cubic + cell.sine * loss));
    result.cosine.lo = cell.cosine_low + (cosine_cross + (cell.cosine * loss - cell.sine * cubic));
    return result;
}

// Nonnegative x. Above 2^53 every double is even. Below it, 128*x fits
// in uint64_t. Power-of-two scaling and subtraction from the nearest
// lattice point are exact, including on either side of a quadrant.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline Rotation pi_rotation(const double x) noexcept {
    if (x >= 0x1p53) return {{0.0, 0.0}, {1.0, 0.0}};
    if (x < 0x1p-100) {
        // Keep the product residual normal for both Dekker and FMA.
        const Wide scaled = times_pi(x * 0x1p600);
        if (x < 0x1p-900) return {{scaled.hi * 0x1p-600, 0.0}, {1.0, 0.0}};
        return {{scaled.hi * 0x1p-600, scaled.lo * 0x1p-600}, {1.0, 0.0}};
    }
    const double q = std::nearbyint(x * 128.0);
    const auto index = static_cast<std::uint64_t>(q) & sincos_index_mask;
    const double d = x - q * 0x1p-7;
    if (d == 0.0) {
        const SinCosEntry& node = sincos_table_256[index];
        return {{node.s, node.s_lo}, {node.c, node.c_lo}};
    }
    return rotate_half_turn(index, d);
}

// Wide sine for a supported radian magnitude above the tiny shortcuts.
Wide radian_sine(const double x) noexcept {
    Rotation rotation{};
    if (x <= sincos_fast_limit) {
        sincos_wide(x, rotation.sine.hi, rotation.sine.lo, rotation.cosine.hi, rotation.cosine.lo);
    } else {
        sincos_wide_medium(x, rotation.sine.hi, rotation.sine.lo, rotation.cosine.hi, rotation.cosine.lo);
    }
    return rotation.sine;
}

double sinc_series(const double r) noexcept {
    const double square = r * r;
    return 1.0 + square * (sin_3 + square * (sin_5 + square * sin_7));
}

// -2 sin²(x/2). The rounding residual of the leading square and the
// sine's low part both contribute before the final rounding.
double negative_twice_square(Wide sine) noexcept {
    // The rotation returns an unnormalized pair. Its low square can
    // matter until this exact sum moves the correction into the head.
    two_sum(sine.hi, sine.lo, sine.hi, sine.lo);
    const bool tiny = std::fabs(sine.hi) < 0x1p-450;
    if (std::fabs(sine.hi) < 0x1p-540) return -0.0;
    if (tiny) {
        sine.hi *= 0x1p500;
        sine.lo *= 0x1p500;
    }
    double square = 0.0;
    double error = 0.0;
    two_product(sine.hi, sine.hi, square, error);
    const double low = error + (2.0 * sine.hi) * sine.lo;
    const double value = -2.0 * (square + low);
    return tiny ? value * 0x1p-1000 : value;
}

struct RoundedPair final { double sine, cosine; };

// Keep the rotation visible in each scalar entry so it can omit the
// other component while preserving the operation order of the pair.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline RoundedPair rounded_phase(const double x) noexcept {
    // In this binade the shift rounds 128*x to an integer and exposes
    // its low eight bits directly, including for negative arguments.
    const double magnitude = std::fabs(x);
    if (magnitude >= 0x1p-100 && magnitude <= 0x1p44) {
        constexpr double shift = 0x1.8p52;
        const double shifted = shift + x * 128.0;
        const auto index = std::bit_cast<std::uint64_t>(shifted) & sincos_index_mask;
        const double d = x - (shifted - shift) * 0x1p-7;
        if (d == 0.0) {
            const auto& node = sincos_table_256[index];
            return {node.s == 0.0 ? std::copysign(0.0, x) : node.s, node.c};
        }
        const Rotation rotation = rotate_half_turn(index, d);
        return {rotation.sine.hi + rotation.sine.lo, rotation.cosine.hi + rotation.cosine.lo};
    }
    if (!std::isfinite(x)) {
        const double refused = std::isnan(x) ? x + x : std::numeric_limits<double>::quiet_NaN();
        return {refused, refused};
    }
    const Rotation rotation = pi_rotation(std::fabs(x));
    const double sine = rotation.sine.hi + rotation.sine.lo;
    return {std::signbit(x) ? -sine : sine, rotation.cosine.hi + rotation.cosine.lo};
}

} // namespace
} // namespace detail

void sincospi(const double x, double& s, double& c) noexcept {
    const auto pair = detail::rounded_phase(x);
    s = pair.sine;
    c = pair.cosine;
}

double sinpi(const double x) noexcept {
    return detail::rounded_phase(x).sine;
}

double cospi(const double x) noexcept {
    return detail::rounded_phase(x).cosine;
}

double sincpi(const double x) noexcept {
    const double magnitude = std::fabs(x);
    if (magnitude < 0x1p-28) return 1.0;
    if (magnitude >= 0x1p52) return 0.0; // All finite inputs here are integers.
    if (std::isnan(x)) return x + x;
    const detail::Wide angle = detail::times_pi(magnitude);
    if (magnitude < 0x1p-8) {
        return detail::sinc_series(angle.hi);
    }
    const detail::Wide sine = detail::pi_rotation(magnitude).sine;
    const detail::Wide quotient = detail::divide(sine.hi, sine.lo, angle.hi, angle.lo);
    return quotient.hi + quotient.lo;
}

double sinc_radians(const double x) noexcept {
    const double magnitude = std::fabs(x);
    if (magnitude < 0x1p-7) return detail::sinc_series(magnitude);
    if (!(magnitude <= detail::sincos_medium_limit)) {
        return std::isnan(x) ? x + x : std::numeric_limits<double>::quiet_NaN();
    }
    const detail::Wide sine = detail::radian_sine(magnitude);
    const detail::Wide result = detail::divide(sine.hi, sine.lo, magnitude, 0.0);
    return result.hi + result.lo;
}

double cosm1pi(const double x) noexcept {
    if (!std::isfinite(x)) return std::isnan(x) ? x + x : std::numeric_limits<double>::quiet_NaN();
    return detail::negative_twice_square(detail::pi_rotation(0.5 * std::fabs(x)).sine);
}

double cosm1(const double x) noexcept {
    const double magnitude = std::fabs(x);
    if (!(magnitude <= detail::sincos_medium_limit)) {
        return std::isnan(x) ? x + x : std::numeric_limits<double>::quiet_NaN();
    }
    const double half = 0.5 * magnitude;
    if (half < 0x1p-100) return -half * magnitude;
    return detail::negative_twice_square(detail::radian_sine(half));
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
