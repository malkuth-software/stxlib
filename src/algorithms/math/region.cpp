// The region kernels (declared in region_functions.hpp): flat single-pass
// forms of pow (double and float), the float logarithms and float hypot,
// each with the public contract of stxlib/math.hpp, the subnormal rule
// included (applied here directly: no flush.cpp wrapper is needed). An
// experiment kept apart from the library's kernels until the owner adopts
// it; tests/math_region_tests.cpp compiles this file as its own native
// variant and compares it with the library's functions. Design and
// measurements: the seat's dev/notes/planning/stages/M-region-kernels.md.
//
// "Flat": one pass per input class, no rounding test with an accurate
// fallback, no cold path for hard inputs. Float pow is the exception
// (decided 2026-11-15): no flat float pow is proved correctly rounded, so
// it rounds, tests and decides. The float functions the library computes
// as "cheap path, certify, else the accurate path" (log, log2,
// log10, pow, hypot) are computed here accurately enough in one pass:
//   - log, log2, log10 (float): a double-double good to 2^-62 of the
//     result, rounded once (round to odd, then to float). Correctly rounded
//     for every float: tools/math/region_exhaustive.py screens all 2^31
//     positive floats against the proved bound and decides the few it
//     cannot screen from the exact reference.
//   - hypot (float): sqrt of the exactly formed a^2 + b^2, rounded to
//     float, then the rounding decided exactly against the midpoint m
//     (m^2 is exact, so the sign of a^2 + b^2 - m^2 is exact): correctly
//     rounded for every pair, by construction, ties to even included.
//   - pow (float): round, test, decide (see "float pow" below): the region
//     double kernel rounded once unless it lies near a float midpoint, then
//     exact results and midpoints detected exactly, else an accurate
//     double-double stage of fixed precision (2^-94) rounded once.
//   - pow (double): the region kernel of region_core.hpp, within 0.500097 ulp.

#include "region_core.hpp"

#include "core/subnormal.hpp"

#include <limits>

#include "region_functions.hpp"

STXLIB_MATH_TARGET_PUSH

