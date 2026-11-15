#pragma once

// The region kernels' shared core (region.cpp). Private to the library; see
// kernel_support.hpp for the floating-point rules. An experiment kept apart
// from the library's kernels (pow.cpp, log.cpp, ...) until the owner
// adopts it: nothing in the library calls it; tests/math_region_tests.cpp
// compares it with them. Design and measurements: the seat's
// dev/notes/planning/stages/M-region-kernels.md; bounds:
// tools/math/region_certificate.py.
//
// Where the current kernels spend their error. pow(double)'s bound, 0.5 +
// 0.076 ulp, comes from three places that the certificate counts for the
// current kernel too (`region_certificate.py pow`):
//   - log2 x is good to 2^-66.75 of itself at worst (x near 1, buckets 80
//     and 81), and |y| multiplies that absolute error up to |p| = 1025:
//     the cubic term of log1p(r) (up to 2^-25.6) is rounded in plain double
//     and then carried in the low part a_lo, whose sums round at 2^-78;
//   - y * s_lo rounds at 2^-60 absolute, because s_lo carries that cubic
//     term too (|s_lo| up to 2^-25);
//   - 2^t is good to 2^-69 (u^2/2 rounded in plain double).
// The region kernels change the representation, not the cost profile: the
// same single pass, no test, no fallback.
//   - Every term of log1p(r) above 2^-32 is formed exactly (cut products:
//     r1 = r cut to 13 bits makes r1^2, r1 r2 and r1^3 * third_hi exact) and
//     joins the head by fast two-sums, so the low part holds only terms
//     below 2^-32 and is normalized away before y multiplies it.
//   - z inv - 1 is formed exactly without an exact product: inv has 10
//     bits (region_log_table128.hpp), z is cut to 43 bits, and both partial
//     products are exact.
//   - In 2^t, u = t ln 2 and u^2/2 are formed from 26-bit cuts, so the
//     terms above 2^-33 are exact and the tail holds the rest.
// Proved bound (tools/math/region_certificate.py): pow(double) within
// 0.500097 ulp (2^-66.3 relative before the final rounding), against 0.5569
// for the current kernel by the same accounting (0.576 as pow.cpp states).
//
// Every operation here is a plain IEEE operation or an exact product by
// two_product (fused or Dekker, the same bits), so every variant returns the
// same bits; no multiply_add.

#include "constants.hpp"
#include "exp_core.hpp"
#include "kernel_support.hpp"
#include "log_core.hpp"
#include "region_log_table128.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT::detail {

// A double cut (toward zero) to its leading 13 or 43 significant bits.
inline constexpr std::uint64_t cut_13_mask = ~((std::uint64_t{1} << 40) - 1);
inline constexpr std::uint64_t cut_43_mask = ~((std::uint64_t{1} << 10) - 1);

inline double cut_13(const double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value) & cut_13_mask;
    const double result = std::bit_cast<double>(bits);
    return result;
}

inline double cut_43(const double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value) & cut_43_mask;
    const double result = std::bit_cast<double>(bits);
    return result;
}

// The log1p coefficients from r^4 on, and 1/3 (the nearest doubles).
inline constexpr double region_third = 1.0 / 3.0;
inline constexpr double region_exp_7 = 1.0 / 5040.0;

// x = 2^k z for a positive normal double, z in [0x1.5fp-1, 0x1.5fp0), and
// the bucket of z (log_core.hpp's offsets). Modular by design, as log_z.
struct RegionReduction final {
    double k{0.0};
    std::uint64_t index{0};
    double z{0.0};
};

STXLIB_MODULAR inline RegionReduction region_reduce(const std::uint64_t bits) noexcept {
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const RegionReduction result{static_cast<double>(k_bits), index, z};
    return result;
}

// r + r_lo = z inv - 1 exactly, with |r_lo| <= ulp(r)/2. inv has at most 10
// significant bits and z_hi (z cut to 43 bits) times inv has at most 53, so
// the product is exact and lies within 2^-7.87 of 1 (the table's max |z inv
// - 1|), which makes the subtraction exact (Sterbenz). z_lo = z - z_hi has
// at most 10 bits, so z_lo * inv is exact too. The two-sum normalizes: in
// bucket 80 (inv = 1) z_hi - 1 and z_lo can nearly cancel, and the sum
// keeps r relative to z - 1.
inline void region_reduce_r(const double z, const double inv, double& r, double& r_lo) noexcept {
    const double z_hi = cut_43(z);
    const double z_lo = z - z_hi;  // exact
    const double r_head = z_hi * inv - 1.0;  // exact
    const double r_tail = z_lo * inv;  // exact
    two_sum(r_head, r_tail, r, r_lo);
}

