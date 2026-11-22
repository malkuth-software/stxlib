// Half-turn trigonometry and cancellation-safe cosine losses (declared, with
// their contracts, in stxlib/math.hpp): sincospi, sinpi, cospi, sincpi,
// cosm1 and cosm1pi. The radian sinc, sin(x) / x, is not provided (owner,
// 2026-11-22): it is sincpi(x / pi), with the rounding of x / pi.
//
// Adopted 2026-11-22 from the study on branch codex/pi-trig-and-interval-study
// (f341b2c), reimplemented here in the house style; the study's Gaussian
// interval mass was rejected by the owner and is not part of it.
//
// ---- The half-turn reduction -----------------------------------------------
//
// sin(pi x) and cos(pi x) have period 2 in x, and the reduction to the
// nearest multiple of 1/128 is exact, so no multiple of pi is ever
// subtracted. For m = |x|:
//
//   q = round(128 m), d = m - q/128 (exact, |d| <= 1/256),
//   a = q pi/128, the anchors S = sin a, C = cos a from sincos_table256.hpp
//   (index q mod 256: the same angles as the radian kernel's table),
//   sin(pi m) = S cos(pi d) + C sin(pi d)
//             = S + (pi C) d + [C (sin(pi d) - pi d) + S (cos(pi d) - 1)]
//   cos(pi m) = C - (pi S) d + [C (cos(pi d) - 1) - S (sin(pi d) - pi d)]
//
// d = m - q/128 is exact: q/128 is exact, and for q != 0 (m >= 1/256)
// q/128 lies within a factor two of m (Sterbenz). Below 2^44, 128 m < 2^51
// and the rounding shift finds q; from 2^44, m is a multiple of 2^-8 and is
// first reduced modulo 2 exactly in whole numbers (256 m < 2^61); from 2^53
// every double is an even whole number, with sine 0 and cosine 1.
//
// ---- The exact head ---------------------------------------------------------
//
// Each cell stores its anchors rounded to multiples of 2^-52 and the slopes
// pi C and -pi S rounded to multiples of 2^-22 (|slope| < 4: at most 24
// bits), each with the rest as a low part (half_turn_cells, built at compile
// time from sincos_table256.hpp). With d1 = d rounded to a multiple of
// 2^-30 (|d1| <= 2^-8: at most 22 bits), the head
//
//   anchor + slope d1
//
// is exact without an exact product: slope d1 is a multiple of 2^-52 of at
// most 46 bits, and the sum is a multiple of 2^-52 below 2 in magnitude,
// which binary64 holds. At the four axis cells (q = 0 mod 64) one anchor is
// zero and its slope is +-pi rounded, while the other anchor is +-1 with
// slope zero; there d1 = cut_26(d) instead (26 bits; the product has at
// most 50), so the head keeps its relative accuracy as d tends to zero. The
// rest, slope d2 + slope_low d with d2 = d - d1 exact, joins the low part
// with the series terms. The head therefore needs no fused multiply-add and
// no Dekker split, and every variant gives the same bits.
// half_turn_cells_valid checks the lattice facts at compile time.
//
// The series: sin(pi d) - pi d to the d^7 term and cos(pi d) - 1 to the d^8
// term, with the correctly rounded coefficients (-1)^k pi^k / k!; the first
// omitted terms are below 2^-75 and 2^-83 for |d| <= 1/256
// (tools/math/half_turn_certificate.py checks the coefficients, the
// remainders and the lattice facts with exact rationals).
//
// Accuracy: the rounding error of the low part is not yet bounded by a
// certificate. The study measured, against MPFR at 256 bits over 172,288
// inputs (binades, lattice centres, midpoints, neighbours, random phases),
// at most 0.500020 ulp (sinpi), 0.500113 (cospi), 0.500295 (sincpi),
// 0.501493 (cosm1pi) and 0.500100 (cosm1), for a
// kernel with this one's operations then. That kernel left the anchors'
// own 2^-53 out of the second-order terms (rotate_half_turn below), and
// sincpi reached 0.500748 ulp off the study's sample; with it in, the
// integrator's run of half_turn_accuracy (2026-11-22, 4 x 100,000 inputs)
// measured at most 0.500102 (sinpi), 0.500063 (cospi), 0.500129 (sincpi),
// 0.500169 (cosm1pi) and 0.500154 (cosm1).
//
// ---- The cosine losses -------------------------------------------------------
//
// cos t - 1 = -2 sin^2(t/2): the half-angle sine comes from the kernels
// above (half-turns) or from the radian kernel (sincos_core.hpp), and its
// square is formed with an exact product, so no 1 - cos t is ever
// subtracted. sincpi divides the double-double sine by the double-double
// angle once.
//
// Product ranges (the same bits from both product forms, kernel_support.hpp
// two_product): pi times m is formed for m in [2^-474, 2^53) only (smaller
// arguments are scaled by 2^600 first); a square is formed for a sine of
// magnitude in [2^-459, 2^502] or zero; the quotients' products are
// between 2^-120 and 2^2.

