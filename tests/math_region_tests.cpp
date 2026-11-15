// Tests of the two paths of the switched math kernels
// (src/algorithms/math/paths.hpp): the region kernels (region.cpp and
// trig_region.cpp), each checked against the double-double references of
// tests/reference/wide.hpp, and compared with today's kernel for the same
// function ("current": today's path by name, with flush.cpp's subnormal rule
// around it, so the comparison and the bench hold whichever path the build's
// switches give the public functions).
//
//   math_region_tests               sampled checks (the ctest run)
//   math_region_tests --exhaustive  every float through the float
//                                   logarithms, 20 million doubles per pow
//                                   region, 100 million float pow and hypot
//                                   pairs
//   math_region_tests --bench       ns per call, region against today's
//
// It calls the library's variant (CMakeLists.txt: stxlib_math_region_tests;
// v128 where the build dispatches) and, in stxlib_math_region_dekker_tests,
// its own copies with Dekker's product; both must print the same hashes.

#include "stxlib/math.hpp"

#include "reference/sincos_worst_cases.hpp"
#include "reference/wide.hpp"
#include "core/checked.hpp"

#if !defined(STXLIB_MATH_VARIANT)
#define STXLIB_MATH_VARIANT native
#endif
#include "algorithms/math/region_functions.hpp"
#include "core/subnormal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace region = stxlib::math::STXLIB_MATH_VARIANT::region;
namespace today = stxlib::math::STXLIB_MATH_VARIANT::today;

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

// Today's kernels by name, with flush.cpp's subnormal rule around them: the
// public functions on today's path, whichever path this build's switches
// give the public functions.
double flushed(const double value) { return ::stxlib::detail::flush_subnormal(value); }
float flushed_float(const float value) { return ::stxlib::detail::flush_subnormal(value); }

double current_pow(const double x, const double y) { return flushed(today::pow(flushed(x), flushed(y))); }
float current_pow(const float x, const float y) {
    return flushed_float(today::pow(flushed_float(x), flushed_float(y)));
}
float current_log(const float x) { return flushed_float(today::log(flushed_float(x))); }
float current_log2(const float x) { return flushed_float(today::log2(flushed_float(x))); }
float current_log10(const float x) { return flushed_float(today::log10(flushed_float(x))); }
float current_hypot(const float x, const float y) {
    return flushed_float(today::hypot(flushed_float(x), flushed_float(y)));
}
void current_sincos(const double x, double& s, double& c) {
    double kernel_s = 0.0;
    double kernel_c = 0.0;
    today::sincos(flushed(x), kernel_s, kernel_c);
    s = flushed(kernel_s);
    c = flushed(kernel_c);
}
double current_tan(const double x) { return flushed(today::tan(flushed(x))); }

// Deterministic inputs: a 64-bit LCG (as tests/math_tests.cpp's).
struct Lcg final {
    std::uint64_t state{0};

    STXLIB_MODULAR double unit() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double result = static_cast<double>(state >> 11) * 0x1.0p-53;
        return result;
    }

    STXLIB_MODULAR std::uint64_t bits() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t high = state >> 32;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t result = (high << 32) | (state >> 32);
        return result;
    }
};

// The proved bound of region::pow(double): 0.500097 ulp
// (tools/math/region_certificate.py pow), with room for the reference.
constexpr double region_pow_ulp_limit = 0.5001;

bool same_bits(const double a, const double b) {
    const bool both_nan = std::isnan(a) && std::isnan(b);
    const bool result = both_nan || std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    return result;
}

bool same_bits(const float a, const float b) {
    const bool both_nan = std::isnan(a) && std::isnan(b);
    const bool result = both_nan || std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    return result;
}

float flushed(const float value) {
    if (std::fpclassify(value) == FP_SUBNORMAL) return std::copysign(0.0f, value);
    return value;
}

// hi + lo rounded once to float (as tests/math_tests.cpp's).
float correctly_rounded_float(const wide::Wide exact) {
    const float rounded = static_cast<float>(exact.hi);
    const double error = (exact.hi - static_cast<double>(rounded)) + exact.lo;
    const float up = std::nextafter(rounded, std::numeric_limits<float>::infinity());
    const float down = std::nextafter(rounded, -std::numeric_limits<float>::infinity());
    const double to_up = static_cast<double>(up) - static_cast<double>(rounded);
    const double to_down = static_cast<double>(rounded) - static_cast<double>(down);
    if (error > to_up / 2.0) return up;
    if (-error > to_down / 2.0) return down;
    return rounded;
}