namespace stxlib::math::STXLIB_MATH_VARIANT {

namespace detail {
namespace {

enum class RegionIntegerKind : std::uint8_t { not_integer, odd, even };

constexpr std::uint64_t region_mantissa_mask = 0x000fffffffffffffULL;
constexpr std::uint64_t region_implicit_bit = 0x0010000000000000ULL;

// Whether finite y is an integer and, if so, its parity (pow.cpp's
// integer_kind, repeated here so the experiment touches no library file).
RegionIntegerKind region_integer_kind(const double y) noexcept {
    if (y == 0.0) return RegionIntegerKind::even;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(y);
    const std::int64_t exponent = static_cast<std::int64_t>((bits >> 52) & 0x7ff) - exponent_bias;
    if (exponent < 0) return RegionIntegerKind::not_integer;
    if (exponent > 52) return RegionIntegerKind::even;
    const std::uint64_t significand = (bits & region_mantissa_mask) | region_implicit_bit;
    const std::uint64_t fraction_bits = 52 - static_cast<std::uint64_t>(exponent);
    const std::uint64_t fraction_mask = (std::uint64_t{1} << fraction_bits) - 1;
    if ((significand & fraction_mask) != 0) return RegionIntegerKind::not_integer;
    const bool odd = ((significand >> fraction_bits) & 1) != 0;
    if (odd) return RegionIntegerKind::odd;
    return RegionIntegerKind::even;
}

// The kernel's y range, as pow.cpp's (2^-80 <= |y| < 2^64): every exact
// product's factors are in range for both product forms, and outside it
// the result is decided without the kernel (pow.cpp, pow_y_kernel_low and
// pow_y_kernel_high, gives the argument).
constexpr std::uint64_t region_y_kernel_low = 0x3af0000000000000ULL;   // 2^-80
constexpr std::uint64_t region_y_kernel_high = 0x43f0000000000000ULL;  // 2^64

// Above 1024.5 (log2 units) x^y rounds to infinity; below -1023 it is under
// 2^-1023 (times 1 + 2^-65), below the smallest normal, so +0 by the
// subnormal rule. Between them the exponent is in [-1023, 1024]
// (region_scale_flush).
constexpr double region_overflow_limit = 1024.5;
constexpr double region_underflow_limit = -1023.0;

// p + p_lo = y log2 x from log2 x = whole + s + s_lo: two exact products
// and an exact sum; |p_lo| <= 2^-40 for |p| <= 1076.
struct RegionPower final {
    double p{0.0};
    double p_lo{0.0};
};

RegionPower region_power(const std::uint64_t x_bits, const double y) noexcept {
    double whole = 0.0;
    double s = 0.0;
    double s_lo = 0.0;
    region_log2_parts(x_bits, whole, s, s_lo);
    double big = 0.0;
    double big_error = 0.0;
    two_product(y, whole, big, big_error);
    double small = 0.0;
    double small_error = 0.0;
    two_product(y, s, small, small_error);
    double p = 0.0;
    double p_error = 0.0;
    two_sum(big, small, p, p_error);
    const double p_lo = p_error + ((big_error + small_error) + y * s_lo);
    const RegionPower result{p, p_lo};
    return result;
}

// x^y for positive normal x and 2^-80 <= |y| < 2^64. The overflow and
// underflow tests use p alone: p_lo is below 2^-40 and the limits have
// room for it.
double region_pow_positive(const double x, const double y) noexcept {
    const RegionPower power = region_power(std::bit_cast<std::uint64_t>(x), y);
    if (power.p > region_overflow_limit) return std::numeric_limits<double>::infinity();
    if (power.p < region_underflow_limit) return 0.0;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    region_exp2(power.p, power.p_lo, head, tail, exponent);
    const double result = region_scale_flush(head, tail, exponent);
    return result;
}

// Inputs where x is zero or infinite (y finite or not, neither NaN): Annex
// F's zeros and infinities.
double region_pow_zero_or_infinite_base(const double x, const double y) noexcept {
    const double infinity = std::numeric_limits<double>::infinity();
    const bool odd = std::isfinite(y) && region_integer_kind(y) == RegionIntegerKind::odd;
    const bool negative_base = std::signbit(x);
    double magnitude = 0.0;
    if ((x == 0.0) == (y < 0.0)) magnitude = infinity;
    if (negative_base && odd) return -magnitude;
    return magnitude;
}

// Every double input the one-test fast path declines, after the subnormal
// rule reads subnormal arguments as zeros of their sign.
STXLIB_MATH_COLD double region_pow_outside(const double x_argument, const double y_argument) noexcept {
    const double x = ::stxlib::detail::flush_subnormal(x_argument);
    const double y = ::stxlib::detail::flush_subnormal(y_argument);
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (x != x || y != y) return x + y;
    const double infinity = std::numeric_limits<double>::infinity();
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(y) & magnitude_mask;
    if (y_magnitude >= region_y_kernel_high) {
        const double magnitude = std::fabs(x);
        if (magnitude == 1.0) return 1.0;
        if ((magnitude < 1.0) == (y > 0.0)) return 0.0;
        return infinity;
    }
    if (x == 0.0 || std::isinf(x)) return region_pow_zero_or_infinite_base(x, y);
    if (y_magnitude < region_y_kernel_low) {
        if (x > 0.0) return 1.0;
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (x > 0.0) return region_pow_positive(x, y);
    const RegionIntegerKind kind = region_integer_kind(y);
    if (kind == RegionIntegerKind::not_integer) return std::numeric_limits<double>::quiet_NaN();
    const double magnitude = region_pow_positive(-x, y);
    if (kind == RegionIntegerKind::odd) return -magnitude;
    return magnitude;
}

// ---- float pow ------------------------------------------------------------
//
// Round, test, decide (the owner's choice for float pow, 2026-11-15: the
// conservative path, bounded in effort and precision):
//   1. the region double kernel's head + tail (within 2^-69.31 of x^y over
//      float's range, proved: tools/math/region_certificate.py `pow`),
//      rounded once, unless it lies within region_pow_float_test of a float
//      midpoint (about 2^-42 of pairs);
//   2. then, if x^y is exactly representable or exactly a midpoint
//      (pow_float_exact: only y = n/2^k with k <= 3 and x a perfect 2^k-th
//      power can give one), that value rounded exactly (ties to even);
//   3. otherwise the accurate stage (pow_float_accurate: double-double
//      throughout, within region_pow_float_accurate_error of x^y, proved:
//      tools/math/pow_float_accurate_certificate.py) rounded once. It has a
//      fixed precision and a bounded cost; no precision loop.
// Correct rounding is proved at every pair except one whose x^y lies within
// region_pow_float_accurate_error of a midpoint without being one: no such
// pair is known (math.hpp states it). Every integer y in [1, 40] is proved
// for every x (tools/math/pow_float_midpoints.py, exact sweep).

// The test distance of stage 1, relative: above the proved 2^-69.31, with
// room, so a value outside it is the correctly rounded float.
constexpr double region_pow_float_test = 0x1p-67;

// The accurate stage's proved relative error, stated in math.hpp: the
// certificate's bound is 2^-94.10 for every pair with |y log2 x| <= 129
// (the log side within 2^-100.58 of y log2 x, the worst at x just above
// 1.0039; the exp side within 2^-102.65), rounded up here.
constexpr double region_pow_float_accurate_error = 0x1p-94;

// Double-double constants (hi the nearest double, lo the nearest double to
// the rest; the certificate checks each).
constexpr Wide pow_accurate_ln2{0x1.62e42fefa39efp-1, 0x1.abc9e3b39803fp-56};
constexpr Wide pow_accurate_inverse_ln2{0x1.71547652b82fep+0, 0x1.777d0ffda0d24p-56};

// log1p(r) = r (c1 + r (c2 + ... + r c13)), c_n = (-1)^(n+1)/n.
constexpr Wide pow_accurate_log_coefficients[13] = {
    {0x1.0000000000000p+0, 0.0},
    {-0x1.0000000000000p-1, 0.0},
    {0x1.5555555555555p-2, 0x1.5555555555555p-56},
    {-0x1.0000000000000p-2, 0.0},
    {0x1.999999999999ap-3, -0x1.999999999999ap-57},
    {-0x1.5555555555555p-3, -0x1.5555555555555p-57},
    {0x1.2492492492492p-3, 0x1.2492492492492p-57},
    {-0x1.0000000000000p-3, 0.0},
    {0x1.c71c71c71c71cp-4, 0x1.c71c71c71c71cp-58},
    {-0x1.999999999999ap-4, 0x1.999999999999ap-58},
    {0x1.745d1745d1746p-4, -0x1.745d1745d1746p-59},
    {-0x1.5555555555555p-4, -0x1.5555555555555p-58},
    {0x1.3b13b13b13b14p-4, -0x1.3b13b13b13b14p-58},
};

// e^u - 1 = u (d1 + u (d2 + ... + u d9)), d_n = 1/n!.
constexpr Wide pow_accurate_exp_coefficients[9] = {
    {0x1.0000000000000p+0, 0.0},
    {0x1.0000000000000p-1, 0.0},
    {0x1.5555555555555p-3, 0x1.5555555555555p-57},
    {0x1.5555555555555p-5, 0x1.5555555555555p-59},
    {0x1.1111111111111p-7, 0x1.1111111111111p-63},
    {0x1.6c16c16c16c17p-10, -0x1.f49f49f49f49fp-65},
    {0x1.a01a01a01a01ap-13, 0x1.a01a01a01a01ap-73},
    {0x1.a01a01a01a01ap-16, 0x1.a01a01a01a01ap-76},
    {0x1.71de3a556c734p-19, -0x1.c154f8ddc6c00p-73},
};

// Double-double arithmetic for the accurate stage. Each renormalizes with a
// two-sum (no precondition); the certificate bounds every rounding.
Wide wide_add(const Wide a, const Wide b) noexcept {
    double sum = 0.0;
    double sum_error = 0.0;
    two_sum(a.hi, b.hi, sum, sum_error);
    const double low = sum_error + (a.lo + b.lo);
    Wide result{};
    two_sum(sum, low, result.hi, result.lo);
    return result;
}

// a times b, a.lo * b.lo dropped.
Wide wide_multiply(const Wide a, const Wide b) noexcept {
    double product = 0.0;
    double product_error = 0.0;
    two_product(a.hi, b.hi, product, product_error);
    const double cross = a.hi * b.lo + a.lo * b.hi;
    const double low = product_error + cross;
    Wide result{};
    two_sum(product, low, result.hi, result.lo);
    return result;
}

Wide wide_multiply_double(const Wide a, const double b) noexcept {
    double product = 0.0;
    double product_error = 0.0;
    two_product(a.hi, b, product, product_error);
    const double low = product_error + a.lo * b;
    Wide result{};
    two_sum(product, low, result.hi, result.lo);
    return result;
}

// x^y = (hi + lo) 2^exponent for positive normal float x and normal float y
// (as doubles) with x^y in float's range, within
// region_pow_float_accurate_error of x^y.
//   log: x = 2^k z, r = z inv - 1 exact (z has 24 bits, inv 10, and |z inv -
//   1| < 2^-7.6, so the product and the difference are exact), log x = k ln 2
//   + logc + log1p(r) with log1p by Horner in double-double (13 terms);
//   p = y log2 x as a double-double.
//   exp: q = round(256 p), t = p - q/256 (exact head, two-sum with the low
//   part), u = t ln 2, e^u - 1 by Horner (9 terms), 2^(j/256) from
//   exp_table_256 as hi + lo.
void pow_float_accurate(const double x, const double y, double& hi, double& lo, std::int64_t& exponent) noexcept {
    const RegionReduction reduction = region_reduce(std::bit_cast<std::uint64_t>(x));
    const RegionLogEntry& entry = region_log_table_128[reduction.index];
    const double r = reduction.z * entry.inv - 1.0;  // exact
    Wide series = pow_accurate_log_coefficients[12];
    for (int n = 11; n >= 0; --n) {
        series = wide_add(wide_multiply_double(series, r), pow_accurate_log_coefficients[n]);
    }
    const Wide log1p_r = wide_multiply_double(series, r);
    const Wide log_z = wide_add(Wide{entry.logc_hi, entry.logc_lo}, log1p_r);
    const Wide log2_z = wide_multiply(log_z, pow_accurate_inverse_ln2);
    const Wide log2_x = wide_add(Wide{reduction.k, 0.0}, log2_z);
    const Wide p = wide_multiply_double(log2_x, y);

    const double shifted = p.hi * 256.0 + kernel_constants.rounding_shift;
    const std::int64_t q_bits = std::bit_cast<std::int64_t>(shifted) -
                                std::bit_cast<std::int64_t>(kernel_constants.rounding_shift);
    const std::uint64_t j = static_cast<std::uint64_t>(q_bits) & exp_index_mask;
    exponent = q_bits >> 8;
    const double t_head = p.hi - (shifted - kernel_constants.rounding_shift) * (1.0 / 256.0);  // exact
    Wide t{};
    two_sum(t_head, p.lo, t.hi, t.lo);
    const Wide u = wide_multiply(t, pow_accurate_ln2);
    Wide expm1_series = pow_accurate_exp_coefficients[8];
    for (int n = 7; n >= 0; --n) {
        expm1_series = wide_add(wide_multiply(expm1_series, u), pow_accurate_exp_coefficients[n]);
    }
    const Wide expm1_u = wide_multiply(expm1_series, u);
    const ExpEntry& power = exp_table_256[j];
    const Wide table{power.hi, power.lo};
    const Wide value = wide_add(table, wide_multiply(table, expm1_u));
    hi = value.hi;
    lo = value.lo;
}

// Whether x^y is exactly a float or a float midpoint, for positive normal
// float x and normal float y (as doubles) with x^y in float's range; if so,
// result is x^y rounded (ties to even). With x = X 2^f (X odd) and y =
// n/2^k (n odd, or k = 0): for y < 0, x^y is a power of two or not dyadic;
// for y > 0 it is dyadic only if X = t^(2^k) and 2^k divides f, and then x^y
// = t^n 2^(f n/2^k), which has at most 25 significant bits only for t = 1,
// or t >= 3 with 2^k <= 15 and n <= 15 (3^16 > 2^25). So y in (0, 16] with
// 8y an integer covers every midpoint; the exact results it leaves out are
// powers of two (t = 1), half a float ulp from every midpoint, which the
// accurate stage rounds correctly.
bool pow_float_exact(const double x, const double y, float& result) noexcept {
    if (!(y > 0.0) || y > 16.0) return false;
    const double scaled_y = y * 8.0;  // exact
    std::int64_t n = static_cast<std::int64_t>(scaled_y);
    if (static_cast<double>(n) != scaled_y) return false;
    int k = 3;
    while (k > 0 && (n & 1) == 0) {
        n >>= 1;
        --k;
    }
    // x = odd 2^f from its bits (normal: the implicit bit set, so odd > 0).
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t significand = (bits & region_mantissa_mask) | region_implicit_bit;
    const int trailing = std::countr_zero(significand);
    std::uint64_t odd = significand >> trailing;
    const std::int64_t f = static_cast<std::int64_t>((bits >> 52) & 0x7ff) - 1075 + trailing;
    const std::int64_t root_degree = std::int64_t{1} << k;
    if (f % root_degree != 0) return false;
    for (int level = 0; level < k; ++level) {
        // odd < 2^24: the square root of a perfect square is exact.
        const std::uint64_t root = static_cast<std::uint64_t>(std::sqrt(static_cast<double>(odd)));
        if (root * root != odd) return false;
        odd = root;
    }
    std::uint64_t power = 1;
    for (std::int64_t i = 0; i < n; ++i) {
        power *= odd;  // below 2^25 times below 2^24: no wrap
        if (power >= (std::uint64_t{1} << 25)) return false;  // more than 25 odd bits
    }
    const std::int64_t scale = (f / root_degree) * n;  // in [-152, 128] for a result in float's range
    const double exact = static_cast<double>(power) * power_of_two(scale);  // exact
    result = static_cast<float>(exact);  // ties to even
    return true;
}

// Stages 2 and 3, out of line: about one call in 2^42.
STXLIB_MATH_COLD float region_pow_float_decide(const double x, const double y) noexcept {
    float exact = 0.0f;
    if (pow_float_exact(x, y, exact)) return exact;
    double hi = 0.0;
    double lo = 0.0;
    std::int64_t exponent = 0;
    pow_float_accurate(x, y, hi, lo, exponent);
    const double scale = power_of_two(exponent);  // exponent in [-128, 129]
    const float result = round_wide_to_float(hi * scale, lo * scale);  // the scalings are exact
    return result;
}

// x^y for positive normal float x and normal float y (as doubles), rounded
// once to float, then flushed. Above 2^128.5 the float result is infinity;
// below 2^-127 it is under the smallest normal float and flushed.
//
// The test: m is the nearest point of the 2^(E-24) grid (E the exponent of
// hi): adding c = 1.5 2^(E+28) rounds to that grid, and the sum's last bit
// is m's grid parity, odd for a midpoint.
float region_pow_float_positive(const double x, const double y) noexcept {
    const RegionPower power = region_power(std::bit_cast<std::uint64_t>(x), y);
    if (power.p > 128.5) return std::numeric_limits<float>::infinity();
    if (power.p < -127.0) return 0.0f;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    region_exp2(power.p, power.p_lo, head, tail, exponent);
    const double scale = power_of_two(exponent);  // exponent in [-128, 129]
    const double hi = head * scale;  // exact
    const double lo = tail * scale;  // exact
    const std::uint64_t hi_bits = std::bit_cast<std::uint64_t>(hi);
    const std::uint64_t grid_bits =
        ((hi_bits & infinity_bits) + (std::uint64_t{28} << 52)) | 0x0008000000000000ULL;
    const double grid = std::bit_cast<double>(grid_bits);  // 1.5 2^(E+28)
    const double on_grid = hi + grid;
    const bool odd = (std::bit_cast<std::uint64_t>(on_grid) & 1) != 0;
    const double midpoint = on_grid - grid;  // exact
    const double distance = (hi - midpoint) + lo;  // hi - midpoint exact
    float chosen = round_wide_to_float(hi, lo);
    if (odd && std::fabs(distance) <= hi * region_pow_float_test) {
        chosen = region_pow_float_decide(x, y);
    }
    const float result = ::stxlib::detail::flush_subnormal(chosen);
    return result;
}

// Every float input the fast path declines, after the subnormal rule.
STXLIB_MATH_COLD float region_pow_float_outside(const float x_argument, const float y_argument) noexcept {
    const float x = ::stxlib::detail::flush_subnormal(x_argument);
    const float y = ::stxlib::detail::flush_subnormal(y_argument);
    if (y == 0.0f) return 1.0f;
    if (x == 1.0f) return 1.0f;
    if (x != x || y != y) return x + y;
    const double xd = widen(x);
    const double yd = widen(y);
    if (std::isinf(yd)) {
        const double magnitude = std::fabs(xd);
        if (magnitude == 1.0) return 1.0f;
        if ((magnitude < 1.0) == (yd > 0.0)) return 0.0f;
        return std::numeric_limits<float>::infinity();
    }
    if (xd == 0.0 || std::isinf(xd)) {
        const double result = region_pow_zero_or_infinite_base(xd, yd);
        return static_cast<float>(result);  // a zero or an infinity: exact
    }
    // x finite, nonzero and normal; y finite and normal.
    if (xd > 0.0) return region_pow_float_positive(xd, yd);
    const RegionIntegerKind kind = region_integer_kind(yd);
    if (kind == RegionIntegerKind::not_integer) return std::numeric_limits<float>::quiet_NaN();
    const float magnitude = region_pow_float_positive(-xd, yd);
    if (kind == RegionIntegerKind::odd) return -magnitude;
    return magnitude;
}

// ---- float logarithms -----------------------------------------------------
//
// For a positive normal float x = 2^k z (z in the region table's buckets):
// z has 24 significant bits and inv at most 10, so r = z inv - 1 is exact
// (34-bit product, Sterbenz), a multiple of 2^-33 with |r| < 2^-7.87 and at
// most 26 bits, and r^2 is exact. Then
//   log x = k ln2 + logc + r - r^2/2 + r^3 (1/3 - r/4 + ... - r^5/8)
// with the head k ln2_hi + logc_hi + r - r^2/2 formed by fast two-sums
// (|k ln2| >= 0.69 > |logc| for k != 0; |logc_hi| >= max |r| in every
// bucket but 80, where logc = 0; |logc + r| > r^2/2) and the rest in the
// tail. The tail is below 2^-24.5, so its roundings are below 2^-77, and
// the truncation r^9/9 is below 2^-73.7: against |log x| >= 2^-9 outside
// bucket 80 (and relative to r inside it) the error is below 2^-62
// (region_certificate.py `logf`).

struct FloatLogParts final {
    double k{0.0};
    double r{0.0};
    double half_square{0.0};
    double cubic{0.0};
    const RegionLogEntry* entry{nullptr};
};

FloatLogParts float_log_parts(const float x) noexcept {
    const double xd = widen(x);
    const RegionReduction reduction = region_reduce(std::bit_cast<std::uint64_t>(xd));
    const RegionLogEntry& entry = region_log_table_128[reduction.index];
    const double r = reduction.z * entry.inv - 1.0;  // exact
    const double square = r * r;  // exact
    const double cubic =
        (r * square) * (kernel_constants.log_3 + r * (kernel_constants.log_4 + r * (kernel_constants.log_5 + r * (kernel_constants.log_6 + r * (kernel_constants.log_7 + r * kernel_constants.log_8)))));
    const FloatLogParts result{reduction.k, r, square * 0.5, cubic, &entry};
    return result;
}

float float_log_natural(const float x) noexcept {
    const FloatLogParts parts = float_log_parts(x);
    const RegionLogEntry& entry = *parts.entry;
    double b = 0.0;
    double e1 = 0.0;
    fast_two_sum(parts.k * region_ln2_hi, entry.logc_hi, b, e1);  // k ln2_hi exact
    double c = 0.0;
    double e2 = 0.0;
    fast_two_sum(b, parts.r, c, e2);
    double d = 0.0;
    double e3 = 0.0;
    fast_two_sum(c, -parts.half_square, d, e3);
    const double tail = ((e1 + e2) + e3) + ((entry.logc_lo + parts.k * region_ln2_lo) + parts.cubic);
    const float result = round_wide_to_float(d, tail);
    return result;
}

// log2 x = k + log2c + (r - r^2/2 + cubic) / ln 2: r (26 bits) times the
// 26-bit cut of 1/ln 2 is exact, and so is the 26-bit cut of r^2/2 times
// it; the rests are below 2^-33 and go to the tail. k + log2c_hi is exact.
float float_log_binary(const float x) noexcept {
    const FloatLogParts parts = float_log_parts(x);
    const RegionLogEntry& entry = *parts.entry;
    const double p1 = parts.r * region_inverse_ln2_cut;  // exact
    const double p1_rest = parts.r * region_inverse_ln2_rest;
    const double h1 = cut_26(parts.half_square);
    const double q1 = h1 * region_inverse_ln2_cut;  // exact
    const double q_rest = (parts.half_square - h1) * region_inverse_ln2_cut + parts.half_square * region_inverse_ln2_rest;
    const double whole = parts.k + entry.log2c_hi;  // exact
    double c = 0.0;
    double e2 = 0.0;
    fast_two_sum(whole, p1, c, e2);
    double d = 0.0;
    double e3 = 0.0;
    fast_two_sum(c, -q1, d, e3);
    const double tail = (e2 + e3) + ((entry.log2c_lo + (p1_rest - q_rest)) + parts.cubic * region_inverse_ln2_hi);
    const float result = round_wide_to_float(d, tail);
    return result;
}

// log10 x = k log10(2) + (logc + r - r^2/2 + cubic) / ln 10: the natural
// log of z as w + w_lo (fast two-sums as above), times 1/ln 10 through the
// 26-bit cuts of w and of 1/ln 10; k log10_2_hi is exact.
float float_log_decimal(const float x) noexcept {
    const FloatLogParts parts = float_log_parts(x);
    const RegionLogEntry& entry = *parts.entry;
    double w1 = 0.0;
    double e1 = 0.0;
    fast_two_sum(entry.logc_hi, parts.r, w1, e1);
    double w = 0.0;
    double e2 = 0.0;
    fast_two_sum(w1, -parts.half_square, w, e2);
    const double w_lo = (e1 + e2) + (entry.logc_lo + parts.cubic);
    const double v1 = cut_26(w);
    const double main = v1 * region_inverse_ln10_cut;  // exact
    const double rest = ((w - v1) * region_inverse_ln10_cut + w * region_inverse_ln10_rest) + w_lo * region_inverse_ln10_hi;
    double h = 0.0;
    double e3 = 0.0;
    fast_two_sum(parts.k * region_log10_2_hi, main, h, e3);  // k log10_2_hi exact
    const double tail = e3 + (parts.k * region_log10_2_lo + rest);
    const float result = round_wide_to_float(h, tail);
    return result;
}

// Every float the logarithms' fast path declines: NaN, zeros and
// subnormals of either sign (read as zero: -infinity), negatives (NaN) and
// +infinity.
STXLIB_MATH_COLD float float_log_outside(const float x) noexcept {
    if (x != x) return x + x;
    const std::uint32_t magnitude = std::bit_cast<std::uint32_t>(x) & 0x7fffffffU;
    if (magnitude < 0x00800000U) return -std::numeric_limits<float>::infinity();
    if (x < 0.0f) return std::numeric_limits<float>::quiet_NaN();
    return x;  // +infinity
}

// True for positive normal finite floats. Modular by design: the range
// as one unsigned compare.
STXLIB_MODULAR bool float_positive_normal(const float x) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x);
    const bool result = bits - 0x00800000U < 0x7f000000U;
    return result;
}

// True for normal finite floats of either sign (modular, as above).
STXLIB_MODULAR bool float_normal(const float x) noexcept {
    const std::uint32_t magnitude = std::bit_cast<std::uint32_t>(x) & 0x7fffffffU;
    const bool result = magnitude - 0x00800000U < 0x7f000000U;
    return result;
}

// ---- float hypot ----------------------------------------------------------

// The value of a float bit pattern as a double, with the infinity pattern
// read as 2^128 (the "float" above FLT_MAX for rounding at overflow).
double float_bits_value(const std::uint32_t bits) noexcept {
    const double finite = static_cast<double>(std::bit_cast<float>(bits & 0x7f7fffffU));
    const double result = bits >= 0x7f800000U ? 0x1p128 : finite;
    return result;
}

// hypot of a >= b >= 0 (floats as doubles), a normal: correctly rounded to
// float for every pair, by construction.
//
// a^2 and b^2 are exact (48 bits) and S + S_err = a^2 + b^2 exactly
// (two-sum). r = sqrt(S) is within an ulp of the true root h, so f = r
// rounded to float is h's correct rounding or its neighbour on r's side of
// f. m, the midpoint between f and that neighbour, has 25 bits, so m^2 is
// exact; S - m^2 is exact (Sterbenz: S and m^2 within 2^-22 of each other)
// and a multiple of ulp(S) (m^2 is a multiple of 2^(2E-48), ulp(S) at most
// 2^(2E-50)), so when nonzero it exceeds |S_err| <= ulp(S)/2 and the sign
// of (S - m^2) + S_err, even rounded, is the sign of h^2 - m^2. h beyond m
// gives the neighbour, short of it f, equal to it the even one of the two.
// Infinity is the float above FLT_MAX: the same test rounds at overflow.
float float_hypot_ordered(const double a, const double b) noexcept {
    const double a_squared = a * a;  // exact
    const double b_squared = b * b;  // exact
    double s = 0.0;
    double s_error = 0.0;
    two_sum(a_squared, b_squared, s, s_error);
    const double r = std::sqrt(s);  // IEEE sqrt: correctly rounded
    const float f = static_cast<float>(r);
    const std::uint32_t f_bits = std::bit_cast<std::uint32_t>(f);
    const double f_value = float_bits_value(f_bits);
    // Above infinity there is no neighbour: an r beyond 2^128 keeps f.
    const bool up = f_value < r && f_bits < 0x7f800000U;
    const std::uint32_t neighbour_bits = up ? f_bits + 1 : f_bits - 1;
    const double midpoint = (f_value + float_bits_value(neighbour_bits)) * 0.5;  // exact
    const double midpoint_squared = midpoint * midpoint;  // exact
    const double difference = (s - midpoint_squared) + s_error;  // sign exact
    const bool beyond = up ? difference > 0.0 : difference < 0.0;
    const std::uint32_t even_bits = (f_bits & 1U) == 0 ? f_bits : neighbour_bits;
    const std::uint32_t nearest_bits = beyond ? neighbour_bits : f_bits;
    const std::uint32_t chosen_bits = difference == 0.0 ? even_bits : nearest_bits;
    const float result = std::bit_cast<float>(chosen_bits);
    return result;
}

} // namespace
} // namespace detail