// log1p(r + r_lo) = a + a_lo for |r| < 2^-7.87 and |r_lo| <= ulp(r)/2, with
// |a_lo| <= ulp(a)/2 and an error below 2^-75 of |log z| for z outside
// bucket 80 and of |r| inside it (tools/math/region_certificate.py).
//
// With r1 = r cut to 13 bits and r2 = r - r1 (exact, |r2| < 2^-12 |r|, at
// most 40 bits):
//   log1p(r) = r - r1^2/2 - r1 r2 + r1^3 third_hi            (head, exact terms)
//              - r2^2/2 + r1^3 third_lo + r2 (r1^2 + r1 r2 + r2^2/3)
//              + r^4 (-1/4 + r/5 - ... - r^6/10)              (low, below 2^-32)
// and log1p(r + r_lo) - log1p(r) = r_lo (1 - r + r^2 - r^3) to r_lo r^4.
// square1 = r1^2 (26 bits), cross = r1 r2 (53 bits), cube1 = r1^3 (39 bits)
// and cube1 * third_hi (third_hi has 14 bits) are exact. Each head term
// joins by a fast two-sum: |r| >= r1^2/2, and every later term is smaller
// than the partial sum (the certificate checks each precondition).
inline void region_log1p(const double r, const double r_lo, double& a, double& a_lo) noexcept {
    const double r1 = cut_13(r);
    const double r2 = r - r1;  // exact
    const double square1 = r1 * r1;  // exact
    const double cross = r1 * r2;  // exact
    const double cube1 = square1 * r1;  // exact
    const double cubic_head = cube1 * region_third_hi;  // exact
    double h = 0.0;
    double e1 = 0.0;
    fast_two_sum(r, -(square1 * 0.5), h, e1);
    double e2 = 0.0;
    fast_two_sum(h, -cross, h, e2);
    double e3 = 0.0;
    fast_two_sum(h, cubic_head, h, e3);
    const double cubic_rest = cube1 * region_third_lo + r2 * (square1 + (cross + (r2 * r2) * region_third));
    const double square_rest = 0.5 * (r2 * r2);
    const double r_squared = r * r;
    const double quartic =
        (r_squared * r_squared) * (kernel_constants.log_4 + r * (kernel_constants.log_5 + r * (kernel_constants.log_6 + r * (kernel_constants.log_7 + r * (kernel_constants.log_8 + r * (kernel_constants.log_9 + r * kernel_constants.log_10))))));
    const double linear_lo = r_lo * (1.0 - r * (1.0 - r * (1.0 - r)));
    const double low = ((e1 + e2) + e3) + (((cubic_rest - square_rest) + quartic) + linear_lo);
    fast_two_sum(h, low, a, a_lo);
}

// log2 x for pow: whole + s + s_lo, whole = k + log2c_hi exact (log2c_hi a
// multiple of 2^-41, |k| < 2^11), |s_lo| <= 2^-42 (the table's log2c_lo plus
// roundings). For a positive normal double's bits.
inline void region_log2_parts(const std::uint64_t bits, double& whole, double& s, double& s_lo) noexcept {
    const RegionReduction reduction = region_reduce(bits);
    const RegionLogEntry& entry = region_log_table_128[reduction.index];
    double r = 0.0;
    double r_lo = 0.0;
    region_reduce_r(reduction.z, entry.inv, r, r_lo);
    double a = 0.0;
    double a_lo = 0.0;
    region_log1p(r, r_lo, a, a_lo);
    double s_error = 0.0;
    two_product(a, region_inverse_ln2_hi, s, s_error);
    s_lo = s_error + ((a * region_inverse_ln2_lo + a_lo * region_inverse_ln2_hi) + entry.log2c_lo);
    whole = reduction.k + entry.log2c_hi;  // exact
}