#include "stxlib/math.hpp"

#include "atan_table128.hpp"
#include "sincos_core.hpp"

#include <limits>

#include "variant_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

// (-1)^(k div 2) pi^k / k!, correctly rounded (half_turn_certificate.py).
constexpr double half_turn_cos_2 = -0x1.3bd3cc9be45dep+2;
constexpr double half_turn_sin_3 = -0x1.4abbce625be53p+2;
constexpr double half_turn_cos_4 = 0x1.03c1f081b5ac4p+2;
constexpr double half_turn_sin_5 = 0x1.466bc6775aae2p+1;
constexpr double half_turn_cos_6 = -0x1.55d3c7e3cbffap+0;
constexpr double half_turn_sin_7 = -0x1.32d2cce62bd86p-1;
constexpr double half_turn_cos_8 = 0x1.e1f506891babbp-3;

// Below it the half-turn functions take pi m directly; at and above it the
// cell reduction's products and squares stay normal.
constexpr double half_turn_tiny_limit = 0x1p-100;
// From it the rounding shift cannot find round(128 m) (128 m >= 2^51).
constexpr double half_turn_whole_limit = 0x1p44;
// From it every double is an even whole number.
constexpr double half_turn_even_limit = 0x1p53;
// The scaling that keeps pi m's product normal for tiny m.
constexpr double tiny_scale_up = 0x1p600;
constexpr double tiny_scale_down = 0x1p-600;
// d + slope_shift - slope_shift rounds |d| <= 2^-8 to a multiple of 2^-30
// (the sum lies in [2^22, 2^23), spaced 2^-30).
constexpr double slope_shift = 0x1.8p22;

struct HalfTurnCell final {
    double sine;              // S rounded to a multiple of 2^-52
    double sine_low;          // (S + S_lo) - sine
    double cosine;            // C rounded to a multiple of 2^-52
    double cosine_low;        // (C + C_lo) - cosine
    double sine_slope;        // pi C rounded to a multiple of 2^-22
    double sine_slope_low;    // pi (C + C_lo) - sine_slope
    double cosine_slope;      // -pi S rounded to a multiple of 2^-22
    double cosine_slope_low;  // -pi (S + S_lo) - cosine_slope
};

struct HalfTurnCells final {
    HalfTurnCell values[256];
};

static_assert(sizeof(HalfTurnCell) == 64, "a half-turn cell fills one cache line");

// value (|value| <= 1) rounded to a multiple of 2^-52: |value| + 1 lies in
// [1, 2], spaced 2^-52, and subtracting 1 is exact.
constexpr double round_to_lattice_52(const double value) noexcept {
    if (value < 0.0) {
        const double magnitude = -value;
        const double rounded = (magnitude + 1.0) - 1.0;
        return -rounded;
    }
    const double rounded = (value + 1.0) - 1.0;
    return rounded;
}

// pi (hi + lo) as a double-double, for |hi| <= 1 zero or at least 2^-6:
// Dekker's product written out (two_product is not constexpr; constant
// evaluation rounds as the run time does).
constexpr Wide times_pi_constant(const double hi, const double lo) noexcept {
    const Split pi_parts = veltkamp_split(atan_pi_hi);
    const Split hi_parts = veltkamp_split(hi);
    const double product = hi * atan_pi_hi;
    const double product_error = ((hi_parts.hi * pi_parts.hi - product) + hi_parts.hi * pi_parts.lo +
                                  hi_parts.lo * pi_parts.hi) +
                                 hi_parts.lo * pi_parts.lo;
    const double low = product_error + (hi * atan_pi_lo + lo * atan_pi_hi);
    const Wide result{product, low};
    return result;
}