double scaled_ulps(const double approx, const wide::ScaledWide exact) {
    const double scaled = std::ldexp(approx, -exact.exponent);
    const double rounded = std::fabs(exact.mantissa.hi + exact.mantissa.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{scaled, 0.0}, wide::negate(exact.mantissa));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

double correctly_rounded_subnormal(const wide::ScaledWide exact) {
    const int shift = exact.exponent + 1074;
    const double units = std::ldexp(exact.mantissa.hi, shift);
    const double units_lo = std::ldexp(exact.mantissa.lo, shift);
    double nearest = std::nearbyint(units);
    const double remainder = (units - nearest) + units_lo;
    if (remainder > 0.5) nearest += 1.0;
    if (remainder < -0.5) nearest -= 1.0;
    const double result = std::ldexp(nearest, -1074);
    return result;
}

// x^y = e^(y log x) for positive x.
wide::ScaledWide pow_reference(const double x, const double y) {
    const wide::Wide log_x = wide::log_reference(x);
    const wide::ScaledWide result = wide::exp_reference(wide::multiply(log_x, y));
    return result;
}

// ---- pow(double) --------------------------------------------------------------

// The regions of tests/math_tests.cpp's pow_input (the exponents scaled by
// the library's own log, so the inputs do not depend on the host).
void pow_input(const int region_index, Lcg& lcg, double& x, double& y) {
    if (region_index == 0) {
        x = 0.5 + 1.5 * lcg.unit();
        y = (lcg.unit() - 0.5) * 200.0;
    } else if (region_index == 1) {
        const double exponent = std::floor(lcg.unit() * 2000.0) - 1000.0;
        x = std::ldexp(1.0 + lcg.unit(), static_cast<int>(exponent));
        y = (lcg.unit() - 0.5) * 1400.0 / std::fabs(stxlib::math::log(x));
    } else if (region_index == 2) {
        x = 1.0 + (lcg.unit() - 0.5) * 2e-10;
        y = (lcg.unit() - 0.5) * 2e10;
    } else if (region_index == 3) {
        x = 1.0 + lcg.unit();
        y = 709.0 / stxlib::math::log(x) * (0.999 + 0.0012 * lcg.unit());
    } else if (region_index == 4) {
        x = 1.0 + lcg.unit();
        y = -1.0 * (708.5 + 36.5 * lcg.unit()) / stxlib::math::log(x);
    } else {
        // Near 1, where the current kernel's log2 loses most (buckets 79 to
        // 82): x within 2^-7 of 1, |y log x| up to 700.
        x = 1.0 + (lcg.unit() - 0.5) * 0x1p-6;
        y = (lcg.unit() - 0.5) * 1400.0 / std::fabs(stxlib::math::log(x));
    }
}

constexpr int pow_region_count = 6;
const char* const pow_region_names[pow_region_count] = {"x in [0.5, 2]", "any x", "x = 1 +- 1e-10",
                                                        "near overflow", "subnormal", "x = 1 +- 2^-7"};

// Each region: the region kernel's worst error (it must meet the proved
// bound) beside the current kernel's on the same inputs.
void pow_double_accuracy(const int per_region) {
    Lcg lcg{2030};
    for (int region_index = 0; region_index < pow_region_count; ++region_index) {
        double worst = 0.0;
        double worst_current = 0.0;
        double worst_x = 0.0;
        double worst_y = 0.0;
        std::uint64_t misrounded = 0;
        std::uint64_t misrounded_current = 0;
        int measured = 0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region_index, lcg, x, y);
            const double result = region::pow(x, y);
            const double current = current_pow(x, y);
            const wide::ScaledWide exact = pow_reference(x, y);
            require(result == 0.0 || result >= 0x1p-1022, "region pow(double): a subnormal result");
            if (result == 0.0 || !std::isfinite(result)) {
                // Flushed or overflowed: the correctly rounded value must be
                // subnormal (or within half a subnormal step of 2^-1022) or
                // above the largest double.
                if (result == 0.0) {
                    require(std::signbit(result) == false, "region pow(double): -0 for a positive base");
                    require(correctly_rounded_subnormal(exact) < 0x1p-1022 + 0x1p-1074,
                            "region pow(double): a normal result flushed");
                } else {
                    // x^y at least the largest double: half of it at least half of that.
                    const double half = std::ldexp(exact.mantissa.hi + exact.mantissa.lo, exact.exponent - 1);
                    require(half >= 0x1.fffffffffffffp1022, "region pow(double): overflow below the largest double");
                }
                continue;
            }
            const double error = scaled_ulps(result, exact);
            const double error_current =
                (std::isfinite(current) && current >= 0x1p-1022) ? scaled_ulps(current, exact) : 0.0;
            if (error > 0.5) misrounded += 1;
            if (error_current > 0.5) misrounded_current += 1;
            measured += 1;
            worst_current = std::max(worst_current, error_current);
            if (error > worst) {
                worst = error;
                worst_x = x;
                worst_y = y;
            }
        }
        std::printf("pow(double) %-15s %10d inputs  region max %.6f ulp (%llu above 0.5) at (%a, %a);  "
                    "current max %.6f ulp (%llu above 0.5)\n",
                    pow_region_names[region_index], measured, worst, static_cast<unsigned long long>(misrounded),
                    worst_x, worst_y, worst_current, static_cast<unsigned long long>(misrounded_current));
        require(worst <= region_pow_ulp_limit, "region pow(double): error above the proved bound");
    }
}

// Every pair of a grid of special and boundary arguments: the region kernel
// must give the current function's bits wherever the result is exact (Annex
// F's cases, the subnormal rule's zeros) and be within the bound elsewhere.
void pow_double_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double xs[] = {0.0,    -0.0,  0x1p-1074, -0x1p-1074, 0x1p-1022, -0x1p-1022, 0.5,     -0.5,
                         1.0,    -1.0,  2.0,       -2.0,       3.0,       -3.0,       0x1p1023, -0x1p1023,
                         10.0,   0.1,   1.0 + 0x1p-52, 1.0 - 0x1p-53, infinity, -infinity, nan,  0x1.fffffffffffffp1023};
    const double ys[] = {0.0,      -0.0,    0x1p-1074, -0x1p-1074, 0.5,          -0.5,       1.0,      -1.0,
                         2.0,      -2.0,    3.0,       -3.0,       0x1p-80,      -0x1p-80,   0x1p-81,  0x1p64,
                         -0x1p64,  0x1p63,  0x1.fffffffffffffp63,  1023.0,       -1074.0,    1e300,    -1e300,
                         infinity, -infinity, nan,     0x1p53,     0x1p53 + 2.0, 0x1p52 + 1.0};
    int checked = 0;
    for (const double x : xs) {
        for (const double y : ys) {
            const double result = region::pow(x, y);
            const double current = current_pow(x, y);
            checked += 1;
            if (same_bits(result, current)) continue;
            // Inexact finite results may differ in their rounding; both
            // must then be normal, of one sign, and the region's within its
            // bound.
            require(std::isfinite(result) && std::isfinite(current) && result != 0.0 && current != 0.0 &&
                        std::signbit(result) == std::signbit(current),
                    "region pow(double): an exact special case differs from the current function");
            const wide::ScaledWide exact = pow_reference(std::fabs(x), y);
            require(scaled_ulps(std::fabs(result), exact) <= region_pow_ulp_limit,
                    "region pow(double): a boundary case above the bound");
        }
    }
    // Exact powers.
    require(region::pow(2.0, 10.0) == 1024.0 && region::pow(10.0, 2.0) == 100.0 && region::pow(4.0, 0.5) == 2.0 &&
                region::pow(-2.0, 3.0) == -8.0 && region::pow(1.0, nan) == 1.0 && region::pow(nan, 0.0) == 1.0,
            "region pow(double): an exact case");
    std::printf("pow(double) special and boundary cases agree (%d pairs)\n", checked);
}