namespace region {

double pow(const double x, const double y) noexcept {
    // The common case in one test, as pow.cpp's: x positive, normal and
    // finite; y in the kernel's range.
    const std::uint64_t x_bits = std::bit_cast<std::uint64_t>(x);
    const std::uint64_t y_magnitude = std::bit_cast<std::uint64_t>(y) & detail::magnitude_mask;
    if (detail::log_fast_path(x_bits) && y_magnitude >= detail::region_y_kernel_low &&
        y_magnitude < detail::region_y_kernel_high) {
        const double result = detail::region_pow_positive(x, y);
        return result;
    }
    const double result = detail::region_pow_outside(x, y);
    return result;
}

float pow(const float x, const float y) noexcept {
    if (detail::float_positive_normal(x) && detail::float_normal(y)) {
        const float result = detail::region_pow_float_positive(detail::widen(x), detail::widen(y));
        return result;
    }
    const float result = detail::region_pow_float_outside(x, y);
    return result;
}

float log(const float x) noexcept {
    if (detail::float_positive_normal(x)) {
        const float result = detail::float_log_natural(x);
        return result;
    }
    const float result = detail::float_log_outside(x);
    return result;
}

float log2(const float x) noexcept {
    if (detail::float_positive_normal(x)) {
        const float result = detail::float_log_binary(x);
        return result;
    }
    const float result = detail::float_log_outside(x);
    return result;
}

float log10(const float x) noexcept {
    if (detail::float_positive_normal(x)) {
        const float result = detail::float_log_decimal(x);
        return result;
    }
    const float result = detail::float_log_outside(x);
    return result;
}

float hypot(const float x_argument, const float y_argument) noexcept {
    const float x = ::stxlib::detail::flush_subnormal(x_argument);
    const float y = ::stxlib::detail::flush_subnormal(y_argument);
    const double ax = std::fabs(detail::widen(x));
    const double ay = std::fabs(detail::widen(y));
    // Annex F: an infinite operand gives +infinity even when the other is NaN.
    if (std::isinf(ax) || std::isinf(ay)) return std::numeric_limits<float>::infinity();
    if (ax != ax || ay != ay) return x + y;
    const bool x_larger = ax >= ay;
    const double a = x_larger ? ax : ay;
    const double b = x_larger ? ay : ax;
    if (a == 0.0) return 0.0f;
    const float result = detail::float_hypot_ordered(a, b);
    return result;
}

// Test hooks (tests/math_region_tests.cpp), for positive normal float x and
// normal float y with x^y in float's normal range: stages 2 and 3 of float
// pow on every pair, stage 1's test skipped (they are correctly rounded
// wherever pow is), and the accurate stage's value, scaled.
float pow_float_decided(const float x, const float y) noexcept {
    const float chosen = detail::region_pow_float_decide(detail::widen(x), detail::widen(y));
    const float result = ::stxlib::detail::flush_subnormal(chosen);
    return result;
}

void pow_float_accurate_value(const float x, const float y, double& hi, double& lo) noexcept {
    std::int64_t exponent = 0;
    detail::pow_float_accurate(detail::widen(x), detail::widen(y), hi, lo, exponent);
    const double scale = detail::power_of_two(exponent);  // exponent in [-128, 129]
    hi *= scale;  // exact
    lo *= scale;  // exact: lo stays a normal double for x^y >= 2^-126
}

} // namespace region

} // namespace stxlib::math::STXLIB_MATH_VARIANT

STXLIB_MATH_TARGET_POP
