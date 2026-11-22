// pow (declared, with its contracts, in stxlib/math.hpp).
//
// x^y = 2^(y * log2 x) for positive finite x, in one pass with no rounding
// test (the owner's log2 formulation, adopted 2026-11-02). One formulation
// on every tier: the exact products are FMA or Dekker's, with the same bits.
//
// The reciprocal grid (adopted 2026-11-22; the owner approved the faster
// pow on 2026-11-18, from the study in projects' Codex outputs of that
// day, stxlib-pow-grid.patch and pow-investigation.md, reimplemented here
// in the house style; PROVENANCE.md names the change of results). With
// x = 2^k M / 2^52, M in [2^52, 2^53), the leading ten fraction bits choose
// a cell of pow_grid_table1024.hpp, holding a short reciprocal n/1024 and
// -log2(n/1024) = head + tail:
//
//   r = (M n - 2^62) / 2^62                      exact (|M n - 2^62| <= 2^53)
//   log2 x = (k + head) + log1p(r) / ln 2 + tail  k + head exact
//   log1p(r) = r - r^2/2 + r^3 (c3 + r^2 (c5 + r^2 c7)),  ci = 1/i - r/(i + 1)
//
// with |r| <= 0x1.35cp-10. r - r^2/2 is kept as a + a_error exactly (r^2
// from an exact product); the terms r^3 to r^8 come from three independent
// coefficient pairs, truncated at r^9/9 < 2^-90. log2 x is normalized to
// log_hi + log_lo before the one exact product with y:
//
//   p + p_lo = y log_hi + y log_lo
//
// and 2^(p + p_lo) = 2^e 2^(j/256) e^u, with q = round(256 p) = 256 e + j,
// t = p - q/256 exact plus p_lo, u = t ln 2 (|u| <= 2^-9.5), from the exp
// table and e^u - 1 - u to the u^5 term. Against the log2-table kernel it
// replaces: the reduction needs no exact product, the logarithm's
// polynomial is shorter (|r| <= 2^-9.6 against 2^-8), and the exp side
// takes one rounded t; the 1024 cells cost 24 KiB.
//
// The same bits need every exact product inside the range where both forms
// are exact (kernel_support.hpp two_product; float_exact's
// in_product_range: each factor zero or of magnitude in [2^-969, 2^995],
// exponents summing to at least -918). The factors are r (twice), a, y,
// log_hi, a table head T and u; the kernel takes only 2^-80 <= |y| < 2^64
// (outside it the result is decided without the kernel: pow_y_kernel_low,
// pow_y_kernel_high). Every other factor is zero or between 2^-788 and
// 2^11, by grids: a rounded sum of values on grids of powers of two lies on
// the finer grid, a rounded product on the product of the grids, and a
// nonzero value on grid g is at least g. r lies on 2^-62; r^2, its error,
// a and a_error on 2^-125; the coefficient pairs (constants on 2^-55 or
// coarser, times r) on 2^-117; the r^3 polynomial on 2^-551; tail on 2^-122
// (zero or at least 2^-70, checked by the generator); k + head on 2^-41;
// so low, log_hi and log_lo lie on 2^-603, y (|y| >= 2^-80) on 2^-132,
// p_lo and t on 2^-735 and u (ln2_hi on 2^-53) on 2^-788. Products of zero
// are exact in both forms.
//
// Error bound (tools/math/pow_grid_certificate.py, exact rational upper
// bounds per cell): the relative error of log2 x is below 9.71e-22, which p
// carries times |y log2 x| <= 1025 for a finite normal result; the exp side
// adds 3.58e-19 relative. In all, within 0.5095 ulp for normal results,
// before review of the correspondence between the certificate's model and
// this code (argued, not machine checked). The study measured 0.501712 ulp
// at worst against MPFR on 2.45 million pairs (Apple M4, 2026-11-18).
// Subnormal results round once on the subnormal grid (scale_wide). Negative
// x with integer y takes the sign of the integer's parity; every other
// special input follows C99 Annex F.

#include "stxlib/math.hpp"

#include "exp_core.hpp"
#include "log_core.hpp"
#include "log_table128.hpp"
#include "pow_grid_table1024.hpp"

#include <limits>

#include "variant_functions.hpp"

#include "paths.hpp"
#include "region_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

enum class IntegerKind : std::uint8_t { not_integer, odd, even };

constexpr std::uint64_t mantissa_mask = 0x000fffffffffffffULL;
constexpr std::uint64_t implicit_bit = 0x0010000000000000ULL;