// A slope (|slope| < 4) rounded to a multiple of 2^-22, and its low part.
// product - head is exact: below 2^-23 and on product's grid.
constexpr Wide slope_parts(const Wide slope) noexcept {
    const double scaled = slope.hi * 0x1p22;
    const double head = ((scaled + rounding_shift) - rounding_shift) * 0x1p-22;
    const double low = (slope.hi - head) + slope.lo;
    const Wide result{head, low};
    return result;
}

constexpr HalfTurnCells make_half_turn_cells() noexcept {
    HalfTurnCells table{};
    for (int i = 0; i < 256; ++i) {
        const SinCosEntry& entry = sincos_table_256[i];
        const double sine = round_to_lattice_52(entry.s);
        const double cosine = round_to_lattice_52(entry.c);
        const Wide sine_slope = slope_parts(times_pi_constant(entry.c, entry.c_lo));
        const Wide cosine_slope = slope_parts(times_pi_constant(-entry.s, -entry.s_lo));
        table.values[i] = HalfTurnCell{sine,           (entry.s - sine) + entry.s_lo,
                                       cosine,         (entry.c - cosine) + entry.c_lo,
                                       sine_slope.hi,  sine_slope.lo,
                                       cosine_slope.hi, cosine_slope.lo};
    }
    return table;
}

alignas(64) constexpr HalfTurnCells half_turn_cells = make_half_turn_cells();

// value times unit_scale is a whole number of magnitude below unit_limit.
constexpr bool on_lattice(const double value, const double unit_scale, const double unit_limit) noexcept {
    const double units = value * unit_scale;
    if (units >= unit_limit || units <= -unit_limit) return false;
    const bool whole = units == static_cast<double>(static_cast<std::int64_t>(units));
    return whole;
}

// The head's exactness facts (the head of this file): anchors on 2^-52 in
// [-1, 1], slopes on 2^-22 below 2^24 units (4), and at each axis cell one
// coordinate with anchor zero and the other with slope zero.
constexpr bool half_turn_cells_valid() noexcept {
    for (int i = 0; i < 256; ++i) {
        const HalfTurnCell& cell = half_turn_cells.values[i];
        if (!on_lattice(cell.sine, 0x1p52, 0x1p52 + 1.0) || !on_lattice(cell.cosine, 0x1p52, 0x1p52 + 1.0)) {
            return false;
        }
        if (!on_lattice(cell.sine_slope, 0x1p22, 0x1p24) || !on_lattice(cell.cosine_slope, 0x1p22, 0x1p24)) {
            return false;
        }
        if ((i & 63) == 0) {
            if (cell.sine != 0.0 && cell.sine_slope != 0.0) return false;
            if (cell.cosine != 0.0 && cell.cosine_slope != 0.0) return false;
        }
    }
    return true;
}

static_assert(half_turn_cells_valid(), "a half-turn cell breaks the exact head's lattice");

// sin(pi m) and cos(pi m), each as an unnormalized pair head + low.
struct HalfTurn final {
    Wide sine;
    Wide cosine;
};