// ---- pow(float) ----------------------------------------------------------------

// x^y as mantissa 2^exponent rounded to float, then flushed: valid while the
// reference's parts stay normal doubles once scaled (results >= 2^-150).
float pow_float_expected(const wide::ScaledWide exact) {
    const wide::Wide value{std::ldexp(exact.mantissa.hi, exact.exponent),
                           std::ldexp(exact.mantissa.lo, exact.exponent)};
    const float result = flushed(correctly_rounded_float(value));
    return result;
}

// The distance from x^y to the nearest float midpoint, relative. The region
// kernel's float pow sends a pair within 2^-67 of a midpoint to stages 2 and
// 3 (region.cpp, region_pow_float_positive); correct rounding is unproved
// only within 2^-94 of a midpoint, not at one. Both are counted, for the
// record: every pair must be correctly rounded either way.
double midpoint_distance(const wide::ScaledWide exact) {
    const double hi = exact.mantissa.hi;
    const double lo = exact.mantissa.lo;
    int exponent = 0;
    (void)std::frexp(hi, &exponent);
    const double grid = std::ldexp(1.0, exponent - 25);  // half a float ulp of hi's binade
    const double nearest_half = std::nearbyint(hi / grid);
    const double odd_half = std::fmod(nearest_half, 2.0) != 0.0 ? nearest_half
                            : (hi / grid > nearest_half ? nearest_half + 1.0 : nearest_half - 1.0);
    const double midpoint = odd_half * grid;
    const double result = std::fabs((hi - midpoint) + lo) / std::fabs(hi);
    return result;
}

void pow_float_check(const float x, const float y, int& checked, int& near_midpoints, int& unproved) {
    const float result = region::pow(x, y);
    if (!(x > 0.0f) || !std::isfinite(result)) return;
    const wide::ScaledWide exact = pow_reference(static_cast<double>(x), static_cast<double>(y));
    if (exact.exponent < -150 || exact.exponent > 129) return;
    const float expected = pow_float_expected(exact);
    require(same_bits(result, expected), "region pow(float): result is not correctly rounded");
    // The midpoint grid of a normal result is that of its mantissa scaled.
    const double distance = midpoint_distance(exact);
    if (expected != 0.0f && distance <= 0x1p-67) near_midpoints += 1;
    if (expected != 0.0f && distance <= 0x1p-94) unproved += 1;
    checked += 1;
}

// Random pairs: results across float's range, normal and flushed.
void pow_float_rounding(const int count) {
    Lcg lcg{2031};
    int checked = 0;
    int near_midpoints = 0;
    int unproved = 0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(lcg.unit() * 16.0);
        const float y = static_cast<float>((lcg.unit() - 0.5) * 80.0);
        pow_float_check(x, y, checked, near_midpoints, unproved);
        // Any positive normal x, y scaled so that x^y spans float's range.
        const std::uint32_t x_bits = 0x00800000U + static_cast<std::uint32_t>(lcg.bits() % 0x7f000000U);
        const float wide_x = std::bit_cast<float>(x_bits);
        const double log2_x = std::log2(static_cast<double>(wide_x));
        if (log2_x != 0.0) {
            const float wide_y = static_cast<float>((lcg.unit() - 0.5) * 260.0 / log2_x);
            pow_float_check(wide_x, wide_y, checked, near_midpoints, unproved);
        }
    }
    std::printf("pow(float) correctly rounded at %d random pairs; %d within 2^-67 of a midpoint (stages 2 and 3), "
                "%d within 2^-94\n",
                checked, near_midpoints, unproved);
}

// Stages 2 and 3 of float pow on every pair (region::pow_float_decided, the
// stage 1 test skipped), and the accurate stage's error against the
// reference (sampled; tools/math/pow_float_accurate_certificate.py proves
// 2^-94.10). The reference (tests/reference/wide.hpp) carries log x with an
// absolute error near 2^-104 (its e^y0 in double-double), which |y|
// multiplies: near x = 1.0039 with |y| = 2^13 it is off by 2^-90.5 while the
// stage is within 2^-98.3 (round 2's failure; replayed operation for
// operation against the 400-bit reference by
// tools/math/pow_float_accurate_replay.py). So the error is checked against
// 2^-93 only for |y| <= 64, where the replayed reference is good to 2^-98.09
// (198,616 replayed pairs, none where the test could see more than 2^-93);
// every pair's rounding is checked.
constexpr float accurate_checked_y = 64.0f;

void pow_float_accurate_check(const float x, const float y, int& checked, double& worst,
                              double& worst_unchecked) {
    if (!(x > 0.0f) || !std::isfinite(x) || !std::isnormal(y)) return;
    const wide::ScaledWide exact = pow_reference(static_cast<double>(x), static_cast<double>(y));
    if (exact.exponent < -125 || exact.exponent > 128) return;  // x^y in [2^-126, 2^128)
    const float expected = pow_float_expected(exact);
    if (!std::isfinite(expected) || !(expected >= 0x1p-126f)) return;
    require(same_bits(region::pow_float_decided(x, y), expected),
            "region pow(float): stages 2 and 3 do not round correctly");
    double hi = 0.0;
    double lo = 0.0;
    region::pow_float_accurate_value(x, y, hi, lo);
    const double exact_hi = std::ldexp(exact.mantissa.hi, exact.exponent);
    const double exact_lo = std::ldexp(exact.mantissa.lo, exact.exponent);
    const double error = std::fabs(((hi - exact_hi) + (lo - exact_lo)) / exact_hi);  // hi - exact_hi exact
    checked += 1;
    if (std::fabs(y) > accurate_checked_y) {
        worst_unchecked = std::fmax(worst_unchecked, error);
        return;
    }
    worst = std::fmax(worst, error);
    require(error <= 0x1p-93, "region pow(float): the accurate stage above its bound");
}