// Whether finite y is an integer and, if so, its parity. |y| >= 2^53 is
// always an even integer; 0 < |y| < 1 is never an integer.
IntegerKind integer_kind(const double y) noexcept {
    if (y == 0.0) return IntegerKind::even;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(y);
    const std::int64_t exponent = static_cast<std::int64_t>((bits >> 52) & 0x7ff) - exponent_bias;
    if (exponent < 0) return IntegerKind::not_integer;
    if (exponent > 52) return IntegerKind::even;
    const std::uint64_t significand = (bits & mantissa_mask) | implicit_bit;
    const std::uint64_t fraction_bits = 52 - static_cast<std::uint64_t>(exponent);
    const std::uint64_t fraction_mask = (std::uint64_t{1} << fraction_bits) - 1;
    if ((significand & fraction_mask) != 0) return IntegerKind::not_integer;
    const bool odd = ((significand >> fraction_bits) & 1) != 0;
    if (odd) return IntegerKind::odd;
    return IntegerKind::even;
}

// 2^r = 1 + c1 r + c2 r^2 + c3 r^3 + ...: ln2, ln2^2/2, ln2^3/6.
constexpr double pow2_c1 = 0.6931471805599453;
constexpr double pow2_c2 = 0.2402265069591007;
constexpr double pow2_c3 = 0.05550410866482158;

// The pow limits, in log2 units. Above 1024.5 the result is at least
// 2^1024.49 and rounds to infinity; below -1076 it is under 2^-1075.99 and
// rounds to +0. Between them the exponent e = floor(q / 256) lies in
// [-1077, 1024], inside scale_wide's range: e = 1024 scales in two steps
// (an overflow rounds once), and results below 2^-1022 round once on the
// subnormal grid. With |y| < 2^64 and |log_hi| < 1077, y log_hi stays
// below 2^75 and p is finite.
constexpr double pow_overflow_limit = 1024.5;
constexpr double pow_underflow_limit = -1076.0;

// The kernel's y range (bits of |y|): 2^-80 <= |y| < 2^64, where every
// exact product is in range for both product forms (head of this file).
// Outside it, for positive finite x != 1 (normal or subnormal):
// - |y| >= 2^64: |log2 x| >= log2(1 + 2^-52) > 2^-53 for x != 1, so
//   |y log2 x| > 2^11 > 1076 and the result is +infinity or +0 (overflow
//   or underflow, rounded to nearest), as for infinite y. Such y is an even
//   integer (|y| >= 2^53), so negative x and zero or infinite x give the
//   same results as infinite y too (C99 Annex F);
// - 0 < |y| < 2^-80: |y log2 x| < 2^-80 * 1075 < 2^-69, so x^y lies within
//   2^-69 of 1 and rounds to 1 (the neighbours of 1 are 1 - 2^-53 and
//   1 + 2^-52). Negative finite x has no real power there (0 < |y| < 1 is
//   not an integer): NaN.
constexpr std::uint64_t pow_y_kernel_low = 0x3af0000000000000ULL;   // 2^-80
constexpr std::uint64_t pow_y_kernel_high = 0x43f0000000000000ULL;  // 2^64

// pow_grid_table1024.hpp's facts, checked at compile time (the generator
// checks them too): cell 0 is unity and the last cell reduces against 2,
// every head is a multiple of 2^-41 in [0, 1], every tail is zero or at
// least 2^-70 (the grid argument at the head of this file), and for both
// ends of every cell 0 < M n < 2^63 and |M n - 2^62| <= 2^53 (M n - 2^62 is
// linear in M, so the ends are its extremes).
constexpr int pow_grid_cell_count = 1024;
constexpr int pow_grid_cell_shift = 42;
constexpr std::int64_t pow_grid_unity = std::int64_t{1} << 62;
constexpr std::int64_t pow_grid_residual_limit = std::int64_t{1} << 53;