// The rotation from cell `index` by d (|d| <= 1/256; d zero or at least
// 2^-100 at index 0, at least 2^-60 elsewhere).
HalfTurn rotate_half_turn(const std::uint64_t index, const double d) noexcept {
    const HalfTurnCell& cell = half_turn_cells.values[index];
    const double square = d * d;
    // sin(pi d) - pi d and cos(pi d) - 1.
    const double cubic = (d * square) * (half_turn_sin_3 + square * (half_turn_sin_5 + square * half_turn_sin_7));
    const double loss =
        square * (half_turn_cos_2 + square * (half_turn_cos_4 + square * (half_turn_cos_6 + square * half_turn_cos_8)));
    // The split of d that makes slope * d1 exact (head of this file).
    double d1 = 0.0;
    if ((index & 63) == 0) {
        d1 = cut_26(d);
    } else {
        d1 = (d + slope_shift) - slope_shift;
    }
    const double d2 = d - d1;  // exact
    const double sine_cross = cell.sine_slope * d2 + cell.sine_slope_low * d;
    const double cosine_cross = cell.cosine_slope * d2 + cell.cosine_slope_low * d;
    // The second-order terms take the cell's whole sine and cosine: the
    // lattice anchors alone are up to 2^-53 off, which times a loss of up
    // to 2^-14 left 2^-67 out of the low part (sincpi measured 0.500748 ulp
    // at -0x1.8e74a2072cf8p-7 before; the study's kernel too).
    const double whole_sine = cell.sine + cell.sine_low;
    const double whole_cosine = cell.cosine + cell.cosine_low;
    HalfTurn result{};
    result.sine.hi = cell.sine + cell.sine_slope * d1;          // exact
    result.cosine.hi = cell.cosine + cell.cosine_slope * d1;    // exact
    result.sine.lo = cell.sine_low + (sine_cross + (whole_cosine * cubic + whole_sine * loss));
    result.cosine.lo = cell.cosine_low + (cosine_cross + (whole_cosine * loss - whole_sine * cubic));
    return result;
}

// sin(pi m) and cos(pi m) for 2^-100 <= m < 2^53 (whole numbers above
// 2^53 are even: sine 0, cosine 1, decided by the callers).
HalfTurn half_turn_wide(const double m) noexcept {
    double phase = m;
    if (m >= half_turn_whole_limit) {
        // m is a multiple of 2^-8 below 2^53: 256 m is a whole number below
        // 2^61, and its residue modulo 512 is m modulo 2 in units of 2^-8.
        const std::uint64_t units = static_cast<std::uint64_t>(m * 256.0);
        phase = static_cast<double>(units & 511) * 0x1p-8;
    }
    const double shifted = phase * 128.0 + rounding_shift;
    // The shift leaves q in the low mantissa bits; q mod 256 is the index.
    const std::uint64_t index = std::bit_cast<std::uint64_t>(shifted) & sincos_index_mask;
    const double q = shifted - rounding_shift;
    const double d = phase - q * 0x1p-7;  // exact
    if (d == 0.0) {
        // A lattice point: the table's correctly rounded values.
        const SinCosEntry& node = sincos_table_256[index];
        const HalfTurn result{Wide{node.s, node.s_lo}, Wide{node.c, node.c_lo}};
        return result;
    }
    const HalfTurn result = rotate_half_turn(index, d);
    return result;
}

// pi m as a double-double for 2^-474 <= m < 2^53.
Wide times_pi(const double m) noexcept {
    double product = 0.0;
    double product_error = 0.0;
    two_product(m, atan_pi_hi, product, product_error);
    const double low = product_error + m * atan_pi_lo;
    Wide result{};
    fast_two_sum(product, low, result.hi, result.lo);
    return result;
}

// The rounded pair for any x: the sign of x on the sine, NaN for infinite
// and NaN x (NaN payloads kept).
struct RoundedPair final {
    double sine;
    double cosine;
};

RoundedPair half_turn_rounded(const double x) noexcept {
    const double m = std::fabs(x);
    RoundedPair result{0.0, 1.0};
    if (m >= half_turn_tiny_limit && m < half_turn_even_limit) {
        const HalfTurn wide = half_turn_wide(m);
        result.sine = wide.sine.hi + wide.sine.lo;
        result.cosine = wide.cosine.hi + wide.cosine.lo;
    } else if (m < half_turn_tiny_limit) {
        // sin(pi m) = pi m (1 - (pi m)^2/6) with (pi m)^2/6 < 2^-196, and
        // cos(pi m) rounds to 1. Rounded before scaling back, so a normal
        // result rounds once.
        const Wide scaled = times_pi(m * tiny_scale_up);
        result.sine = (scaled.hi + scaled.lo) * tiny_scale_down;
    } else if (!(m < std::numeric_limits<double>::infinity())) {
        // Infinite or NaN.
        double refused = std::numeric_limits<double>::quiet_NaN();
        if (x != x) refused = x + x;
        result.sine = refused;
        result.cosine = refused;
        return result;
    }
    // m >= 2^53 keeps sine +0 and cosine 1. Odd: the sine takes x's sign,
    // zeros included.
    if (std::signbit(x)) result.sine = -result.sine;
    return result;
}