void pow_float_accurate_stage(const int count) {
    Lcg lcg{2032};
    int checked = 0;
    double worst = 0.0;
    double worst_unchecked = 0.0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(lcg.unit() * 16.0);
        const float y = static_cast<float>((lcg.unit() - 0.5) * 80.0);
        pow_float_accurate_check(x, y, checked, worst, worst_unchecked);
        const std::uint32_t x_bits = 0x00800000U + static_cast<std::uint32_t>(lcg.bits() % 0x7f000000U);
        const float wide_x = std::bit_cast<float>(x_bits);
        const double log2_x = std::log2(static_cast<double>(wide_x));
        if (log2_x != 0.0) {
            const float wide_y = static_cast<float>((lcg.unit() - 0.5) * 250.0 / log2_x);
            pow_float_accurate_check(wide_x, wide_y, checked, worst, worst_unchecked);
        }
    }
    // x just above 1, where the log side's bound is tightest (bucket 81), and
    // large y: rounding checked here; the error, beyond the reference's reach,
    // is replayed against the 400-bit reference by
    // tools/math/pow_float_accurate_replay.py.
    for (int i = 0; i < 4096; ++i) {
        const float x = std::bit_cast<float>(0x3f808000U + static_cast<std::uint32_t>(lcg.bits() % 0x10000U));
        const float y = static_cast<float>((lcg.unit() - 0.5) * 250.0 / std::log2(static_cast<double>(x)));
        pow_float_accurate_check(x, y, checked, worst, worst_unchecked);
    }
    // A hard case from the CORE-MATH project's slides (ARITH 2022): x^y =
    // 0x1.15f75e00000000000000058b...p-1, a float followed by 61 zero bits.
    const float hard_x = 0x1.46ee2p+67f;
    const float hard_y = -0x1.acbb3ap-7f;
    require(same_bits(region::pow(hard_x, hard_y), 0x1.15f75ep-1f) &&
                same_bits(region::pow_float_decided(hard_x, hard_y), 0x1.15f75ep-1f),
            "region pow(float): the CORE-MATH hard case");
    pow_float_accurate_check(hard_x, hard_y, checked, worst, worst_unchecked);
    // The exact midpoints go through stage 2.
    require(same_bits(region::pow_float_decided(4097.0f, 2.0f), 16785408.0f) &&
                same_bits(region::pow_float_decided(66049.0f, 1.5f), 16974592.0f) &&
                same_bits(region::pow_float_decided(67081.0f, 1.5f), 17373980.0f),
            "region pow(float): stage 2 misses an exact midpoint");
    std::printf("pow(float) stages 2 and 3 correctly rounded at %d pairs; accurate stage worst 2^%.2f for |y| <= 64, "
                "2^%.2f beyond (the reference's error, not checked)\n",
                checked, worst > 0.0 ? std::log2(worst) : -1000.0,
                worst_unchecked > 0.0 ? std::log2(worst_unchecked) : -1000.0);
}

// Exact midpoints round to even: x^y = m with 25 significant bits.
void pow_float_midpoints() {
    struct Midpoint final {
        float x{0.0f};
        float y{0.0f};
        float expected{0.0f};
    };
    const Midpoint cases[] = {
        {4097.0f, 2.0f, 16785408.0f},             // 4097^2 = 16785409: down to even
        {257.0f, 3.0f, 16974592.0f},              // 257^3 = 16974593: down
        {66049.0f, 1.5f, 16974592.0f},            // (257^2)^1.5 = 257^3
        {259.0f, 3.0f, 17373980.0f},              // 259^3 = 17373979: up to even
        {67081.0f, 1.5f, 17373980.0f},            // (259^2)^1.5 = 259^3
        {-259.0f, 3.0f, -17373980.0f},            // odd y keeps the sign
        {-4097.0f, 2.0f, 16785408.0f},            // even y drops it
    };
    int checked = 0;
    for (const Midpoint& item : cases) {
        const float result = region::pow(item.x, item.y);
        require(same_bits(result, item.expected), "region pow(float): an exact midpoint does not round to even");
        checked += 1;
    }
    // The same midpoints scaled by powers of two: (259 2^-j)^3 = 259^3 2^-3j.
    for (int j = -40; j <= 40; ++j) {
        const float x = std::ldexp(259.0f, j);
        const float expected = std::ldexp(17373980.0f, 3 * j);
        if (!(std::fabs(expected) >= 0x1p-126f) || !std::isfinite(expected)) continue;
        require(same_bits(region::pow(x, 3.0f), expected), "region pow(float): a scaled midpoint");
        const float x2 = std::ldexp(4097.0f, j);
        const float expected2 = std::ldexp(16785408.0f, 2 * j);
        if (std::fabs(expected2) >= 0x1p-126f && std::isfinite(expected2)) {
            require(same_bits(region::pow(x2, 2.0f), expected2), "region pow(float): a scaled midpoint");
        }
        checked += 2;
    }
    std::printf("pow(float) exact midpoints round to even (%d cases)\n", checked);
}

void pow_float_special_values() {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float xs[] = {0.0f, -0.0f, 0x1p-149f, -0x1p-149f, 0x1p-126f, -0x1p-126f, 0.5f, -0.5f, 1.0f, -1.0f,
                        2.0f, -2.0f, 3.0f, -3.0f, 0x1.fffffep127f, -0x1.fffffep127f, infinity, -infinity, nan};
    const float ys[] = {0.0f,  -0.0f, 0x1p-149f, -0x1p-149f, 0x1p-126f, 0.5f,       -0.5f,     1.0f,
                        -1.0f, 2.0f,  -2.0f,     3.0f,       -3.0f,     0x1p24f,    0x1p24f + 2.0f, 0x1p23f + 1.0f,
                        127.0f, -126.0f, 0x1.fffffep127f, -0x1.fffffep127f, infinity, -infinity, nan};
    int checked = 0;
    int differ = 0;
    for (const float x : xs) {
        for (const float y : ys) {
            const float result = region::pow(x, y);
            const float current = current_pow(x, y);
            checked += 1;
            if (same_bits(result, current)) continue;
            differ += 1;
            // Both claim correct rounding: decide which is right.
            require(std::isfinite(x) && x != 0.0f && std::isfinite(result),
                    "region pow(float): a special case differs from the current function");
            const wide::ScaledWide exact = pow_reference(std::fabs(static_cast<double>(x)), static_cast<double>(y));
            const float magnitude = pow_float_expected(exact);
            const float expected = std::signbit(result) ? -magnitude : magnitude;
            require(same_bits(result, expected), "region pow(float): a boundary case not correctly rounded");
        }
    }
    std::printf("pow(float) special and boundary cases agree (%d pairs, %d differ from the current, region right)\n",
                checked, differ);
}

