// The trigonometric region kernels (declared in region_functions.hpp):
// sincos and tan of a double, faster than the library's at the library's
// bound (0.51 ulp, math.hpp), with its subnormal rule. The owner's aim for
// this work (2026-11-15) is speed at today's accuracy, not tighter bounds.
// A library kernel source (every variant), one of two paths for sincos and
// tan of a double: the switch STXLIB_MATH_REGION_TRIG_DOUBLE (paths.hpp,
// on by default) sends sincos.cpp's and tan.cpp's variant functions here;
// tests/math_region_tests.cpp tests and benches both paths. Design and the
// proof: the seat's
// dev/notes/planning/stages/M-trig-region.md and
// tools/math/trig_region_certificate.py.
//
// What changes against sincos_core.hpp and tan.cpp, and why each is faster:
//   - A region reduction for 2^-27 <= |x| < 2^6 (ten turns: every angle a
//     GUI forms): pi/128 in parts of 41, 41 and 53 bits. q < 2^12 there, so
//     q * part_1 and q * part_2 are exact, and the split holds pi/128 to
//     ~148 bits: its error stays within the reduction contract at every
//     remainder a double can produce (certified minimum 2^-66.49), so the
//     library's test |r| < 2^-30 and its cold eight-part redo are gone, and
//     the final normalization is a fast two-sum (|low| < 2^-60 |r2|).
//     Above 2^6 the library's reductions serve, unchanged.
//   - One cache line per table index: the table's sin, cos, their low parts
//     and the 26-bit cuts of both in one 64-byte entry (the library keeps
//     the cuts in a second table, a second line per call).
//   - Shorter dependency chains in the polynomials (Estrin's scheme): sin r
//     - r as r^3 ((s3 + s5 r^2) + s7 r^4), tan r - r as r^3 ((t3 + t5 r^2) +
//     r^4 ((t7 + t9 r^2) + t11 r^4)); cos r - 1 keeps its form.
// The rotation and the split division are the library's otherwise. Every
// rounding is counted in the certificate, which recomputes the core's error
// and the reduction contract for these chains.

#include "stxlib/math.hpp"

#include "sincos_core.hpp"

#include "core/subnormal.hpp"

#include <limits>

#include "region_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

// pi/128 = trig_region_step_1 + trig_region_step_2 + trig_region_step_3
// (~148 bits): 41, 38 and 53 significant bits; q * step_1 and q * step_2
// are exact for |q| < 2^12 (|x| < 2^6 gives |q| <= 2608).
inline constexpr double trig_region_step_1 = 0x1.921fb54443000p-6;
inline constexpr double trig_region_step_2 = -0x1.73dcb3b398000p-49;
inline constexpr double trig_region_step_3 = -0x1.d747f23e32ed7p-89;
inline constexpr double trig_region_limit = 0x1p6;
inline constexpr std::uint64_t trig_region_limit_bits = std::bit_cast<std::uint64_t>(trig_region_limit);

// One table index per cache line: the library's entry and its cuts.
struct alignas(64) SinCosLine final {
    double s{0.0};
    double c{0.0};
    double s_lo{0.0};
    double c_lo{0.0};
    double s1{0.0};
    double s2{0.0};
    double c1{0.0};
    double c2{0.0};
};

static_assert(sizeof(SinCosLine) == 64, "one table index per 64-byte line");

struct SinCosLines final {
    SinCosLine values[256];
};

constexpr SinCosLines make_sincos_lines() noexcept {
    SinCosLines table{};
    for (int i = 0; i < 256; ++i) {
        const SinCosEntry& entry = sincos_table_256[i];
        const SinCosCut& cut = sincos_cuts.values[i];
        table.values[i] = SinCosLine{entry.s, entry.c, entry.s_lo, entry.c_lo, cut.s1, cut.s2, cut.c1, cut.c2};
    }
    return table;
}

alignas(64) inline constexpr SinCosLines sincos_lines = make_sincos_lines();

