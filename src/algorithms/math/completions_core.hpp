#pragma once

// The cores of exp2, expm1 and log1p (completions.cpp; roadmap R1). Each
// returns a double-double that the double function rounds once and the
// float function rounds once to float (round_wide_to_float), so both
// precisions come from one computation. Built on the region kernels' proved
// pieces (region_core.hpp): region_exp2 for 2^(p + p_lo) and region_log1p
// for log1p(r + r_lo) with the 10-bit inverse table. Bounds:
// tools/math/r1_certificate.py; design: the seat's
// dev/notes/planning/stages/R1-math-completions.md.
//
// Every operation is a plain IEEE operation or an exact product by
// two_product (fused or Dekker, the same bits), so every variant returns the
// same bits; no multiply_add, no fused path.

#include "region_core.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT::detail {

// expm1's regions. Below 2^-54, e^x - 1 rounds to x. Below 2^-5 a series
// (the exp path's error, relative to e^x, would be multiplied by e^x /
// |e^x - 1| > 32 there). Above 45 the -1 is below 2^-64.9 of e^x and the
// result is e^x. Below -38, e^x < 2^-54.8 and e^x - 1 rounds to -1.
inline constexpr std::uint64_t expm1_tiny_bits = 0x3c90000000000000ULL;    // 2^-54
inline constexpr std::uint64_t expm1_series_bits = 0x3fa0000000000000ULL;  // 2^-5
inline constexpr double expm1_large = 45.0;
inline constexpr double expm1_minus_one = -38.0;

inline constexpr double expm1_7 = 1.0 / 5040.0;
inline constexpr double expm1_8 = 1.0 / 40320.0;
inline constexpr double expm1_9 = 1.0 / 362880.0;

// e^x - 1 = hi + lo for 2^-54 <= |x| < 2^-5. With x^2 = sq + sq_error
// exact and sq/2 exact:
//   e^x - 1 = x + sq/2                                  (head, fast two-sum)
//             + sq_error/2 + x^3 (1/6 + x/24 + ... + x^6/9!)   (low)
// The truncation is below x^10/10! / (1 - |x|), 2^-66.8 of |x|.
inline void expm1_series(const double x, double& hi, double& lo) noexcept {
    double square = 0.0;
    double square_error = 0.0;
    two_product(x, x, square, square_error);
    double head = 0.0;
    double head_error = 0.0;
    fast_two_sum(x, square * 0.5, head, head_error);  // |x| >= x^2/2
    const double cubic =
        (x * square) * (kernel_constants.exp_3 + x * (kernel_constants.exp_4 + x * (kernel_constants.exp_5 + x * (kernel_constants.exp_6 + x * (expm1_7 + x * (expm1_8 + x * expm1_9))))));
    hi = head;
    lo = head_error + (square_error * 0.5 + cubic);
}

// e^x = (head + tail) 2^exponent for |x| <= 745.2, from region_exp2 with
// p + p_lo = x / ln 2: an exact product and the rest (|p_lo| < 2^-41, the
// error of p + p_lo below 2^-104 |x|).
inline void exp_parts(const double x, double& head, double& tail, std::int64_t& exponent) noexcept {
    double p = 0.0;
    double p_error = 0.0;
    two_product(x, region_inverse_ln2_hi, p, p_error);
    const double p_lo = p_error + x * region_inverse_ln2_lo;
    region_exp2(p, p_lo, head, tail, exponent);
}

// e^x - 1 = hi + lo for 2^-5 <= |x| <= 45 and x >= -38: H = head 2^exponent
// (exact: exponent in [-55, 65]) minus 1 by an exact two-sum, the scaled
// tail added to the two-sum's error.
inline void expm1_exp(const double x, double& hi, double& lo) noexcept {
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    exp_parts(x, head, tail, exponent);
    const double scale = power_of_two(exponent);
    const double scaled_head = head * scale;  // exact
    const double scaled_tail = tail * scale;  // exact
    double difference = 0.0;
    double difference_error = 0.0;
    two_sum(scaled_head, -1.0, difference, difference_error);
    hi = difference;
    lo = difference_error + scaled_tail;
}

// log1p's fast path: 2^-54 <= |x| and -1 < x < +infinity. Below 2^-54,
// log1p(x) rounds to x.
inline constexpr std::uint64_t log1p_tiny_bits = 0x3c90000000000000ULL;  // 2^-54

// log1p(x) = hi + lo on the fast path. u + u_lo = 1 + x exactly (two-sum);
// u = 2^k z with z in the region table's bucket of inverse inv, so
//   1 + x = 2^k z (1 + u_lo / u),  z inv (1 + u_lo / u) - 1 = r + r_lo + w
// with r + r_lo = z inv - 1 exactly (region_reduce_r) and w = inv u_lo 2^-k
// (u_lo 2^-k = u_lo z / u). Then
//   log1p(x) = k ln 2 - ln(inv) + log1p(r + r_lo + w).
// w is formed from u_lo times 2^-k, with 2^-k held at 2^-1000 when k > 1000
// (there u_lo = 1 and w < 2^-999, below every rounding here, and the
// clamp keeps the product normal); one rounding (times inv). In bucket 80
// (inv = 1, k = 0) r = z - 1, r_lo = 0 and w = u_lo exactly, so r + w = x
// exactly. A two-sum renormalizes (r, r_lo + w) for region_log1p. The head
// k ln2_hi + logc_hi + a joins by fast two-sums (|k ln2_hi| >= 0.69 >
// |logc_hi| for k != 0; |logc_hi| >= 2 |a| outside bucket 80, where logc_hi
// = 0; certificate `log1p`).
inline void log1p_wide(const double x, double& hi, double& lo) noexcept {
    double u = 0.0;
    double u_lo = 0.0;
    two_sum(1.0, x, u, u_lo);
    const RegionReduction reduction = region_reduce(std::bit_cast<std::uint64_t>(u));
    const RegionLogEntry& entry = region_log_table_128[reduction.index];
    double r_head = 0.0;
    double r_head_lo = 0.0;
    region_reduce_r(reduction.z, entry.inv, r_head, r_head_lo);
    const std::int64_t k_bits = static_cast<std::int64_t>(reduction.k);
    const std::int64_t clamped = k_bits > 1000 ? 1000 : k_bits;
    const double w = (u_lo * power_of_two(-clamped)) * entry.inv;
    double r = 0.0;
    double r_lo = 0.0;
    two_sum(r_head, r_head_lo + w, r, r_lo);
    double a = 0.0;
    double a_lo = 0.0;
    region_log1p(r, r_lo, a, a_lo);
    double b = 0.0;
    double e1 = 0.0;
    fast_two_sum(reduction.k * region_ln2_hi, entry.logc_hi, b, e1);  // k ln2_hi exact
    double c = 0.0;
    double e2 = 0.0;
    fast_two_sum(b, a, c, e2);
    hi = c;
    lo = (e1 + e2) + ((entry.logc_lo + reduction.k * region_ln2_lo) + a_lo);
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT::detail

STXLIB_MATH_TARGET_POP