// ---- float logarithms -----------------------------------------------------------

using LogFloat = float (*)(float);

struct LogPair final {
    const char* name{nullptr};
    LogFloat region_function{nullptr};
    LogFloat current_function{nullptr};
    double base{0.0};  // 0 for e
};

float region_log_f(const float x) { return region::log(x); }
float region_log2_f(const float x) { return region::log2(x); }
float region_log10_f(const float x) { return region::log10(x); }
float current_log_f(const float x) { return current_log(x); }
float current_log2_f(const float x) { return current_log2(x); }
float current_log10_f(const float x) { return current_log10(x); }

const LogPair log_pairs[] = {
    {"log", region_log_f, current_log_f, 0.0},
    {"log2", region_log2_f, current_log2_f, 2.0},
    {"log10", region_log10_f, current_log10_f, 10.0},
};

float log_float_expected(const LogPair& pair, const float x) {
    const wide::Wide natural = wide::log_reference(static_cast<double>(x));
    wide::Wide value = natural;
    if (pair.base != 0.0) value = wide::divide(natural, wide::log_reference(pair.base));
    const float result = correctly_rounded_float(value);
    return result;
}

// Every float pattern at the stride (1 with --exhaustive: all 2^32): the
// region function must give the current function's bits, which
// tests/math_tests.cpp --exhaustive proves correctly rounded for every
// float (and the subnormal rule's results for the rest). A difference is
// decided against the reference and reported either way.
void log_float_rounding(const std::uint32_t stride) {
    for (const LogPair& pair : log_pairs) {
        std::uint64_t checked = 0;
        std::uint64_t differ = 0;
        for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += stride) {
            const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float result = (*pair.region_function)(x);
            const float current = (*pair.current_function)(x);
            checked += 1;
            if (same_bits(result, current)) continue;
            differ += 1;
            const bool positive_normal = x >= 0x1p-126f && x <= std::numeric_limits<float>::max();
            if (positive_normal) {
                const float expected = log_float_expected(pair, x);
                std::fprintf(stderr, "%s(%a): region %a, current %a, correctly rounded %a\n", pair.name,
                             static_cast<double>(x), static_cast<double>(result), static_cast<double>(current),
                             static_cast<double>(expected));
            } else {
                std::fprintf(stderr, "%s(%a): region %a, current %a (special)\n", pair.name,
                             static_cast<double>(x), static_cast<double>(result), static_cast<double>(current));
            }
        }
        std::printf("%s(float): %llu patterns (stride %u), %llu differ from the current function\n", pair.name,
                    static_cast<unsigned long long>(checked), stride, static_cast<unsigned long long>(differ));
        require(differ == 0, "region log(float): differs from the current, correctly rounded function");
    }
}

void log_float_exact_cases() {
    for (int k = -126; k <= 127; ++k) {
        const float x = std::ldexp(1.0f, k);
        require(region::log2(x) == static_cast<float>(k), "region log2(float): log2(2^k) is not k");
    }
    float power = 1.0f;
    for (int k = 0; k <= 10; ++k) {
        require(region::log10(power) == static_cast<float>(k), "region log10(float): log10(10^k) is not k");
        power *= 10.0f;
    }
    require(same_bits(region::log(1.0f), 0.0f) && same_bits(region::log2(1.0f), 0.0f) &&
                same_bits(region::log10(1.0f), 0.0f),
            "region log(float): log(1) is not +0");
    std::printf("log(float) exact cases hold\n");
}

// ---- hypot(float) ---------------------------------------------------------------

// sqrt(a^2 + b^2) correctly rounded to float (ties to even, the infinity
// pattern above FLT_MAX), for normal or zero a, b (not both zero), from the
// reference; exact ties found exactly: a^2 + b^2 = m^2 for a midpoint m (a^2,
// b^2 and m^2 are exact doubles, and their sum is exact when its two-sum
// error is zero).
float hypot_float_expected(const float x, const float y) {
    const double a = std::fabs(static_cast<double>(x));
    const double b = std::fabs(static_cast<double>(y));
    const wide::ScaledWide exact = wide::hypot_reference(a, b);
    const double hi = std::ldexp(exact.mantissa.hi, exact.exponent);
    const double lo = std::ldexp(exact.mantissa.lo, exact.exponent);
    const double overflow_point = 0x1.ffffffp127;  // FLT_MAX + half an ulp: ties to even give infinity
    if (hi > overflow_point || (hi == overflow_point && lo >= 0.0)) return std::numeric_limits<float>::infinity();
    const float rounded = correctly_rounded_float(wide::Wide{hi, lo});
    const double larger = std::max(a * a, b * b);
    const double smaller = std::min(a * a, b * b);
    const double s = larger + smaller;
    const double s_error = smaller - (s - larger);
    const float neighbours[2] = {std::nextafter(rounded, 0.0f),
                                 std::nextafter(rounded, std::numeric_limits<float>::infinity())};
    for (const float neighbour : neighbours) {
        if (std::isinf(neighbour)) continue;
        const double midpoint = (static_cast<double>(rounded) + static_cast<double>(neighbour)) * 0.5;
        if (s == midpoint * midpoint && s_error == 0.0) {
            const bool rounded_even = (std::bit_cast<std::uint32_t>(rounded) & 1U) == 0;
            return rounded_even ? rounded : neighbour;
        }
    }
    return rounded;
}

void hypot_float_check(const float x, const float y, int& checked) {
    const float result = region::hypot(x, y);
    const float expected = hypot_float_expected(x, y);
    require(same_bits(result, expected), "region hypot(float): result is not correctly rounded");
    checked += 1;
}