// True when 2^-27 <= |x| < 2^6, false for NaN.
inline bool trig_region_path(const double x) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & magnitude_mask;
    const bool result = magnitude >= sincos_tiny_bits && magnitude < trig_region_limit_bits;
    return result;
}

// q, its table index and r + r_lo = x - q pi/128 (|r_lo| <= ulp(r)/2) for
// 2^-27 <= |x| < 2^6. x - q step_1 is exact (granularity: the certificate
// checks it per binade); two_sum keeps the next subtraction's error; the
// rest is far below the contract.
inline void trig_region_reduce(const double x, std::uint64_t& index, double& r, double& r_lo) noexcept {
    const double q = sincos_step(x, index);
    const double r1 = x - q * trig_region_step_1;
    double r2 = 0.0;
    double r2_error = 0.0;
    two_sum(r1, -(q * trig_region_step_2), r2, r2_error);
    const double low = r2_error - q * trig_region_step_3;
    // |low| <= ulp(r2)/2 + 2^-75 is below |r2| (>= 2^-66.5 - 2^-75, or
    // low = 0 when q = 0): a fast two-sum normalizes exactly.
    fast_two_sum(r2, low, r, r_lo);
}

// q's table index and r + r_lo for every 2^-27 <= |x| <= 2^32: the region
// reduction below 2^6, the library's above.
inline void trig_reduce(const double x, std::uint64_t& index, double& r, double& r_lo) noexcept {
    if (trig_region_path(x)) {
        trig_region_reduce(x, index, r, r_lo);
    } else if (sincos_fast_path(x)) {
        (void)sincos_reduce_fast(x, index, r, r_lo);
    } else {
        sincos_reduce_medium(x, index, r, r_lo);
    }
}

// sincos_core.hpp's sincos_rotate with the one-line table and sin r - r by
// Estrin's scheme (cos r - 1 is the library's expression, bit for bit).
inline void trig_region_rotate(const std::uint64_t index, const double r, const double r_lo, double& s_hi,
                               double& s_lo, double& c_hi, double& c_lo) noexcept {
    const double r_squared = r * r;
    const double r_fourth = r_squared * r_squared;
    const double r_cubed = r * r_squared;
    const double sin_polynomial = (sin_3 + r_squared * sin_5) + r_fourth * sin_7;
    const double sin_r_minus_r = r_lo + r_cubed * sin_polynomial;
    const double cos_r_minus_1 = r_fourth * (cos_4 + r_squared * cos_6) - r_squared * cos_half;
    const SinCosLine& line = sincos_lines.values[index];
    const double r1 = cut_26(r);
    const double r2 = r - r1;  // exact

    double sin_head = 0.0;
    double sin_head_error = 0.0;
    fast_two_sum(line.s, line.c1 * r1, sin_head, sin_head_error);
    const double sin_cross = line.c1 * r2 + line.c2 * r;
    const double sin_small = line.c * sin_r_minus_r + line.s * cos_r_minus_1;
    const double sin_tail = (sin_head_error + (sin_cross + sin_small)) + line.s_lo;

    double cos_head = 0.0;
    double cos_head_error = 0.0;
    fast_two_sum(line.c, -(line.s1 * r1), cos_head, cos_head_error);
    const double cos_cross = line.s1 * r2 + line.s2 * r;
    const double cos_small = line.c * cos_r_minus_1 - line.s * sin_r_minus_r;
    const double cos_tail = (cos_head_error + (cos_small - cos_cross)) + line.c_lo;

    s_hi = sin_head;
    s_lo = sin_tail;
    c_hi = cos_head;
    c_lo = cos_tail;
}

