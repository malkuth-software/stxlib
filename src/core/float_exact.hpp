#pragma once

// STXlib's exact floating-point primitives (planning/stages/
// A1-float-discipline.md, section 2; owner decisions F3 = 1 and F4 = 1,
// 2026-11-13, notes decisions/2026-11-13-locals-float-mutability.md).
// Private to STXlib's sources, with no public interface until a consumer
// needs one. In float and double:
//
//   two_sum, fast_two_sum      a sum and its exact rounding error
//   split, two_product         a product and its exact rounding error, by a
//                              fused multiply-add or by Dekker's split, with
//                              identical bits (the error term is exact and
//                              therefore unique)
//   grow_expansion, expansion_sum, scale_expansion, expansion_product,
//   expansion_sign             Shewchuk's exact expansion arithmetic
//   KahanSum, NeumaierSum      compensated summation
//   orient2d, incircle         exact signs of the two geometric determinants
//
// Sources: D. E. Knuth, TAOCP vol. 2, 3rd ed., section 4.2.2 (two_sum);
// T. J. Dekker, Numerische Mathematik 18 (1971) 224-242 (fast_two_sum, the
// split and the product); J. R. Shewchuk, Discrete & Computational Geometry
// 18 (1997) 305-363 (expansions, the orient2d and incircle error bounds);
// W. Kahan, CACM 8(1) (1965) 40; A. Neumaier, ZAMM 54 (1974) 39-51. The
// double forms of two_sum, fast_two_sum, split and two_product are the
// math's (src/algorithms/math/kernel_support.hpp, stx_auxlib b6e330a), moved
// here with the same operations in the same order, so the math's bits are
// unchanged.
//
// Range (F3 = 1). Exactness needs every error term to be representable:
// not subnormal (STXlib's subnormal rule forbids subnormal values) and not
// overflowed. Each primitive states a range in which no intermediate
// overflows and no error term can be subnormal; outside it the exact
// identities do not hold (an error term can be NaN or infinite, the M5
// counterexamples below), so the range is a precondition:
// - sums (two_sum, fast_two_sum, compensated sums): every operand zero or of
//   magnitude at least 2^(emin + p) (double 2^-969, float 2^-102) and at
//   most 2^(emax - 1) (double 2^1022, float 2^126). Operands at least
//   2^(emin + p - 1) are integer multiples of 2^emin, so every error term is
//   a multiple of 2^emin: zero or normal. The upper bound is on each
//   operand, so it is checked before any arithmetic, and it excludes
//   overflow outright: |a + b| <= 2^emax, which is a float, so the sum is
//   at most 2^emax and finite. Then two_sum's b_part = sum - a = b + d,
//   |d| <= ulp(sum) / 2 <= 2^(emax - p), is at most 2^(emax - 1) +
//   2^(emax - p) < 2^emax; likewise a_part = sum - b_part = a - d', |d'| <=
//   2^(emax - p); and the two corrections are at most 2^(emax - p + 1). So
//   no operation of two_sum overflows, and Knuth's theorem (exact barring
//   overflow) applies. The M5 runs refuted two weaker bounds: none (a =
//   0x767e0000, b = -FLT_MAX: a finite sum and a NaN error) and 2^emax on
//   each operand (a = -0x1.fffffep126, b = -0x1p127: the sum is the tie
//   above FLT_MAX and rounds to infinity; ESBMC's overflow check on the
//   first add). fast_two_sum needs only a finite sum (its sum - a is
//   exact), but shares the range.
// - split: a zero, or exponent(a) >= emin + p - 1 (double -970, float
//   -103), so both halves are multiples of ulp(a) >= 2^emin; and |a| <=
//   2^995 (double) or 2^115 (float), where the split cannot overflow.
// - two_product: both operands in split's range, a zero included (Dekker's
//   form splits a zero's partner too: a = -0, b = -FLT_MAX gave NaN, M5);
//   for nonzero operands, exponent(a) + exponent(b) >= emin + 2p - 2
//   (double -918, float -80), which makes ulp(a) * ulp(b), and so the error
//   term and every partial product of the split, a multiple of 2^emin; and
//   exponent(a) + exponent(b) <= emax - 2 (double 1021, float 125). Each
//   split half is at most 2^(exponent + 1) in magnitude, so a_hi * b_hi and
//   the product are at most 2^emax and no partial product or partial sum
//   overflows; without it, a = 0x5edbbe36, b = 0x60151e43 (exponents 62 and
//   65) has a finite product and an infinite Dekker error (M5). The fused
//   form needs only the finite product, but the forms share one range so
//   their bits agree wherever it holds. The owner's F3 wording names the
//   sums' bound; a product needs these as well, with the same purpose.
// - expansions: every component zero or normal, and every value an integer
//   multiple of a power of two whose products stay at or above 2^emin; and
//   every two-sum's operands at most 2^(emax - 1) (as for the sums), every
//   product's operands within split's magnitude limit with exponent(a) +
//   exponent(b) <= emax - 2. A sufficient condition for the sums: the sum
//   of the magnitudes of every component that enters (times the factor, for
//   a scaled expansion) at most 2^(emax - 2), since each running sum is
//   then at most that sum times (1 + u)^n <= 2 for n < 2^52 (rounding is
//   monotone). The
//   expansions orient2d and incircle build meet all of it by their
//   coordinate ranges below.
// The ranges are checked when STXLIB_FLOAT_CHECKS is defined (CMake defines
// it for every STXlib target in Debug builds, so one build never mixes
// checked and unchecked copies of these inline functions), and are the
// caller's to argue otherwise. The raw forms (namespace raw) are never
// checked: the math calls them under its own error analysis.
//
// Rounding is the default round-to-nearest-even, which rule 3 of the
// discipline guarantees (Shewchuk's expansion sum needs it).