constexpr bool pow_grid_cell_valid(const int index) noexcept {
    const PowGridCell& cell = pow_grid_cells[index];
    const std::int64_t lowest = static_cast<std::int64_t>(pow_grid_cell_count + index) << pow_grid_cell_shift;
    const std::int64_t highest =
        (static_cast<std::int64_t>(pow_grid_cell_count + index + 1) << pow_grid_cell_shift) - 1;
    // n <= 1024 and M < 2^53 keep both products below 2^63 before the test.
    if (cell.numerator < 1 || cell.numerator > pow_grid_cell_count) return false;
    const std::int64_t low_residual = lowest * cell.numerator - pow_grid_unity;
    const std::int64_t high_residual = highest * cell.numerator - pow_grid_unity;
    if (low_residual < -pow_grid_residual_limit || low_residual > pow_grid_residual_limit) return false;
    if (high_residual < -pow_grid_residual_limit || high_residual > pow_grid_residual_limit) return false;
    if (cell.head < 0.0 || cell.head > 1.0) return false;
    const double head_units = cell.head * 0x1p41;
    if (head_units != static_cast<double>(static_cast<std::int64_t>(head_units))) return false;
    // Spelled as two comparisons: std::fabs is not constexpr in C++20.
    if (cell.tail != 0.0 && cell.tail < 0x1p-70 && cell.tail > -0x1p-70) return false;
    return true;
}

constexpr bool pow_grid_table_valid() noexcept {
    if (pow_grid_cells[0].numerator != pow_grid_cell_count || pow_grid_cells[0].head != 0.0 ||
        pow_grid_cells[0].tail != 0.0) {
        return false;
    }
    const PowGridCell& last = pow_grid_cells[pow_grid_cell_count - 1];
    if (last.numerator != pow_grid_cell_count / 2 || last.head != 1.0 || last.tail != 0.0) return false;
    for (int index = 0; index < pow_grid_cell_count; ++index) {
        if (!pow_grid_cell_valid(index)) return false;
    }
    return true;
}

static_assert(pow_grid_table_valid(), "pow_grid_table1024.hpp breaks a fact pow_positive relies on");