// Pythagorean triples with a 25-bit odd hypotenuse (an exact midpoint),
// scaled across float's range, and random pairs.
void hypot_float_rounding(const int count) {
    int checked = 0;
    int ties = 0;
    for (std::int64_t u = 2897; u <= 5792 && ties < 4000; ++u) {
        for (std::int64_t v = 1; v <= 64; ++v) {
            if (((u + v) & 1) == 0) continue;
            const std::int64_t a = u * u - v * v;
            const std::int64_t b = 2 * u * v;
            const std::int64_t c = u * u + v * v;
            if (c < (std::int64_t{1} << 24) || c >= (std::int64_t{1} << 25) || a >= (std::int64_t{1} << 24)) continue;
            const float even = ((c - 1) / 2) % 2 == 0 ? static_cast<float>(c - 1) : static_cast<float>(c + 1);
            const int scales[] = {-100, -40, 0, 40, 100};
            for (const int scale : scales) {
                const float x = std::ldexp(static_cast<float>(a), scale);
                const float y = std::ldexp(static_cast<float>(b), scale);
                require(same_bits(region::hypot(x, y), std::ldexp(even, scale)),
                        "region hypot(float): an exact midpoint does not round to even");
                require(same_bits(region::hypot(-y, x), std::ldexp(even, scale)),
                        "region hypot(float): an exact midpoint (swapped, negated)");
            }
            ties += 1;
        }
    }
    Lcg lcg{2041};
    for (int i = 0; i < count; ++i) {
        // Random patterns of normal floats, and pairs of close magnitude.
        const float x = std::bit_cast<float>(0x00800000U + static_cast<std::uint32_t>(lcg.bits() % 0x7f000000U));
        const float y = std::bit_cast<float>(0x00800000U + static_cast<std::uint32_t>(lcg.bits() % 0x7f000000U));
        hypot_float_check(x, y, checked);
        const float close = std::ldexp(static_cast<float>(1.0 + lcg.unit()), static_cast<int>(lcg.bits() % 250) - 125);
        const float other = close * static_cast<float>(lcg.unit() * 2.0);
        if (other >= 0x1p-126f) hypot_float_check(close, other, checked);
    }
    // Overflow and the largest floats.
    const float largest = std::numeric_limits<float>::max();
    hypot_float_check(largest, largest, checked);
    hypot_float_check(largest, 0x1p103f, checked);
    hypot_float_check(largest, 0x1p104f, checked);
    hypot_float_check(0x1p127f, 0x1p127f, checked);
    std::printf("hypot(float) correctly rounded at %d pairs; %d exact midpoint triples x 10 round to even\n",
                checked, ties);
}

void hypot_float_special_values() {
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float values[] = {0.0f, -0.0f, 0x1p-149f, -0x1p-140f, 0x1p-126f, 1.0f, -3.0f, 4.0f,
                            0x1.fffffep127f, infinity, -infinity, nan};
    int checked = 0;
    for (const float x : values) {
        for (const float y : values) {
            const float result = region::hypot(x, y);
            const float current = current_hypot(x, y);
            require(same_bits(result, current), "region hypot(float): a special case differs from the current");
            checked += 1;
        }
    }
    std::printf("hypot(float) special cases agree (%d pairs)\n", checked);
}

// ---- sincos and tan (double), trig_region.cpp ----------------------------------

// The library's stated bound for sin, cos and tan (0.51 ulp), which the
// region kernels keep (tools/math/trig_region_certificate.py proves it).
constexpr double trig_ulp_limit = sincos_worst_cases::stated_bound_ulps;

