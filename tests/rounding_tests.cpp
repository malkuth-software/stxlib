// Tests of rounding.hpp (M2 C6). The reference is the host's own <cmath>
// where the host is defined (a normal or zero argument whose rounding lies
// in range): std::round (halves away), std::nearbyint in the default
// rounding mode (halves to even), std::trunc, std::floor and std::ceil,
// each exact by IEEE 754 and C17 7.12.9, then converted to an integer
// (exact, in range). Outside the range, NaN and subnormals have STXlib's
// stated results and are checked against the definition.
//
// Properties:
//   R1  every float, each rule: round_to_i64 agrees with the reference or
//       the stated result (all 2^32 patterns with --exhaustive; every 4099th
//       pattern otherwise, a stride prime to every field width);
//   R2  doubles: every binade's boundaries (each exponent, the smallest and
//       largest significands, the halves around every integer magnitude up
//       to 2^53, the values next to +-2^31, +-2^52, +-2^53, +-2^63) and
//       2,000,000 patterns from a fixed sequence;
//   R3  round_to_i32 at its range's ends; the saturated readings;
//   R4  constant evaluation (static_assert).
// R1 with --exhaustive is a proof over all floats (enumeration); R2 is a
// sample, the ESBMC harness in proofs/esbmc/time_for_ui.cpp covers doubles
// once run.

#include "core/checked.hpp"
#include "stxlib/rounding.hpp"

#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>

namespace {

using stxlib::i64;
using stxlib::IntegerRounding;
using stxlib::Rounding;

constexpr Rounding rules[5] = {Rounding::nearest_ties_away, Rounding::nearest_ties_even, Rounding::toward_zero,
                               Rounding::down, Rounding::up};

static_assert(stxlib::round_to_i64(2.5, Rounding::nearest_ties_away) == IntegerRounding::of(3), "R4 halves away");
static_assert(stxlib::round_to_i64(2.5, Rounding::nearest_ties_even) == IntegerRounding::of(2), "R4 halves even");
static_assert(stxlib::round_to_i64(-2.5, Rounding::nearest_ties_away) == IntegerRounding::of(-3), "R4 negative half");
static_assert(stxlib::round_to_i64(-0.5, Rounding::nearest_ties_even) == IntegerRounding::of(0), "R4 -0.5 even");
static_assert(stxlib::round_to_i64(-1.25, Rounding::down) == IntegerRounding::of(-2), "R4 floor");
static_assert(stxlib::round_to_i64(-1.25, Rounding::up) == IntegerRounding::of(-1), "R4 ceil");
static_assert(stxlib::round_to_i64(1.75, Rounding::toward_zero) == IntegerRounding::of(1), "R4 trunc");
static_assert(stxlib::round_to_i64(9223372036854775808.0, Rounding::toward_zero) == IntegerRounding::above_range(),
              "R4 2^63 is above");
static_assert(stxlib::round_to_i64(-9223372036854775808.0, Rounding::toward_zero) ==
                  IntegerRounding::of(stxlib::i64_min),
              "R4 -2^63 is in range");
static_assert(stxlib::round_to_i64(__builtin_nan(""), Rounding::up) == IntegerRounding::not_a_number(), "R4 NaN");
static_assert(stxlib::round_to_i64(-__builtin_inf(), Rounding::up) == IntegerRounding::below_range(), "R4 -inf");
static_assert(stxlib::round_to_i64(4.9406564584124654e-324, Rounding::up) == IntegerRounding::of(0),
              "R4 a subnormal is zero");
static_assert(stxlib::round_to_i32(2147483647.4, Rounding::nearest_ties_away) == IntegerRounding::of(2147483647),
              "R4 i32 top");
static_assert(stxlib::round_to_i32(2147483647.5, Rounding::nearest_ties_away) == IntegerRounding::above_range(),
              "R4 i32 above");
static_assert(stxlib::round_to_i32_saturated(-1e300) == -2147483647 - 1, "R4 i32 saturates");
static_assert(stxlib::round_to_i64_saturated(1e300) == stxlib::i64_max, "R4 i64 saturates");

std::uint64_t mismatches = 0;
std::uint64_t comparisons = 0;

void expect(const bool condition, const char* const property, const double x, const int rule) {
    comparisons = comparisons + 1;
    if (condition) return;
    mismatches = mismatches + 1;
    if (mismatches <= 20) std::fprintf(stderr, "%s mismatch at %.17g (rule %d)\n", property, x, rule);
}

double host_round(const double x, const Rounding rule) {
    switch (rule) {
    case Rounding::nearest_ties_away:
        return std::round(x);
    case Rounding::nearest_ties_even:
        return std::nearbyint(x); // the default mode, never changed here
    case Rounding::toward_zero:
        return std::trunc(x);
    case Rounding::down:
        return std::floor(x);
    case Rounding::up:
        return std::ceil(x);
    }
    return x;
}

// The stated result of `x` (as a double: every float converts exactly).
IntegerRounding expected_of(const double x, const bool subnormal, const Rounding rule) {
    if (std::isnan(x)) return IntegerRounding::not_a_number();
    const double y = subnormal ? 0.0 : x;
    const double rounded = host_round(y, rule);
    // [-2^63, 2^63): exactly the doubles in range.
    if (rounded >= 9223372036854775808.0) return IntegerRounding::above_range();
    if (rounded < -9223372036854775808.0) return IntegerRounding::below_range();
    return IntegerRounding::of(static_cast<i64>(rounded));
}

void check_double(const double x, const char* const property) {
    const bool subnormal = std::fpclassify(x) == FP_SUBNORMAL;
    for (int index = 0; index < 5; ++index) {
        const IntegerRounding got = stxlib::round_to_i64(x, rules[index]);
        expect(got == expected_of(x, subnormal, rules[index]), property, x, index);
    }
}

void check_float(const float x) {
    const bool subnormal = std::fpclassify(x) == FP_SUBNORMAL;
    for (int index = 0; index < 5; ++index) {
        const IntegerRounding got = stxlib::round_to_i64(x, rules[index]);
        expect(got == expected_of(static_cast<double>(x), subnormal, rules[index]), "R1", static_cast<double>(x),
               index);
    }
}

void every_float(const bool exhaustive) {
    const std::uint64_t stride = exhaustive ? 1 : 4099;
    for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += stride) {
        const std::uint32_t bits = static_cast<std::uint32_t>(pattern);
        float x = 0.0f;
        std::memcpy(&x, &bits, sizeof x);
        check_float(x);
    }
}