// 2^(p + p_lo) = (head + tail) 2^exponent for |p| <= 1076 and |p_lo| <=
// 2^-40, with head in [0.998, 2.003) and an error below 2^-79.9 of the result
// (before scaling; tools/math/region_certificate.py).
//
// q = round(256 p) = 256 exponent + j, t + t_lo = p + p_lo - q/256 (|t| <=
// 2^-9 + 2^-40), T = 2^(j/256) = T_hi + T_lo (exp_table_256) cut as exp_core
// cuts it (c1 = cut_26(T_hi), c2 = (T_hi - c1) + T_lo). u + u_lo = (t +
// t_lo) ln 2 from the 26-bit cuts of t and ln 2: t1 * ln2_1 is exact and the
// cross terms are below 2^-34. Then with u1 = cut_26(u), u2 = u - u1 and w1
// = cut_26(u1^2/2), w2 = u^2/2 - w1:
//   T e^u = T_hi + c1 u1 + c1 w1                                   (head)
//           + [c1 u2 + c2 u] + [c1 w2 + c2 u^2/2]
//           + T_hi (u^3/6 + ... + u^7/5040 + u_lo (1 + u)) + T_lo   (tail)
// c1 u1 and c1 w1 are exact (26 + 26 bits) and join T_hi by fast two-sums
// (T_hi >= 1 > |c1 u1|).
inline void region_exp2(const double p, const double p_lo, double& head, double& tail,
                        std::int64_t& exponent) noexcept {
    const double shifted = p * 256.0 + kernel_constants.rounding_shift;
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(kernel_constants.rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & exp_index_mask;
    exponent = q_bits >> 8;  // floor(q / 256): an arithmetic shift in C++20
    const double t_head = p - (shifted - kernel_constants.rounding_shift) * (1.0 / 256.0);  // exact
    double t = 0.0;
    double t_lo = 0.0;
    two_sum(t_head, p_lo, t, t_lo);
    const double t1 = cut_26(t);
    const double t2 = t - t1;  // exact
    const double u_head = t1 * region_ln2_1;  // exact
    const double u_cross = (t1 * region_ln2_2 + t2 * region_ln2_1) + (t2 * region_ln2_2 + (t * region_ln2_3 + t_lo * region_ln2_1));
    double u = 0.0;
    double u_lo = 0.0;
    fast_two_sum(u_head, u_cross, u, u_lo);
    const double u1 = cut_26(u);
    const double u2 = u - u1;  // exact
    const double half_square1 = (u1 * u1) * 0.5;  // exact
    const double w1 = cut_26(half_square1);
    const double w2 = ((half_square1 - w1) + u1 * u2) + 0.5 * (u2 * u2);
    const double u_squared = u * u;
    const double cubic =
        (u * u_squared) * (kernel_constants.exp_3 + u * (kernel_constants.exp_4 + u * (kernel_constants.exp_5 + u * (kernel_constants.exp_6 + u * region_exp_7))));
    const ExpEntry& power = exp_table_256[j];
    const ExpCut& cut = exp_cuts.values[j];
    double f1 = 0.0;
    fast_two_sum(power.hi, cut.t1 * u1, head, f1);
    double f2 = 0.0;
    fast_two_sum(head, cut.t1 * w1, head, f2);
    const double rotation_rest = cut.t1 * u2 + cut.t2 * u;
    const double square_rest = cut.t1 * w2 + cut.t2 * (0.5 * u_squared);
    const double small = power.hi * (cubic + u_lo * (1.0 + u)) + power.lo;
    tail = (f1 + f2) + ((rotation_rest + square_rest) + small);
}

// (head + tail) 2^exponent for head in [0.998, 2.003), |tail| < 2^-40 and exponent
// in [-1023, 1024], rounded once to double, with STXlib's subnormal rule: a
// result below 2^-1022 is +0. y = head + tail is the rounded significand
// (in [0.998, 2.003]); y 2^e1 with e1 = exponent >> 1 is exact, and the
// second scaling by 2^(exponent - e1) rounds only on overflow (to infinity,
// once, from the rounded y) or below 2^-1022 (then flushed). No branch on
// the exponent.
inline double region_scale_flush(const double head, const double tail, const std::int64_t exponent) noexcept {
    const double y = head + tail;
    const std::int64_t first = exponent >> 1;
    const double scaled = (y * power_of_two(first)) * power_of_two(exponent - first);
    const double result = scaled < smallest_normal ? 0.0 : scaled;
    return result;
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT::detail

STXLIB_MATH_TARGET_POP