// x^y for positive finite x (normal or subnormal) and 2^-80 <= |y| < 2^64
// (pow_y_kernel_low, pow_y_kernel_high). The head of this file gives the
// steps and their exactness.
double pow_positive(const double x, const double y) noexcept {
    double scaled = x;
    std::int64_t exponent_adjust = 0;
    if (x < smallest_normal) {
        scaled = x * 0x1p52;  // subnormal: exact scaling into the normal range
        exponent_adjust = 52;
    }
    // x = 2^k M / 2^52; the cell from the leading ten fraction bits.
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(scaled);
    const std::uint64_t fraction = bits & mantissa_mask;
    const std::int64_t significand = static_cast<std::int64_t>(fraction | implicit_bit);
    const std::int64_t k = static_cast<std::int64_t>(bits >> 52) - exponent_bias - exponent_adjust;
    const PowGridCell& cell = pow_grid_cells[fraction >> pow_grid_cell_shift];
    // r = (M n - 2^62) / 2^62: the product is below 2^63 and the centred
    // difference at most 2^53 (pow_grid_table_valid), so both are exact.
    const std::int64_t residual = significand * cell.numerator - pow_grid_unity;
    const double r = static_cast<double>(residual) * 0x1p-62;
    const double whole = static_cast<double>(k) + cell.head;  // exact
    // log1p(r) = a + a_tail: r - r^2/2 exactly, the rest from r^3 to r^8.
    double r_squared = 0.0;
    double r_squared_error = 0.0;
    two_product(r, r, r_squared, r_squared_error);
    double a = 0.0;
    double a_error = 0.0;
    fast_two_sum(r, -0.5 * r_squared, a, a_error);  // |r| >= r^2/2
    const double pair_3 = kernel_constants.log_3 + r * kernel_constants.log_4;
    const double pair_5 = kernel_constants.log_5 + r * kernel_constants.log_6;
    const double pair_7 = kernel_constants.log_7 + r * kernel_constants.log_8;
    const double cubic = (r * r_squared) * (pair_3 + r_squared * (pair_5 + r_squared * pair_7));
    const double a_tail = a_error + (cubic - 0.5 * r_squared_error);
    // log2 x = whole + (a + a_tail) / ln 2 + tail, normalized to log_hi + log_lo.
    double s = 0.0;
    double s_error = 0.0;
    two_product(a, kernel_constants.inverse_ln2_hi, s, s_error);
    double h = 0.0;
    double h_error = 0.0;
    two_sum(whole, s, h, h_error);
    const double scaled_tail = a_tail * kernel_constants.inverse_ln2_hi + a * kernel_constants.inverse_ln2_lo;
    const double low = (scaled_tail + cell.tail) + (s_error + h_error);
    double log_hi = 0.0;
    double log_lo = 0.0;
    two_sum(h, low, log_hi, log_lo);
    // p + p_lo = y log2 x.
    double p = 0.0;
    double p_error = 0.0;
    two_product(y, log_hi, p, p_error);
    if (p > pow_overflow_limit) return std::numeric_limits<double>::infinity();
    if (p < pow_underflow_limit) return 0.0;
    const double p_lo = p_error + y * log_lo;
    // q = round(256 p) = 256 e + j; t = p + p_lo - q/256, one rounding.
    const double shifted = p * 256.0 + kernel_constants.rounding_shift;
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(kernel_constants.rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & exp_index_mask;
    const std::int64_t exponent = q_bits >> 8;  // floor(q / 256): an arithmetic shift in C++20
    const double t_head = p - (shifted - kernel_constants.rounding_shift) * (1.0 / 256.0);  // exact
    const double t = t_head + p_lo;
    // u = t ln 2; e^u - 1 - u to the u^5 term (truncation below u^6/720).
    const double u = t * kernel_constants.ln2_hi;
    const double u_squared = u * u;
    const double higher = u_squared * ((exp_2 + u * kernel_constants.exp_3) +
                                       u_squared * (kernel_constants.exp_4 + u * kernel_constants.exp_5));
    // 2^(j/256) e^u = T_hi + T_hi u + [T_lo (1 + u) + T_hi (e^u - 1 - u)].
    const ExpEntry& power = exp_table_256[j];
    double rotation = 0.0;
    double rotation_error = 0.0;
    two_product(power.hi, u, rotation, rotation_error);
    double head = 0.0;
    double head_error = 0.0;
    fast_two_sum(power.hi, rotation, head, head_error);  // |T| >= 1 > |T u|
    const double tail = head_error + (rotation_error + (power.lo * (1.0 + u) + power.hi * higher));
    const double result = scale_wide(head, tail, exponent);
    return result;
}

// Float pow's cheap kernel: x^y = 2^(y log2 x) in plain double for x
// positive normal and y finite nonzero. log2 x = k + log z / ln 2 from the
// log table (polynomial to r^5: truncation below 2^-50 absolute); p = y
// log2 x; 2^p = 2^(kk) 2^(j/256) 2^r2 with r2 = p - q/256 exact (|r2| <=
// 1/512) and a cubic for 2^r2 (truncation below 2^-42.7 relative). |log2 x|
// errs by at most (1 + |log2 x|) 2^-48 (seven roundings of terms below
// 1 + |log2 x|, with margin), which p carries times |y|, and 2^p turns an
// absolute error d in p into a relative error d ln 2. Returns false when
// |p| > 150 (results beyond float's range) or the rounding is uncertain.
// Modular by design: the log table offset arithmetic (see log_core.hpp).
STXLIB_MODULAR bool pow_float_cheap(const double x, const double y, float& result) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t offset = bits - log_offset_bits;
    const std::uint64_t index = (offset >> 45) & log_index_mask;
    const std::int64_t k_bits = static_cast<std::int64_t>(offset) >> 52;
    const double z = std::bit_cast<double>(bits - (offset & exponent_field));
    const LogEntry& entry = log_table_128[index];
    const double r = multiply_add(z, entry.inv, -1.0);
    // Estrin's split: two short chains instead of one long one.
    const double r_squared = r * r;
    const double low_terms = multiply_add(r, kernel_constants.log_3, -0.5);
    const double high_terms = multiply_add(r, kernel_constants.log_5, kernel_constants.log_4);
    const double log1p_r = multiply_add(r_squared, multiply_add(r_squared, high_terms, low_terms), r);
    const double log2_x = multiply_add(entry.logc_hi + log1p_r, log_inv_ln2_hi, static_cast<double>(k_bits));
    const double p = y * log2_x;
    if (!(std::fabs(p) <= 150.0)) return false;
    // q = round(256 p), j = q mod 256, kk = floor(q / 256).
    const double shifted = multiply_add(p, 256.0, rounding_shift);
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & 255;
    const std::int64_t kk = q_bits >> 8;
    const double r2 = p - (shifted - rounding_shift) * (1.0 / 256.0);  // exact
    const double two_r2 = multiply_add(r2, multiply_add(r2, multiply_add(r2, pow2_c3, pow2_c2), pow2_c1), 1.0);
    const double value = (exp_table_256[j].hi * two_r2) * power_of_two(kk);
    const double p_error = std::fabs(y) * ((1.0 + std::fabs(log2_x)) * 0x1p-48) + std::fabs(p) * 0x1p-52;
    const double relative = p_error * 0.7 + 0x1p-42;
    const bool certain = float_if_certain(value, value * relative, result);
    return certain;
}