double from_bits(const std::uint64_t bits) {
    double x = 0.0;
    std::memcpy(&x, &bits, sizeof x);
    return x;
}

// SplitMix64, arithmetic modulo 2^64 by definition.
STXLIB_MODULAR std::uint64_t split_mix_64(std::uint64_t& state) {
    state = state + 0x9e3779b97f4a7c15ULL;
    std::uint64_t mixed = state;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
    mixed = mixed ^ (mixed >> 31);
    return mixed;
}

void double_boundaries() {
    for (std::uint64_t biased = 0; biased <= 0x7ff; ++biased) {
        for (const std::uint64_t sign : {0ULL, 1ULL}) {
            const std::uint64_t base = (sign << 63) | (biased << 52);
            const std::uint64_t mantissas[6] = {0, 1, 2, 1ULL << 51, (1ULL << 52) - 2, (1ULL << 52) - 1};
            for (const std::uint64_t mantissa : mantissas) check_double(from_bits(base | mantissa), "R2 binade");
        }
    }
    // Halves around integer magnitudes, up to 2^53 (where halves stop being
    // representable), and their neighbours.
    for (double magnitude = 1.0; magnitude <= 9007199254740992.0; magnitude = magnitude * 2.0) {
        const double points[4] = {magnitude - 0.5, magnitude + 0.5, magnitude - 1.5, magnitude + 1.5};
        for (const double point : points) {
            check_double(point, "R2 halves");
            check_double(-point, "R2 halves");
            check_double(std::nextafter(point, 0.0), "R2 halves");
            check_double(std::nextafter(point, 1e300), "R2 halves");
            check_double(-std::nextafter(point, 0.0), "R2 halves");
        }
    }
    const double edges[5] = {2147483648.0, 4503599627370496.0, 9007199254740992.0, 9223372036854775808.0, 0.5};
    for (const double edge : edges) {
        for (const double value : {edge, -edge}) {
            check_double(value, "R2 edge");
            check_double(std::nextafter(value, 0.0), "R2 edge");
            check_double(std::nextafter(value, value > 0 ? 1e300 : -1e300), "R2 edge");
        }
    }
    // A fixed sequence of patterns (SplitMix64 from 1): a sample.
    std::uint64_t state = 1;
    for (int index = 0; index < 2000000; ++index) {
        const std::uint64_t mixed = split_mix_64(state);
        // Bias the exponent toward the interesting range [2^-2, 2^64].
        const std::uint64_t biased = 1021 + ((mixed >> 52) % 66);
        const std::uint64_t pattern = (mixed & 0x800fffffffffffffULL) | (biased << 52);
        check_double(from_bits(pattern), "R2 sample");
    }
}

void thirty_two_bits() {
    CHECK(stxlib::round_to_i32(-2147483648.0, Rounding::toward_zero) == IntegerRounding::of(-2147483647LL - 1));
    CHECK(stxlib::round_to_i32(-2147483648.5, Rounding::nearest_ties_even) == IntegerRounding::of(-2147483647LL - 1));
    CHECK(stxlib::round_to_i32(-2147483648.5, Rounding::nearest_ties_away) == IntegerRounding::below_range());
    CHECK(stxlib::round_to_i32(2147483647.9f, Rounding::down) == IntegerRounding::above_range()); // the float is 2^31
    CHECK(stxlib::round_to_i32(std::numeric_limits<double>::quiet_NaN(), Rounding::up).is_not_a_number());
    const IntegerRounding nan = stxlib::round_to_i64(std::numeric_limits<double>::quiet_NaN(), Rounding::up);
    CHECK(nan.value() == nullptr);
    CHECK(nan.saturated(stxlib::i64_min, stxlib::i64_max) == 0);
    CHECK(nan.saturated(stxlib::i64_min, stxlib::i64_max, -1) == -1);
    const IntegerRounding seven = stxlib::round_to_i64(6.5, Rounding::nearest_ties_away);
    CHECK(seven.in_range() && seven.value() != nullptr && *seven.value() == 7);
    CHECK(stxlib::round_to_i32_saturated(1e10) == 2147483647);
    CHECK(stxlib::round_to_i64_saturated(-std::numeric_limits<double>::infinity()) == stxlib::i64_min);
}

} // namespace

int main(const int argc, char** const argv) {
    const bool exhaustive = argc > 1 && std::strcmp(argv[1], "--exhaustive") == 0;
    every_float(exhaustive);
    double_boundaries();
    thirty_two_bits();
    std::printf("stxlib_rounding_tests: %llu comparisons, %llu mismatches%s\n",
                static_cast<unsigned long long>(comparisons), static_cast<unsigned long long>(mismatches),
                exhaustive ? " (every float)" : "");
    const int harness = mismatches == 0 ? 0 : 1;
    const int checks = stxlib_tests::finish("stxlib_rounding_tests");
    return harness | checks;
}