double trig_ulps(const double approx, const wide::Wide exact) {
    const double rounded = std::fabs(exact.hi + exact.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{approx, 0.0}, wide::negate(exact));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

struct TrigRegion final {
    const char* name{nullptr};
    double low{0.0};
    double high{0.0};
};

constexpr TrigRegion trig_regions[] = {
    {"[-2 pi, 2 pi]", -6.283185307179586, 6.283185307179586},
    {"[-64, 64]", -64.0, 64.0},
    {"2^-27 .. 2^-6", 0x1p-27, 0x1p-6},
    {"[64, 1e6]", 64.0, 1e6},
    {"[1e6, 2^32]", 1e6, 0x1p32},
};

// Sampled accuracy against the references, and how often the bits differ
// from the library's (both are within the bound; the region kernel rounds
// differently in a few last bits).
void trig_accuracy(const int per_region) {
    for (const TrigRegion& region_entry : trig_regions) {
        Lcg lcg{2027};
        double worst_sin = 0.0;
        double worst_cos = 0.0;
        double worst_tan = 0.0;
        int differ = 0;
        for (int i = 0; i < per_region; ++i) {
            const double x = region_entry.low + (region_entry.high - region_entry.low) * lcg.unit();
            double s = 0.0;
            double c = 0.0;
            region::sincos(x, s, c);
            const double t = region::tan(x);
            const wide::WideSinCos exact = wide::reference(x);
            worst_sin = std::max(worst_sin, trig_ulps(s, exact.s));
            worst_cos = std::max(worst_cos, trig_ulps(c, exact.c));
            worst_tan = std::max(worst_tan, trig_ulps(t, wide::tan_reference(x)));
            double current_s = 0.0;
            double current_c = 0.0;
            current_sincos(x, current_s, current_c);
            const double current_t = current_tan(x);
            if (!same_bits(s, current_s) || !same_bits(c, current_c) || !same_bits(t, current_t)) differ += 1;
        }
        std::printf("sincos/tan(double) %-14s worst sin %.6f cos %.6f tan %.6f ulp; %d of %d differ from current\n",
                    region_entry.name, worst_sin, worst_cos, worst_tan, differ, per_region);
        require(worst_sin <= trig_ulp_limit && worst_cos <= trig_ulp_limit && worst_tan <= trig_ulp_limit,
                "region sincos/tan(double): error above the stated bound");
    }
}

// The certified worst cases (tests/reference/sincos_worst_cases.hpp): the
// double nearest a multiple of pi/128 in every binade and residue class,
// and its negation, against the exact values the certificate computed.
void trig_worst_cases() {
    int checked = 0;
    for (const sincos_worst_cases::Case& entry : sincos_worst_cases::cases) {
        for (int sign = 0; sign < 2; ++sign) {
            const double flip = sign == 0 ? 1.0 : -1.0;
            const double x = flip * entry.x;
            double s = 0.0;
            double c = 0.0;
            region::sincos(x, s, c);
            const double t = region::tan(x);
            const double sin_error = trig_ulps(s, wide::Wide{flip * entry.sin_hi, flip * entry.sin_lo});
            const double cos_error = trig_ulps(c, wide::Wide{entry.cos_hi, entry.cos_lo});
            const double tan_error = trig_ulps(t, wide::Wide{flip * entry.tan_hi, flip * entry.tan_lo});
            if (sin_error > trig_ulp_limit || cos_error > trig_ulp_limit || tan_error > trig_ulp_limit) {
                std::fprintf(stderr, "x = %a: sin %.6f cos %.6f tan %.6f ulp\n", x, sin_error, cos_error, tan_error);
            }
            require(sin_error <= trig_ulp_limit && cos_error <= trig_ulp_limit && tan_error <= trig_ulp_limit,
                    "region sincos/tan(double): a certified worst case above the bound");
            checked += 1;
        }
    }
    std::printf("sincos/tan(double) certified worst cases within the bound (%d)\n", checked);
}

// Zeros, subnormals, the tiny path, the ends of the supported range and the
// refusals: the same bits as the library's public functions.
void trig_special_values() {
    const double values[] = {0.0, -0.0, 0x1p-1074, -0x1p-1074, 0x1p-1022, -0x1.8p-1023, 0x1p-28, 0x1.fffffffffffffp-28,
                             0x1p-27, -0x1p-27, 0x1.fffffffffffffp5, 0x1p6, -0x1p6, 0x1p20, 0x1.0000000000001p20,
                             0x1p32, -0x1p32, 0x1.0000000000001p32, 1e300,
                             std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()};
    int checked = 0;
    for (const double x : values) {
        double s = 0.0;
        double c = 0.0;
        region::sincos(x, s, c);
        double current_s = 0.0;
        double current_c = 0.0;
        current_sincos(x, current_s, current_c);
        const bool nan_agrees = (s != s) == (current_s != current_s) && (c != c) == (current_c != current_c);
        const bool bits_agree = (s != s || same_bits(s, current_s)) && (c != c || same_bits(c, current_c));
        require(nan_agrees && bits_agree, "region sincos(double): a special case differs from the current");
        const double t = region::tan(x);
        const double current_t = current_tan(x);
        require((t != t) == (current_t != current_t) && (t != t || same_bits(t, current_t)),
                "region tan(double): a special case differs from the current");
        checked += 1;
    }
    std::printf("sincos/tan(double) special cases agree (%d)\n", checked);
}

// ---- determinism ----------------------------------------------------------------

STXLIB_MODULAR void hash_word(std::uint64_t& hash, const std::uint64_t word) {
    hash ^= word;
    hash *= 1099511628211ULL;
}

// FNV-1a over fixed inputs: every variant and both product forms must give
// the same value (the fused build and the Dekker build print it; the
// integrator freezes it from the first run of both).
std::uint64_t region_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{96};
    for (int region_index = 0; region_index < pow_region_count; ++region_index) {
        for (int i = 0; i < 200000; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region_index, lcg, x, y);
            hash_word(hash, std::bit_cast<std::uint64_t>(region::pow(x, y)));
            hash_word(hash, std::bit_cast<std::uint64_t>(region::pow(-x, std::floor(y))));
            const float xf = static_cast<float>(x);
            const float yf = static_cast<float>(y);
            hash_word(hash, std::bit_cast<std::uint32_t>(region::pow(xf, yf)));
            hash_word(hash, std::bit_cast<std::uint32_t>(region::hypot(xf, yf)));
        }
    }
    for (const TrigRegion& region_entry : trig_regions) {
        for (int i = 0; i < 200000; ++i) {
            const double x = region_entry.low + (region_entry.high - region_entry.low) * lcg.unit();
            double s = 0.0;
            double c = 0.0;
            region::sincos(x, s, c);
            hash_word(hash, std::bit_cast<std::uint64_t>(s));
            hash_word(hash, std::bit_cast<std::uint64_t>(c));
            hash_word(hash, std::bit_cast<std::uint64_t>(region::tan(x)));
        }
    }
    for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += 4099) {
        const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
        hash_word(hash, std::bit_cast<std::uint32_t>(region::log(x)));
        hash_word(hash, std::bit_cast<std::uint32_t>(region::log2(x)));
        hash_word(hash, std::bit_cast<std::uint32_t>(region::log10(x)));
    }
    return hash;
}

// Not yet frozen (0): the first M4 run of both builds sets it.
constexpr std::uint64_t expected_region_output_hash = 0;

void region_determinism() {
    const std::uint64_t hash = region_output_hash();
    std::printf("region output hash %016llx\n", static_cast<unsigned long long>(hash));
    if (expected_region_output_hash == 0) {
        std::printf("region output hash not frozen yet\n");
        return;
    }
    require(hash == expected_region_output_hash, "region: outputs differ from the frozen bits");
}

// ---- --bench --------------------------------------------------------------------

struct BenchBuffers final {
    std::vector<double> x{};
    std::vector<double> y{};
    std::vector<double> out{};
    std::vector<float> xf{};
    std::vector<float> yf{};
    std::vector<float> outf{};
    std::vector<double> out2{};
};

using BenchKernel = void (*)(BenchBuffers& buffers);

template <double (*Function)(double, double)>
void binary_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.out[i] = Function(b.x[i], b.y[i]);
}

template <float (*Function)(float, float)>
void binary_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.outf[i] = Function(b.xf[i], b.yf[i]);
}

template <float (*Function)(float)>
void unary_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.outf[i] = Function(b.xf[i]);
}

template <double (*Function)(double)>
void unary_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.out[i] = Function(b.x[i]);
}

template <void (*Function)(double, double&, double&)>
void pair_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) Function(b.x[i], b.out[i], b.out2[i]);
}