// tan.cpp's tan_rotate with the one-line table and tan r - r by Estrin's
// scheme.
inline void trig_region_tan_rotate(const std::uint64_t index, const double r, const double r_lo, Wide& numerator,
                                   Wide& denominator) noexcept {
    const double r_squared = r * r;
    const double r_fourth = r_squared * r_squared;
    const double r_cubed = r * r_squared;
    const double tan_low = tan_3 + r_squared * tan_5;
    const double tan_middle = tan_7 + r_squared * tan_9;
    const double tan_polynomial = tan_low + r_fourth * (tan_middle + r_fourth * tan_11);
    const double t_minus_r = r_cubed * tan_polynomial + r_lo * (1.0 + r_squared);
    const SinCosLine& line = sincos_lines.values[index];
    const double r1 = cut_26(r);
    const double r2 = r - r1;  // exact

    double n_head = 0.0;
    double n_head_error = 0.0;
    fast_two_sum(line.s, line.c1 * r1, n_head, n_head_error);  // c1 r1 exact
    const double n_low = (n_head_error + ((line.c1 * r2 + line.c2 * r) + line.c * t_minus_r)) + line.s_lo;
    fast_two_sum(n_head, n_low, numerator.hi, numerator.lo);

    double d_head = 0.0;
    double d_head_error = 0.0;
    fast_two_sum(line.c, -(line.s1 * r1), d_head, d_head_error);  // s1 r1 exact
    const double d_low = (d_head_error - ((line.s1 * r2 + line.s2 * r) + line.s * t_minus_r)) + line.c_lo;
    fast_two_sum(d_head, d_low, denominator.hi, denominator.lo);
}

// tan.cpp's divide_split, unchanged (its proof is the library's).
inline Wide trig_region_divide(const Wide& numerator, const Wide& denominator) noexcept {
    const double inverse = 1.0 / denominator.hi;
    const double q1 = cut_26(numerator.hi * inverse);
    const double d1 = cut_26(denominator.hi);
    const double d2 = denominator.hi - d1;  // exact
    const double residual = (((numerator.hi - q1 * d1) - q1 * d2) + numerator.lo) - q1 * denominator.lo;
    const Wide result{q1, residual * inverse};
    return result;
}

// True when 2^-27 <= |x| <= 2^32, false for NaN.
inline bool trig_supported(const double x) noexcept {
    const bool result = sincos_fast_path(x) || sincos_medium_path(x);
    return result;
}

} // namespace
} // namespace detail

namespace region {

// sin and cos of a double: math.hpp's contract for sincos(double),
// subnormal rule included (a subnormal angle reads as zero of its sign;
// no result for |x| >= 2^-27 is subnormal).
void sincos(const double x, double& s, double& c) noexcept {
    if (!detail::trig_supported(x)) {
        const double flushed = ::stxlib::detail::flush_subnormal(x);
        if (std::fabs(flushed) < detail::sincos_tiny_limit) {
            s = flushed;
            c = 1.0;
        } else {
            // |x| > 2^32, infinities and NaN: refused (NaN propagates).
            const double refused = x != x ? x + x : std::numeric_limits<double>::quiet_NaN();
            s = refused;
            c = refused;
        }
        return;
    }
    std::uint64_t index = 0;
    double r = 0.0;
    double r_lo = 0.0;
    detail::trig_reduce(x, index, r, r_lo);
    double sin_head = 0.0;
    double sin_tail = 0.0;
    double cos_head = 0.0;
    double cos_tail = 0.0;
    detail::trig_region_rotate(index, r, r_lo, sin_head, sin_tail, cos_head, cos_tail);
    s = sin_head + sin_tail;
    c = cos_head + cos_tail;
}

// tan of a double: math.hpp's contract for tan(double), subnormal rule
// included.
double tan(const double x) noexcept {
    if (!detail::trig_supported(x)) {
        const double flushed = ::stxlib::detail::flush_subnormal(x);
        if (std::fabs(flushed) < detail::sincos_tiny_limit) return flushed;
        if (x != x) return x + x;
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::uint64_t index = 0;
    double r = 0.0;
    double r_lo = 0.0;
    detail::trig_reduce(x, index, r, r_lo);
    detail::Wide numerator{};
    detail::Wide denominator{};
    detail::trig_region_tan_rotate(index, r, r_lo, numerator, denominator);
    const detail::Wide quotient = detail::trig_region_divide(numerator, denominator);
    const double result = quotient.hi + quotient.lo;
    return result;
}

} // namespace region

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