// sin h for 2^-100 <= h <= 2^32 radians, as an unnormalized pair.
Wide radian_sine(const double h) noexcept {
    Wide sine{};
    if (h < sincos_tiny_limit) {
        // sin h = h + h^3 (-1/6 + h^2/120) with the next term below
        // h^7/5040 < 2^-200 h.
        const double h_squared = h * h;
        sine.hi = h;
        sine.lo = h * (h_squared * (sin_3 + h_squared * sin_5));
        return sine;
    }
    double cosine_hi = 0.0;
    double cosine_lo = 0.0;
    if (h <= sincos_fast_limit) {
        sincos_wide(h, sine.hi, sine.lo, cosine_hi, cosine_lo);
    } else {
        sincos_wide_medium(h, sine.hi, sine.lo, cosine_hi, cosine_lo);
    }
    return sine;
}

// sin(a) / a = 1 + a^2 (-1/6 + a^2 (1/120 - a^2/5040)) for 0 <= a <= 2^-6.3,
// with the next term below a^8/9! < 2^-68.
double sinc_series(const double a) noexcept {
    const double square = a * a;
    const double result = 1.0 + square * (sin_3 + square * (sin_5 + square * sin_7));
    return result;
}

// sincpi within 2^-8 of a nonzero whole number n below 2^52, m = n + d with
// d != 0: sin(pi m) / (pi m) = (-1)^n (d / m) (sin(pi d) / (pi d)), so pi
// cancels and the quotient d / m is a double-double with an exact residual.
// The series' excess sin(a)/a - 1 (a = pi |d| <= 2^-6.3) scales the head
// into the tail, and the tail is rounded to odd before the one final sum,
// so a result next to a midpoint of the head's grid (1 - 2^-53 lies 2^-54
// ulp below one) is not pushed onto it by the tail's own rounding.
double near_whole_sinc(const double m, const double whole, const double offset) noexcept {
    const std::uint64_t odd = static_cast<std::uint64_t>(whole) & 1;
    const double signed_offset = odd != 0 ? -offset : offset;
    const Wide ratio = divide(signed_offset, 0.0, m, 0.0);
    const double angle = times_pi(std::fabs(offset)).hi;
    const double square = angle * angle;
    const double excess = square * (sin_3 + square * (sin_5 + square * sin_7));
    double tail = 0.0;
    double tail_error = 0.0;
    two_sum(ratio.lo, ratio.hi * excess, tail, tail_error);
    // Round to odd (as round_wide_to_float): step an even tail toward its
    // rounding error; tail is nonzero whenever the error is.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(tail);
    const std::uint64_t inexact = static_cast<std::uint64_t>(tail_error != 0.0);
    const std::uint64_t adjust = inexact & ((bits & 1) ^ 1);
    const std::uint64_t outward = static_cast<std::uint64_t>((tail_error > 0.0) == (tail > 0.0));
    const double odd_tail = std::bit_cast<double>((bits + (adjust & outward)) - (adjust & (outward ^ 1)));
    const double result = ratio.hi + odd_tail;
    return result;
}

// -2 (hi + lo)^2 rounded once, for |hi| zero or in [2^-459, 2^502]: the
// pair is normalized first (the rotations hand over low parts up to 2^-29
// of the head), the leading square is an exact product, and the cross term
// 2 hi lo joins its error.
double negative_twice_square(const Wide sine) noexcept {
    double hi = 0.0;
    double lo = 0.0;
    two_sum(sine.hi, sine.lo, hi, lo);
    double square = 0.0;
    double square_error = 0.0;
    two_product(hi, hi, square, square_error);
    const double low = square_error + (2.0 * hi) * lo;
    const double result = -2.0 * (square + low);
    return result;
}

} // namespace
} // namespace detail

// ---- Double ---------------------------------------------------------------------

void sincospi(const double x, double& s, double& c) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(x);
    s = pair.sine;
    c = pair.cosine;
}

double sinpi(const double x) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(x);
    return pair.sine;
}

double cospi(const double x) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(x);
    return pair.cosine;
}