void region_sincos_d(const double x, double& s, double& c) { region::sincos(x, s, c); }
void current_sincos_d(const double x, double& s, double& c) { current_sincos(x, s, c); }
double region_tan_d(const double x) { return region::tan(x); }
double current_tan_d(const double x) { return current_tan(x); }
double region_pow_d(const double x, const double y) { return region::pow(x, y); }
double current_pow_d(const double x, const double y) { return current_pow(x, y); }
float region_pow_f(const float x, const float y) { return region::pow(x, y); }
float current_pow_f(const float x, const float y) { return current_pow(x, y); }
float region_hypot_f(const float x, const float y) { return region::hypot(x, y); }
float current_hypot_f(const float x, const float y) { return current_hypot(x, y); }

double best_ns_per_call(const BenchKernel kernel, BenchBuffers& buffers) {
    double best = std::numeric_limits<double>::infinity();
    for (int trial = 0; trial < 15; ++trial) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        kernel(buffers);
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(end - start).count();
        best = std::min(best, ns / static_cast<double>(buffers.x.size()));
    }
    return best;
}

struct BenchRow final {
    const char* name{nullptr};
    double x_low{0.0};
    double x_high{0.0};
    double y_low{0.0};
    double y_high{0.0};
    BenchKernel region_kernel{nullptr};
    BenchKernel current_kernel{nullptr};
};

void bench() {
    const BenchRow rows[] = {
        {"pow(double) x [0.5, 2]", 0.5, 2.0, -8.0, 8.0, binary_double<region_pow_d>, binary_double<current_pow_d>},
        {"pow(double) x [0, 100]", 0.0, 100.0, -8.0, 8.0, binary_double<region_pow_d>, binary_double<current_pow_d>},
        {"pow(double) x 1+-2^-7", 1.0 - 0x1p-7, 1.0 + 0x1p-7, -500.0, 500.0, binary_double<region_pow_d>,
         binary_double<current_pow_d>},
        {"pow(float) x [0.5, 2]", 0.5, 2.0, -8.0, 8.0, binary_float<region_pow_f>, binary_float<current_pow_f>},
        {"pow(float) x [0, 100]", 0.0, 100.0, -8.0, 8.0, binary_float<region_pow_f>, binary_float<current_pow_f>},
        {"log(float) [1e-30, 1e30]", 1e-30, 1e30, 0.0, 0.0, unary_float<region_log_f>, unary_float<current_log_f>},
        {"log(float) [0.5, 2]", 0.5, 2.0, 0.0, 0.0, unary_float<region_log_f>, unary_float<current_log_f>},
        {"log(float) 1 +- 1e-3", 0.999, 1.001, 0.0, 0.0, unary_float<region_log_f>, unary_float<current_log_f>},
        {"log2(float) [0.5, 2]", 0.5, 2.0, 0.0, 0.0, unary_float<region_log2_f>, unary_float<current_log2_f>},
        {"log10(float) [0.5, 2]", 0.5, 2.0, 0.0, 0.0, unary_float<region_log10_f>, unary_float<current_log10_f>},
        {"hypot(float) [-8, 8]", -8.0, 8.0, -8.0, 8.0, binary_float<region_hypot_f>, binary_float<current_hypot_f>},
        {"hypot(float) [1e-20, 1e20]", 1e-20, 1e20, 1e-20, 1e20, binary_float<region_hypot_f>,
         binary_float<current_hypot_f>},
        {"sincos(double) [-pi, pi]", -3.141592653589793, 3.141592653589793, 0.0, 0.0,
         pair_double<region_sincos_d>, pair_double<current_sincos_d>},
        {"sincos(double) [-64, 64]", -64.0, 64.0, 0.0, 0.0, pair_double<region_sincos_d>,
         pair_double<current_sincos_d>},
        {"sincos(double) [-1e6, 1e6]", -1e6, 1e6, 0.0, 0.0, pair_double<region_sincos_d>,
         pair_double<current_sincos_d>},
        {"tan(double) [-pi, pi]", -3.141592653589793, 3.141592653589793, 0.0, 0.0, unary_double<region_tan_d>,
         unary_double<current_tan_d>},
        {"tan(double) [-64, 64]", -64.0, 64.0, 0.0, 0.0, unary_double<region_tan_d>, unary_double<current_tan_d>},
    };
    const std::size_t count = std::size_t{1} << 20;
    std::printf("region kernels against the current ones, ns per call, %zu inputs, best of 15\n", count);
    std::printf("%-28s %10s %10s %7s\n", "function and region", "current", "region", "ratio");
    for (const BenchRow& row : rows) {
        BenchBuffers b{};
        Lcg lcg{12345};
        b.x.resize(count);
        b.y.resize(count);
        b.out.resize(count);
        b.xf.resize(count);
        b.yf.resize(count);
        b.outf.resize(count);
        b.out2.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            b.x[i] = row.x_low + (row.x_high - row.x_low) * lcg.unit();
            b.y[i] = row.y_low + (row.y_high - row.y_low) * lcg.unit();
            b.xf[i] = static_cast<float>(b.x[i]);
            b.yf[i] = static_cast<float>(b.y[i]);
        }
        const double current = best_ns_per_call(row.current_kernel, b);
        const double region_ns = best_ns_per_call(row.region_kernel, b);
        std::printf("%-28s %10.3f %10.3f %7.2f\n", row.name, current, region_ns, region_ns / current);
    }
}

} // namespace

int main(const int argument_count, char** const arguments) {
    const bool exhaustive = argument_count == 2 && std::strcmp(arguments[1], "--exhaustive") == 0;
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        bench();
        return 0;
    }
    try {
        pow_double_special_values();
        pow_double_accuracy(exhaustive ? 20000000 : 100000);
        pow_float_special_values();
        pow_float_midpoints();
        pow_float_rounding(exhaustive ? 100000000 : 200000);
        pow_float_accurate_stage(exhaustive ? 10000000 : 100000);
        log_float_exact_cases();
        log_float_rounding(exhaustive ? 1 : 997);
        hypot_float_special_values();
        hypot_float_rounding(exhaustive ? 100000000 : 300000);
        trig_special_values();
        trig_worst_cases();
        trig_accuracy(exhaustive ? 20000000 : 200000);
        region_determinism();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "math region tests failed: %s\n", failure.what());
        return 1;
    }
    std::puts("math region tests passed");
    return 0;
}