// Inputs where x is zero or infinite (y finite or not, neither NaN).
double pow_zero_or_infinite_base(const double x, const double y) noexcept {
    const double infinity = std::numeric_limits<double>::infinity();
    const bool odd = std::isfinite(y) && integer_kind(y) == IntegerKind::odd;
    const bool negative_base = std::signbit(x);
    double magnitude = 0.0;
    // 0^y is infinite for y < 0; infinity^y is infinite for y > 0.
    if ((x == 0.0) == (y < 0.0)) magnitude = infinity;
    if (negative_base && odd) return -magnitude;
    return magnitude;
}

} // namespace
} // namespace detail

namespace today {

double pow(const double x, const double y) noexcept {
    // The common case in one test: x positive, normal and finite; y in the
    // kernel's range (2^-80 <= |y| < 2^64).
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(y) & detail::magnitude_mask;
    if (detail::log_fast_path(x_bits) && y_magnitude >= detail::pow_y_kernel_low &&
        y_magnitude < detail::pow_y_kernel_high) {
        const double result = detail::pow_positive(x, y);
        return result;
    }
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    const double infinity = std::numeric_limits<double>::infinity();
    // Infinite y, and finite |y| >= 2^64 (an even integer whose power of
    // any |x| != 1 overflows or underflows: pow_y_kernel_high).
    if (y_magnitude >= detail::pow_y_kernel_high) {
        const double magnitude = std::fabs(x);
        if (magnitude == 1.0) return 1.0;
        if ((magnitude < 1.0) == (y > 0.0)) return 0.0;
        return infinity;
    }
    if (x == 0.0 || std::isinf(x)) return detail::pow_zero_or_infinite_base(x, y);
    // 0 < |y| < 2^-80: x^y rounds to 1 for positive finite x; negative x
    // has no real power (pow_y_kernel_low).
    if (y_magnitude < detail::pow_y_kernel_low) {
        if (x > 0.0) return 1.0;
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (x > 0.0) return detail::pow_positive(x, y);
    // Negative finite x: defined only for integer y, with the parity's sign.
    const detail::IntegerKind kind = detail::integer_kind(y);
    if (kind == detail::IntegerKind::not_integer) return std::numeric_limits<double>::quiet_NaN();
    const double magnitude = detail::pow_positive(-x, y);
    if (kind == detail::IntegerKind::odd) return -magnitude;
    return magnitude;
}

} // namespace today

namespace detail {
namespace {

// The accurate float power: the double result rounded once to float (which
// also rounds float overflow and underflow correctly), out of line so the
// cheap path's tail call needs no stack frame.
// Today's double kernel by name: the double switch must not reach this path.
STXLIB_MATH_COLD float pow_float_accurate(const float x, const float y) noexcept {
    const double result = today::pow(detail::widen(x), detail::widen(y));
    return static_cast<float>(result);
}

} // namespace
} // namespace detail

// Float pow: first 2^(y log2 x) in plain double (pow_float_cheap),
// certified with float_if_certain; otherwise the double result rounded once
// (pow_float_accurate).
namespace today {

float pow(const float x, const float y) noexcept {
    const double xd = detail::widen(x);
    const double yd = detail::widen(y);
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(xd);
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(yd) & detail::magnitude_mask;
    if (detail::log_fast_path(x_bits) && y_magnitude != 0 && y_magnitude < detail::infinity_bits) {
        float fast = 0.0f;
        if (detail::pow_float_cheap(xd, yd, fast)) return fast;
    }
    const float result = detail::pow_float_accurate(x, y);
    return result;
}

} // namespace today

// The variant functions: the path paths.hpp's switches choose. The region
// path serves arguments that are not subnormal and results it does not
// flush (a region zero from nonzero arguments is a flushed underflow);
// today's kernel serves the rest, so the unflushed contract holds on both.
double pow(const double x, const double y) noexcept {
#if STXLIB_MATH_REGION_POW_DOUBLE
    if (paths::not_subnormal(x) && paths::not_subnormal(y)) {
        const double region_result = region::pow(x, y);
        if (region_result != 0.0) return region_result;
    }
#endif
    const double result = today::pow(x, y);
    return result;
}

float pow(const float x, const float y) noexcept {
#if STXLIB_MATH_REGION_POW_FLOAT
    if (paths::not_subnormal(x) && paths::not_subnormal(y)) {
        const float region_result = region::pow(x, y);
        if (region_result != 0.0f) return region_result;
    }
#endif
    const float result = today::pow(x, y);
    return result;
}

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