#include "core/float_discipline.hpp"
#include "stxlib/require.hpp"
#include "stxlib/types.hpp"

#include <bit>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define STXLIB_FLOAT_EXACT_INLINE __attribute__((always_inline)) inline
#else
#define STXLIB_FLOAT_EXACT_INLINE inline
#endif

// The form two_product takes outside the math (the math chooses per kernel
// variant, calling raw::two_product_fused or raw::two_product_dekker
// itself): fused where the whole build targets a hardware fused
// multiply-add, Dekker's otherwise. The two forms live in differently named
// inline namespaces, so a program never holds two different bodies under
// one name.
#if !defined(STXLIB_FLOAT_EXACT_DEKKER) && (defined(__FMA__) || defined(__ARM_FEATURE_FMA))
#define STXLIB_FLOAT_EXACT_FUSED 1
#endif

#if defined(STXLIB_FLOAT_CHECKS)
#define STXLIB_FLOAT_REQUIRE(condition, message) STXLIB_REQUIRE(condition, message)
#else
#define STXLIB_FLOAT_REQUIRE(condition, message) static_cast<void>(0)
#endif

namespace stxlib::float_exact {

// A value and its exact error: value + error is the exact result.
template <typename T>
struct Pair final {
    T value;
    T error;
};

// The two formats and the constants of their ranges.
template <typename T>
struct Format;

template <>
struct Format<double> final {
    using Bits = std::uint64_t;
    static constexpr int precision = 53;
    static constexpr int min_exponent = -1022;
    static constexpr int max_exponent = 1023;
    static constexpr int exponent_bias = 1023;
    static constexpr int fraction_bits = 52;
    static constexpr Bits exponent_mask = 0x7ffULL;
    static constexpr double splitter = 134217729.0;  // 2^27 + 1
    static constexpr double split_limit = 0x1p995;
    static constexpr double sum_floor = 0x1p-969;    // 2^(emin + p)
    static constexpr double sum_ceiling = 0x1p1022;  // 2^(emax - 1)
    static constexpr double unit_roundoff = 0x1p-53; // Shewchuk's epsilon
};

template <>
struct Format<float> final {
    using Bits = std::uint32_t;
    static constexpr int precision = 24;
    static constexpr int min_exponent = -126;
    static constexpr int max_exponent = 127;
    static constexpr int exponent_bias = 127;
    static constexpr int fraction_bits = 23;
    static constexpr Bits exponent_mask = 0xffU;
    static constexpr float splitter = 4097.0f;       // 2^12 + 1
    static constexpr float split_limit = 0x1p115f;
    static constexpr float sum_floor = 0x1p-102f;    // 2^(emin + p)
    static constexpr float sum_ceiling = 0x1p126f;   // 2^(emax - 1)
    static constexpr float unit_roundoff = 0x1p-24f;
};

// The unbiased exponent of a normal value (the biased field less the bias);
// min_exponent - 1 for zero (and for a subnormal, which the range excludes).
template <typename T>
[[nodiscard]] constexpr int exponent_of(const T value) noexcept {
    using Bits = typename Format<T>::Bits;
    const Bits bits = std::bit_cast<Bits>(value);
    const Bits field = (bits >> Format<T>::fraction_bits) & Format<T>::exponent_mask;
    const int result = static_cast<int>(field) - Format<T>::exponent_bias;
    return result;
}

template <typename T>
[[nodiscard]] constexpr T magnitude(const T value) noexcept {
    const T result = value < T(0) ? -value : value;
    return result;
}

template <typename T>
[[nodiscard]] constexpr bool finite(const T value) noexcept {
    const bool result = value - value == T(0);
    return result;
}

// Range tests, for the checks and for callers that want to test first.
template <typename T>
[[nodiscard]] constexpr bool in_sum_range(const T value) noexcept {
    const bool result = value == T(0) || (finite(value) && magnitude(value) >= Format<T>::sum_floor &&
                                          magnitude(value) <= Format<T>::sum_ceiling);
    return result;
}

template <typename T>
[[nodiscard]] constexpr bool in_split_range(const T value) noexcept {
    if (value == T(0)) return true;
    const bool result = finite(value) && magnitude(value) <= Format<T>::split_limit &&
                        exponent_of(value) >= Format<T>::min_exponent + Format<T>::precision - 1;
    return result;
}

template <typename T>
[[nodiscard]] constexpr bool in_product_range(const T a, const T b) noexcept {
    if (!in_split_range(a) || !in_split_range(b)) return false;
    if (a == T(0) || b == T(0)) return true;
    const int exponents = exponent_of(a) + exponent_of(b);
    const bool above = exponents >= Format<T>::min_exponent + 2 * Format<T>::precision - 2;
    const bool below = exponents <= Format<T>::max_exponent - 2;
    const bool result = above && below;
    return result;
}

// The overflow side of the ranges alone, for the expansions' steps (whose
// underflow side is argued from the components' multiples, not from
// exponents): a two-sum operand at most 2^(emax - 1), and a product's
// operands within split's magnitude limit with exponent(a) + exponent(b) <=
// emax - 2.
// A NaN fails both.
template <typename T>
[[nodiscard]] constexpr bool below_sum_ceiling(const T value) noexcept {
    const bool result = magnitude(value) <= Format<T>::sum_ceiling;
    return result;
}

template <typename T>
[[nodiscard]] constexpr bool product_below_overflow(const T a, const T b) noexcept {
    const bool limited = magnitude(a) <= Format<T>::split_limit && magnitude(b) <= Format<T>::split_limit;
    if (!limited) return false;
    if (a == T(0) || b == T(0)) return true;
    const bool result = exponent_of(a) + exponent_of(b) <= Format<T>::max_exponent - 2;
    return result;
}

template <typename T>
[[nodiscard]] constexpr bool zero_or_normal(const T value) noexcept {
    const bool result = value == T(0) || (finite(value) && exponent_of(value) >= Format<T>::min_exponent);
    return result;
}

namespace raw {

// Unchecked forms: the operations exactly as Knuth and Dekker give them.

// Sum and its exact rounding error (Knuth's two-sum, 6 operations).
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE constexpr Pair<T> two_sum(const T a, const T b) noexcept {
    const T sum = a + b;
    const T b_part = sum - a;
    const T a_part = sum - b_part;
    const T error = (a - a_part) + (b - b_part);
    return Pair<T>{sum, error};
}

// Sum and its exact rounding error when exponent(a) >= exponent(b) or
// a == 0 (Dekker's fast two-sum, 3 operations).
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE constexpr Pair<T> fast_two_sum(const T a, const T b) noexcept {
    const T sum = a + b;
    const T error = b - (sum - a);
    return Pair<T>{sum, error};
}

// Veltkamp's split: hi + lo == a exactly, each with at most ceil(p/2)
// significant bits (26 and 26 for double, 12 and 12 for float, the sign
// absorbing one). Requires |a| <= split_limit.
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE constexpr Pair<T> split(const T a) noexcept {
    const T scaled = Format<T>::splitter * a;
    const T hi = scaled - (scaled - a);
    const T lo = a - hi;
    return Pair<T>{hi, lo};
}

// Product and its exact error by Dekker's split (17 operations).
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE constexpr Pair<T> two_product_dekker(const T a, const T b) noexcept {
    const T product = a * b;
    const Pair<T> a_parts = split(a);
    const Pair<T> b_parts = split(b);
    const T error = ((a_parts.value * b_parts.value - product) + a_parts.value * b_parts.error +
                     a_parts.error * b_parts.value) +
                    a_parts.error * b_parts.error;
    return Pair<T>{product, error};
}

// The same with a's split supplied (a constant split at compile time): 10
// operations, the same bits.
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE constexpr Pair<T> two_product_dekker_split(const T a, const Pair<T> a_parts,
                                                                                const T b) noexcept {
    const T product = a * b;
    const Pair<T> b_parts = split(b);
    const T error = ((a_parts.value * b_parts.value - product) + a_parts.value * b_parts.error +
                     a_parts.error * b_parts.value) +
                    a_parts.error * b_parts.error;
    return Pair<T>{product, error};
}

#if defined(__GNUC__) || defined(__clang__)
// Product and its exact error by a fused multiply-add (2 operations). Call
// only where the code runs on a hardware fused multiply-add (always inlined,
// so it takes its caller's target); elsewhere the builtin falls back to the
// C library's fma, which is slow and on some runtimes not correctly rounded.
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> two_product_fused(const T a, const T b) noexcept {
    const T product = a * b;
    T error = T(0);
    if constexpr (sizeof(T) == sizeof(float)) {
        error = __builtin_fmaf(a, b, -product);
    } else {
        error = __builtin_fma(a, b, -product);
    }
    return Pair<T>{product, error};
}
#endif

} // namespace raw

// Checked forms (the ranges above, when STXLIB_FLOAT_CHECKS is defined).

template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> two_sum(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(in_sum_range(a) && in_sum_range(b), "float_exact::two_sum: operand out of range");
    const Pair<T> result = raw::two_sum(a, b);
    STXLIB_FLOAT_REQUIRE(finite(result.value), "float_exact::two_sum: the sum overflows");
    return result;
}

template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> fast_two_sum(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(in_sum_range(a) && in_sum_range(b), "float_exact::fast_two_sum: operand out of range");
    STXLIB_FLOAT_REQUIRE(a == T(0) || b == T(0) || exponent_of(a) >= exponent_of(b),
                         "float_exact::fast_two_sum: exponent(a) < exponent(b)");
    const Pair<T> result = raw::fast_two_sum(a, b);
    STXLIB_FLOAT_REQUIRE(finite(result.value), "float_exact::fast_two_sum: the sum overflows");
    return result;
}

template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> split(const T a) noexcept {
    STXLIB_FLOAT_REQUIRE(in_split_range(a), "float_exact::split: operand out of range");
    const Pair<T> result = raw::split(a);
    return result;
}

// Everything from here on depends on the build's product form, so it lives
// in an inline namespace named for that form.
#if defined(STXLIB_FLOAT_EXACT_FUSED)
inline namespace fused_form {
#else
inline namespace dekker_form {
#endif

template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> two_product(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(in_product_range(a, b), "float_exact::two_product: operands out of range");
#if defined(STXLIB_FLOAT_EXACT_FUSED)
    const Pair<T> result = raw::two_product_fused(a, b);
#else
    const Pair<T> result = raw::two_product_dekker(a, b);
#endif
    STXLIB_FLOAT_REQUIRE(finite(result.value), "float_exact::two_product: the product overflows");
    return result;
}

// ---- Expansions (Shewchuk 1997) --------------------------------------------
//
// An expansion is a sum of components e[0] + ... + e[n-1], nonoverlapping
// and in increasing order of magnitude, which represents its value exactly.
// The functions write into caller storage and return the count written;
// the zero-eliminating forms drop zero components but always write at least
// one. Inputs and outputs must not overlap. Each states the most it writes.
// The arithmetic inside is the raw forms'; the checks (STXLIB_FLOAT_CHECKS)
// are on the components, which must be zero or normal, and on the overflow
// side of each step's range (the component steps below and
// expansion_product_step).

// The expansions' sum steps: the raw forms, with the overflow side of
// two_sum's range checked when STXLIB_FLOAT_CHECKS is defined (the same
// bits otherwise).
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> component_two_sum(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(below_sum_ceiling(a) && below_sum_ceiling(b),
                         "float_exact: an expansion's two-sum operand above 2^(emax - 1)");
    const Pair<T> result = raw::two_sum(a, b);
    STXLIB_FLOAT_REQUIRE(finite(result.value), "float_exact: an expansion's sum overflows");
    return result;
}

template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> component_fast_two_sum(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(below_sum_ceiling(a) && below_sum_ceiling(b),
                         "float_exact: an expansion's two-sum operand above 2^(emax - 1)");
    const Pair<T> result = raw::fast_two_sum(a, b);
    STXLIB_FLOAT_REQUIRE(finite(result.value), "float_exact: an expansion's sum overflows");
    return result;
}

// h = e + b; writes elen + 1 components (Grow-Expansion, zero components
// kept).
template <typename T>
usize grow_expansion(const T* const e, const usize elen, const T b, T* const h) noexcept {
    T q = b;
    for (usize index = 0; index < elen; ++index) {
        STXLIB_FLOAT_REQUIRE(zero_or_normal(e[index]), "float_exact::grow_expansion: component out of range");
        const Pair<T> sum = component_two_sum(q, e[index]);
        h[index] = sum.error;
        q = sum.value;
    }
    h[elen] = q;
    return elen + 1;
}

// h = e + f, zero components eliminated; writes at most elen + flen
// (Fast-Expansion-Sum with zero elimination; needs round-to-nearest-even
// and strongly nonoverlapping inputs, which every function here produces).
template <typename T>
usize expansion_sum(const T* const e, const usize elen, const T* const f, const usize flen, T* const h) noexcept {
    usize e_index = 0;
    usize f_index = 0;
    usize h_count = 0;
    // The next component in order of magnitude, from e or f.
    T e_now = elen > 0 ? e[0] : T(0);
    T f_now = flen > 0 ? f[0] : T(0);
    T q = T(0);
    const bool take_e_first = flen == 0 || (elen > 0 && ((f_now > e_now) == (f_now > -e_now)));
    if (take_e_first) {
        q = e_now;
        ++e_index;
        if (e_index < elen) e_now = e[e_index];
    } else {
        q = f_now;
        ++f_index;
        if (f_index < flen) f_now = f[f_index];
    }
    if (e_index < elen && f_index < flen) {
        Pair<T> sum{};
        if ((f_now > e_now) == (f_now > -e_now)) {
            sum = component_fast_two_sum(e_now, q);
            ++e_index;
            if (e_index < elen) e_now = e[e_index];
        } else {
            sum = component_fast_two_sum(f_now, q);
            ++f_index;
            if (f_index < flen) f_now = f[f_index];
        }
        q = sum.value;
        if (sum.error != T(0)) h[h_count++] = sum.error;
        while (e_index < elen && f_index < flen) {
            if ((f_now > e_now) == (f_now > -e_now)) {
                sum = component_two_sum(q, e_now);
                ++e_index;
                if (e_index < elen) e_now = e[e_index];
            } else {
                sum = component_two_sum(q, f_now);
                ++f_index;
                if (f_index < flen) f_now = f[f_index];
            }
            q = sum.value;
            if (sum.error != T(0)) h[h_count++] = sum.error;
        }
    }
    while (e_index < elen) {
        const Pair<T> sum = component_two_sum(q, e_now);
        ++e_index;
        if (e_index < elen) e_now = e[e_index];
        q = sum.value;
        if (sum.error != T(0)) h[h_count++] = sum.error;
    }
    while (f_index < flen) {
        const Pair<T> sum = component_two_sum(q, f_now);
        ++f_index;
        if (f_index < flen) f_now = f[f_index];
        q = sum.value;
        if (sum.error != T(0)) h[h_count++] = sum.error;
    }
    if (q != T(0) || h_count == 0) h[h_count++] = q;
    STXLIB_FLOAT_REQUIRE(zero_or_normal(q), "float_exact::expansion_sum: component out of range");
    return h_count;
}

// The two-product form an expansion uses: the build's (fused where the
// whole build has a hardware fused multiply-add).
template <typename T>
[[nodiscard]] STXLIB_FLOAT_EXACT_INLINE Pair<T> expansion_product_step(const T a, const T b) noexcept {
    STXLIB_FLOAT_REQUIRE(product_below_overflow(a, b), "float_exact: an expansion's product step may overflow");
#if defined(STXLIB_FLOAT_EXACT_FUSED)
    const Pair<T> result = raw::two_product_fused(a, b);
#else
    const Pair<T> result = raw::two_product_dekker(a, b);
#endif
    return result;
}

// h = e * b, zero components eliminated; writes at most 2 * elen
// (Scale-Expansion with zero elimination).
template <typename T>
usize scale_expansion(const T* const e, const usize elen, const T b, T* const h) noexcept {
    STXLIB_FLOAT_REQUIRE(elen > 0, "float_exact::scale_expansion: empty expansion");
    usize h_count = 0;
    Pair<T> product = expansion_product_step(e[0], b);
    T q = product.value;
    if (product.error != T(0)) h[h_count++] = product.error;
    for (usize index = 1; index < elen; ++index) {
        product = expansion_product_step(e[index], b);
        const Pair<T> sum = component_two_sum(q, product.error);
        if (sum.error != T(0)) h[h_count++] = sum.error;
        const Pair<T> carried = component_fast_two_sum(product.value, sum.value);
        if (carried.error != T(0)) h[h_count++] = carried.error;
        q = carried.value;
    }
    if (q != T(0) || h_count == 0) h[h_count++] = q;
    return h_count;
}

// h = e * f; writes at most 2 * elen * flen. `scratch` holds at least
// 4 * elen * flen components (two running sums and one scaled term).
template <typename T>
usize expansion_product(const T* const e, const usize elen, const T* const f, const usize flen, T* const h,
                        T* const scratch) noexcept {
    T* running = scratch;
    T* next = scratch + 2 * elen * flen;
    T* const scaled = h;  // h doubles as the scaled term until the last sum
    usize running_count = scale_expansion(e, elen, f[0], running);
    for (usize index = 1; index < flen; ++index) {
        const usize scaled_count = scale_expansion(e, elen, f[index], scaled);
        const usize next_count = expansion_sum(running, running_count, scaled, scaled_count, next);
        T* const swap = running;
        running = next;
        next = swap;
        running_count = next_count;
    }
    for (usize index = 0; index < running_count; ++index) h[index] = running[index];
    return running_count;
}

// -1, 0 or +1: the sign of an expansion's value, which is the sign of its
// largest component (the components do not overlap).
template <typename T>
[[nodiscard]] int expansion_sign(const T* const e, const usize elen) noexcept {
    if (elen == 0) return 0;
    const T top = e[elen - 1];
    const int result = top > T(0) ? 1 : (top < T(0) ? -1 : 0);
    return result;
}

// ---- Compensated summation ----------------------------------------------------
//
// Order matters to both: the result is the same bits only for the same
// terms in the same order (evaluation rule 6). Every term zero or of
// magnitude at least 2^(emin + p) and at most 2^(emax - 1) (in_sum_range),
// and every partial sum finite. With terms so bounded, Neumaier's
// correction is an exact difference and Kahan's (next - sum) is at most
// 2^(emax - 1) (1 + 3u), so neither overflows while the partial sums are
// finite.

// Kahan's: error at most (2u + O(n u^2)) sum |x_i| (Higham, Accuracy and
// Stability of Numerical Algorithms, 2nd ed., section 4.3). 4 operations a
// term.
template <typename T>
class KahanSum final {
public:
    void add(const T term) noexcept {
        STXLIB_FLOAT_REQUIRE(in_sum_range(term), "float_exact::KahanSum: term out of range");
        const T corrected = term - compensation;
        const T next = sum + corrected;
        compensation = (next - sum) - corrected;
        sum = next;
        STXLIB_FLOAT_REQUIRE(finite(sum), "float_exact::KahanSum: the sum overflows");
    }
    [[nodiscard]] T value() const noexcept {
        return sum;
    }

private:
    T sum = T(0);
    T compensation = T(0);
};

// Neumaier's: also exact when a term exceeds the running sum; error at most
// u |S| + O(n u^2) sum |x_i|. 4 operations and a comparison a term.
template <typename T>
class NeumaierSum final {
public:
    void add(const T term) noexcept {
        STXLIB_FLOAT_REQUIRE(in_sum_range(term), "float_exact::NeumaierSum: term out of range");
        const T next = sum + term;
        if (magnitude(sum) >= magnitude(term)) {
            compensation += (sum - next) + term;
        } else {
            compensation += (term - next) + sum;
        }
        sum = next;
        STXLIB_FLOAT_REQUIRE(finite(sum), "float_exact::NeumaierSum: the sum overflows");
    }
    [[nodiscard]] T value() const noexcept {
        const T result = sum + compensation;
        return result;
    }

private:
    T sum = T(0);
    T compensation = T(0);
};

// ---- Exact geometric predicates ------------------------------------------------
//
// Shewchuk's floating-point filters (orient2d's A bound, incircle's A
// bound) answer when they can; otherwise the determinant is evaluated
// exactly in expansions and its sign returned. Shewchuk's intermediate
// adaptive stages (B, C) are not taken: the exact stage follows the filter
// directly, which is slower in the rare uncertain cases and simpler to
// argue. The answer is the exact sign in every case.
//
// Ranges (double): orient2d's coordinates zero or 2^-458 <= |x| <= 2^500;
// incircle's zero or 2^-200 <= |x| <= 2^250. A coordinate of magnitude at
// least 2^-458 is an integer multiple of 2^-510, so every difference and
// every component is too, and every product of two of them a multiple of
// 2^-1020: no product or error term is subnormal, and the filter's products
// round with relative error. Likewise incircle's degree-four terms are
// multiples of 2^-1008. The upper bounds meet the expansions' overflow
// conditions above with room: orient2d's exact products have exponent sums
// at most 1000 (<= emax - 2 = 1021) and factors at most 2^500 (split limit
// 2^995), and the six products sum in magnitude to below 2^1003; incircle's
// differences are at most 2^251, the lifts and cross terms at most 2^505 in
// component magnitude, their products' exponent sums at most 1010, and the
// whole determinant's components sum in magnitude to below 2^1014 (<=
// 2^(emax - 2)). Every two-sum operand is so at most 2^(emax - 1) and
// every product step within split's limit. The float forms evaluate in
// double, exactly (a float converts to double exactly), so any zero or
// normal float coordinate is in range.

template <typename T>
[[nodiscard]] constexpr bool in_coordinate_range(const T value, const T low, const T high) noexcept {
    const bool result = value == T(0) || (finite(value) && magnitude(value) >= low && magnitude(value) <= high);
    return result;
}

namespace detail {

// The exact orient2d determinant's sign: the six products of
// ax*by - ax*cy - cx*by - ay*bx + ay*cx + cy*bx, each exact as a two-
// component expansion, summed exactly.
[[nodiscard]] inline int orient2d_exact(const double ax, const double ay, const double bx, const double by,
                                        const double cx, const double cy) noexcept {
    const Pair<double> products[6] = {
        expansion_product_step(ax, by),  expansion_product_step(-ax, cy), expansion_product_step(-cx, by),
        expansion_product_step(-ay, bx), expansion_product_step(ay, cx),  expansion_product_step(cy, bx),
    };
    double terms[6][2]{};
    for (usize index = 0; index < 6; ++index) {
        terms[index][0] = products[index].error;
        terms[index][1] = products[index].value;
    }
    double first[4]{};
    double second[4]{};
    double third[4]{};
    const usize first_count = expansion_sum(terms[0], 2, terms[1], 2, first);
    const usize second_count = expansion_sum(terms[2], 2, terms[3], 2, second);
    const usize third_count = expansion_sum(terms[4], 2, terms[5], 2, third);
    double partial[8]{};
    const usize partial_count = expansion_sum(first, first_count, second, second_count, partial);
    double total[12]{};
    const usize total_count = expansion_sum(partial, partial_count, third, third_count, total);
    const int result = expansion_sign(total, total_count);
    return result;
}

// An exact difference a - b as a two-component expansion.
inline usize exact_difference(const double a, const double b, double (&out)[2]) noexcept {
    const Pair<double> difference = component_two_sum(a, -b);
    out[0] = difference.error;
    out[1] = difference.value;
    return 2;
}

// x*y - z*w for two-component expansions: at most 16 components.
inline usize cross_term(const double (&x)[2], const double (&y)[2], const double (&z)[2], const double (&w)[2],
                        double (&out)[16]) noexcept {
    double scratch[16]{};
    double left[8]{};
    double right[8]{};
    const usize left_count = expansion_product(x, 2, y, 2, left, scratch);
    const usize right_count = expansion_product(z, 2, w, 2, right, scratch);
    for (usize index = 0; index < right_count; ++index) right[index] = -right[index];
    const usize result = expansion_sum(left, left_count, right, right_count, out);
    return result;
}

// x*x + y*y for two-component expansions: at most 16 components.
inline usize lift_term(const double (&x)[2], const double (&y)[2], double (&out)[16]) noexcept {
    double scratch[16]{};
    double xx[8]{};
    double yy[8]{};
    const usize xx_count = expansion_product(x, 2, x, 2, xx, scratch);
    const usize yy_count = expansion_product(y, 2, y, 2, yy, scratch);
    const usize result = expansion_sum(xx, xx_count, yy, yy_count, out);
    return result;
}

// The exact incircle determinant's sign, from the exact differences to d:
// alift * (bdx cdy - cdx bdy) + blift * (cdx ady - adx cdy)
// + clift * (adx bdy - bdx ady). Uses about 37 KB of stack (the worst
// case of 1,536 components).
[[nodiscard]] inline int incircle_exact(const double ax, const double ay, const double bx, const double by,
                                        const double cx, const double cy, const double dx,
                                        const double dy) noexcept {
    double adx[2]{};
    double ady[2]{};
    double bdx[2]{};
    double bdy[2]{};
    double cdx[2]{};
    double cdy[2]{};
    static_cast<void>(exact_difference(ax, dx, adx));
    static_cast<void>(exact_difference(ay, dy, ady));
    static_cast<void>(exact_difference(bx, dx, bdx));
    static_cast<void>(exact_difference(by, dy, bdy));
    static_cast<void>(exact_difference(cx, dx, cdx));
    static_cast<void>(exact_difference(cy, dy, cdy));
    double cross[3][16]{};
    double lift[3][16]{};
    const usize cross_counts[3] = {cross_term(bdx, cdy, cdx, bdy, cross[0]), cross_term(cdx, ady, adx, cdy, cross[1]),
                                   cross_term(adx, bdy, bdx, ady, cross[2])};
    const usize lift_counts[3] = {lift_term(adx, ady, lift[0]), lift_term(bdx, bdy, lift[1]),
                                  lift_term(cdx, cdy, lift[2])};
    double scratch[1024]{};
    double term[512]{};
    double total[1536]{};
    double next[1536]{};
    usize total_count = 0;
    for (usize index = 0; index < 3; ++index) {
        const usize term_count =
            expansion_product(lift[index], lift_counts[index], cross[index], cross_counts[index], term, scratch);
        if (total_count == 0) {
            for (usize component = 0; component < term_count; ++component) total[component] = term[component];
            total_count = term_count;
        } else {
            const usize next_count = expansion_sum(total, total_count, term, term_count, next);
            for (usize component = 0; component < next_count; ++component) total[component] = next[component];
            total_count = next_count;
        }
    }
    const int result = expansion_sign(total, total_count);
    return result;
}

} // namespace detail

// +1 when a, b, c turn counterclockwise, -1 clockwise, 0 when collinear: the
// sign of (ax - cx)(by - cy) - (ay - cy)(bx - cx), exactly.
[[nodiscard]] inline int orient2d(const double ax, const double ay, const double bx, const double by,
                                  const double cx, const double cy) noexcept {
    STXLIB_FLOAT_REQUIRE(in_coordinate_range(ax, 0x1p-458, 0x1p500) && in_coordinate_range(ay, 0x1p-458, 0x1p500) &&
                             in_coordinate_range(bx, 0x1p-458, 0x1p500) &&
                             in_coordinate_range(by, 0x1p-458, 0x1p500) &&
                             in_coordinate_range(cx, 0x1p-458, 0x1p500) && in_coordinate_range(cy, 0x1p-458, 0x1p500),
                         "float_exact::orient2d: coordinate out of range");
    // Shewchuk's stage A: ccwerrboundA = (3 + 16 eps) eps.
    constexpr double eps = Format<double>::unit_roundoff;
    constexpr double bound_a = (3.0 + 16.0 * eps) * eps;
    const double det_left = (ax - cx) * (by - cy);
    const double det_right = (ay - cy) * (bx - cx);
    const double det = det_left - det_right;
    double det_sum = 0.0;
    if (det_left > 0.0) {
        if (det_right <= 0.0) return det > 0.0 ? 1 : (det < 0.0 ? -1 : 0);
        det_sum = det_left + det_right;
    } else if (det_left < 0.0) {
        if (det_right >= 0.0) return det > 0.0 ? 1 : (det < 0.0 ? -1 : 0);
        det_sum = -det_left - det_right;
    } else {
        return det > 0.0 ? 1 : (det < 0.0 ? -1 : 0);
    }
    const double error_bound = bound_a * det_sum;
    if (det >= error_bound) return 1;
    if (-det >= error_bound) return -1;
    const int result = detail::orient2d_exact(ax, ay, bx, by, cx, cy);
    return result;
}

[[nodiscard]] inline int orient2d(const float ax, const float ay, const float bx, const float by, const float cx,
                                  const float cy) noexcept {
    STXLIB_FLOAT_REQUIRE(zero_or_normal(ax) && zero_or_normal(ay) && zero_or_normal(bx) && zero_or_normal(by) &&
                             zero_or_normal(cx) && zero_or_normal(cy),
                         "float_exact::orient2d: coordinate out of range");
    const int result = orient2d(static_cast<double>(ax), static_cast<double>(ay), static_cast<double>(bx),
                                static_cast<double>(by), static_cast<double>(cx), static_cast<double>(cy));
    return result;
}

// +1 when d lies inside the circle through a, b, c (taken counterclockwise),
// -1 outside, 0 on it: the sign of the lifted 3x3 determinant, exactly.
[[nodiscard]] inline int incircle(const double ax, const double ay, const double bx, const double by,
                                  const double cx, const double cy, const double dx, const double dy) noexcept {
    STXLIB_FLOAT_REQUIRE(in_coordinate_range(ax, 0x1p-200, 0x1p250) && in_coordinate_range(ay, 0x1p-200, 0x1p250) &&
                             in_coordinate_range(bx, 0x1p-200, 0x1p250) &&
                             in_coordinate_range(by, 0x1p-200, 0x1p250) &&
                             in_coordinate_range(cx, 0x1p-200, 0x1p250) &&
                             in_coordinate_range(cy, 0x1p-200, 0x1p250) &&
                             in_coordinate_range(dx, 0x1p-200, 0x1p250) && in_coordinate_range(dy, 0x1p-200, 0x1p250),
                         "float_exact::incircle: coordinate out of range");
    // Shewchuk's stage A: iccerrboundA = (10 + 96 eps) eps. The added
    // 2^-1060 covers the absolute error of a product that underflows (a
    // lift times a difference of products can reach 2^-1060; each such
    // rounding errs by at most 2^-1075, and three are summed), which
    // Shewchuk's relative bound does not.
    constexpr double eps = Format<double>::unit_roundoff;
    constexpr double bound_a = (10.0 + 96.0 * eps) * eps;
    constexpr double underflow_slack = 0x1p-1060;
    const double adx = ax - dx;
    const double bdx = bx - dx;
    const double cdx = cx - dx;
    const double ady = ay - dy;
    const double bdy = by - dy;
    const double cdy = cy - dy;
    const double bdxcdy = bdx * cdy;
    const double cdxbdy = cdx * bdy;
    const double alift = adx * adx + ady * ady;
    const double cdxady = cdx * ady;
    const double adxcdy = adx * cdy;
    const double blift = bdx * bdx + bdy * bdy;
    const double adxbdy = adx * bdy;
    const double bdxady = bdx * ady;
    const double clift = cdx * cdx + cdy * cdy;
    const double det = alift * (bdxcdy - cdxbdy) + blift * (cdxady - adxcdy) + clift * (adxbdy - bdxady);
    const double permanent = (magnitude(bdxcdy) + magnitude(cdxbdy)) * alift +
                             (magnitude(cdxady) + magnitude(adxcdy)) * blift +
                             (magnitude(adxbdy) + magnitude(bdxady)) * clift;
    const double error_bound = bound_a * permanent + underflow_slack;
    if (det > error_bound) return 1;
    if (-det > error_bound) return -1;
    const int result = detail::incircle_exact(ax, ay, bx, by, cx, cy, dx, dy);
    return result;
}

[[nodiscard]] inline int incircle(const float ax, const float ay, const float bx, const float by, const float cx,
                                  const float cy, const float dx, const float dy) noexcept {
    STXLIB_FLOAT_REQUIRE(zero_or_normal(ax) && zero_or_normal(ay) && zero_or_normal(bx) && zero_or_normal(by) &&
                             zero_or_normal(cx) && zero_or_normal(cy) && zero_or_normal(dx) && zero_or_normal(dy),
                         "float_exact::incircle: coordinate out of range");
    const int result =
        incircle(static_cast<double>(ax), static_cast<double>(ay), static_cast<double>(bx), static_cast<double>(by),
                 static_cast<double>(cx), static_cast<double>(cy), static_cast<double>(dx), static_cast<double>(dy));
    return result;
}

} // namespace fused_form / dekker_form

} // namespace stxlib::float_exact