// sin(pi x) / (pi x): even, 1 near zero, +0 at nonzero whole numbers (the
// sine is an exact zero there) and at the infinities.
double sincpi(const double x) noexcept {
    const double m = std::fabs(x);
    if (m < 0x1p-28) return 1.0;  // (pi m)^2/6 < 2^-55.2: rounds to 1
    if (m >= 0x1p52) return 0.0;  // every such double is whole; infinities too
    if (x != x) return x + x;
    const detail::Wide angle = detail::times_pi(m);
    if (m < 0x1p-8) {
        const double result = detail::sinc_series(angle.hi);
        return result;
    }
    // The nearest whole number (m < 2^52; the sum rounds to it) and the exact
    // offset from it.
    const double whole = (m + 0x1p52) - 0x1p52;
    const double offset = m - whole;
    if (offset == 0.0) return 0.0;
    if (whole != 0.0 && std::fabs(offset) < 0x1p-8) {
        const double result = detail::near_whole_sinc(m, whole, offset);
        return result;
    }
    const detail::Wide sine = detail::half_turn_wide(m).sine;
    double sine_hi = 0.0;
    double sine_lo = 0.0;
    detail::two_sum(sine.hi, sine.lo, sine_hi, sine_lo);
    const detail::Wide quotient = detail::divide(sine_hi, sine_lo, angle.hi, angle.lo);
    const double result = quotient.hi + quotient.lo;
    return result;
}

// cos(x) - 1 = -2 sin^2(x/2) for |x| <= 2^32 radians; NaN beyond.
double cosm1(const double x) noexcept {
    const double m = std::fabs(x);
    if (!(m <= detail::sincos_medium_limit)) {
        double refused = std::numeric_limits<double>::quiet_NaN();
        if (x != x) refused = x + x;
        return refused;
    }
    const double half = 0.5 * m;
    if (half < detail::half_turn_tiny_limit) {
        // -x^2/2, with the next term x^4/24 below 2^-200 of it; -0 at zero.
        const double result = -(half * m);
        return result;
    }
    const double result = detail::negative_twice_square(detail::radian_sine(half));
    return result;
}

// cos(pi x) - 1 = -2 sin^2(pi x/2) for every finite x: -0 at even whole
// numbers, -2 at odd ones.
double cosm1pi(const double x) noexcept {
    const double m = std::fabs(x);
    if (!(m < std::numeric_limits<double>::infinity())) {
        double refused = std::numeric_limits<double>::quiet_NaN();
        if (x != x) refused = x + x;
        return refused;
    }
    const double half = 0.5 * m;
    if (half >= detail::half_turn_even_limit) return -0.0;  // m/2 whole: sine zero
    if (half >= detail::half_turn_tiny_limit) {
        const double result = detail::negative_twice_square(detail::half_turn_wide(half).sine);
        return result;
    }
    // Below 2^-540 the exact value is below 2^-1076 and rounds to -0.
    if (half < 0x1p-540) return -0.0;
    // pi half scaled by 2^600 lies in [2^61, 2^502); its loss, scaled by
    // 2^1200, is scaled back in two exact steps for a normal result.
    const detail::Wide scaled = detail::times_pi(half * detail::tiny_scale_up);
    const double loss = detail::negative_twice_square(scaled);
    const double result = (loss * detail::tiny_scale_down) * detail::tiny_scale_down;
    return result;
}

// ---- Float: the double functions of the widened argument, rounded once -------

void sincospi(const float x, float& s, float& c) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(detail::widen(x));
    s = static_cast<float>(pair.sine);
    c = static_cast<float>(pair.cosine);
}

float sinpi(const float x) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(detail::widen(x));
    return static_cast<float>(pair.sine);
}

float cospi(const float x) noexcept {
    const detail::RoundedPair pair = detail::half_turn_rounded(detail::widen(x));
    return static_cast<float>(pair.cosine);
}

float sincpi(const float x) noexcept {
    const double result = sincpi(detail::widen(x));
    return static_cast<float>(result);
}

float cosm1(const float x) noexcept {
    const double result = cosm1(detail::widen(x));
    return static_cast<float>(result);
}

float cosm1pi(const float x) noexcept {
    const double result = cosm1pi(detail::widen(x));
    return static_cast<float>(result);
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
