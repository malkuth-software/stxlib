// Tests of exp2, expm1, log1p, sin and cos (stxlib/math.hpp; roadmap R1),
// and of the half-turn functions and cosine losses (sincospi, sinpi, cospi,
// sincpi, cosm1, cosm1pi; 2026-11-22), against the
// double-double references of tests/reference/wide.hpp.
//
//   math_completion_tests               sampled checks (the ctest run)
//   math_completion_tests --exhaustive  every float through the float
//                                       screen (the proof of correct
//                                       rounding, with the certificate) and
//                                       20 million doubles per region
//   math_completion_tests --bench       ns per call against the C++ library
//
// The float proof: tools/math/r1_certificate.py bounds each core's
// double-double (hi + lo, before the one rounding) within a relative error
// E of the exact value. For every float on the fast path, the screen below
// forms the same double-double with the core functions (compiled here as
// the native variant, whose bits every variant matches) and checks that hi
// + lo lies farther than E |hi| from both ends of its float's rounding
// interval: the exact value then rounds to the same float. Any float the
// screen cannot settle is decided against the reference and listed. The
// screen also checks that the public function returns that float.

#include "stxlib/math.hpp"

#include "reference/wide.hpp"
#include "core/checked.hpp"

#if !defined(STXLIB_MATH_VARIANT)
#define STXLIB_MATH_VARIANT native
#endif
#include "algorithms/math/completions_core.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "frozen_hash.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace core = stxlib::math::STXLIB_MATH_VARIANT::detail;

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

struct Lcg final {
    std::uint64_t state{0};

    STXLIB_MODULAR double unit() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double result = static_cast<double>(state >> 11) * 0x1.0p-53;
        return result;
    }
};

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

double flushed(const double value) {
    if (std::fpclassify(value) == FP_SUBNORMAL) return std::copysign(0.0, value);
    return value;
}

// The proved bounds (tools/math/r1_certificate.py), double in ulp with room
// for the reference, and each core's relative error for the float screen.
constexpr double exp2_ulp_limit = 0.5001;   // proved 0.5000001
constexpr double expm1_ulp_limit = 0.5013;  // proved 0.5011219
constexpr double log1p_ulp_limit = 0.5001;  // proved 0.5000001
constexpr double exp2_screen = 0x1p-79;     // proved 2^-79.92
constexpr double expm1_screen = 0x1.6a09e667f3bcdp-63; // 2^-62.5 rounded up; proved 2^-62.80
constexpr double log1p_screen = 0x1p-75;    // proved 2^-75.80

// ---- references ------------------------------------------------------------------

double wide_ulps(const double approx, const wide::Wide exact) {
    const double rounded = std::fabs(exact.hi + exact.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{approx, 0.0}, wide::negate(exact));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// 2^x = e^(x ln 2), as mantissa 2^exponent (|x| <= 1075).
wide::ScaledWide exp2_reference(const double x) {
    const wide::Wide ln2{wide::ln2_1, wide::ln2_2};
    const wide::ScaledWide result = wide::exp_reference(wide::multiply(ln2, x));
    return result;
}

double scaled_ulps(const double approx, const wide::ScaledWide exact) {
    const double scaled = std::ldexp(approx, -exact.exponent);
    const double rounded = std::fabs(exact.mantissa.hi + exact.mantissa.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{scaled, 0.0}, wide::negate(exact.mantissa));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// sum of x^k / k! for k = 1 to 40 in double-double (|x| < 2^-4).
wide::Wide expm1_series_reference(const double x) {
    wide::Wide term{x, 0.0};
    wide::Wide sum = term;
    for (int k = 2; k <= 40; ++k) {
        term = wide::divide(wide::multiply(term, x), static_cast<double>(k));
        sum = wide::add(sum, term);
    }
    return sum;
}

// e^x - 1 as mantissa 2^exponent: the series near 0, else e^x minus 1 in
// the reference's scaled form (exact enough for |x| <= 745).
wide::ScaledWide expm1_reference(const double x) {
    if (std::fabs(x) < 0x1p-4) {
        const wide::Wide value = expm1_series_reference(x);
        int exponent = 0;
        (void)std::frexp(value.hi, &exponent);
        const wide::ScaledWide result{wide::Wide{std::ldexp(value.hi, -exponent), std::ldexp(value.lo, -exponent)},
                                      exponent};
        return result;
    }
    const wide::ScaledWide e = wide::exp_reference(x);
    const wide::Wide one_scaled{-std::ldexp(1.0, -e.exponent), 0.0};
    const wide::ScaledWide result{wide::add(e.mantissa, one_scaled), e.exponent};
    return result;
}

// log(1 + x): the series near 0, else log u + u_lo / u with u + u_lo = 1 + x.
wide::Wide log1p_reference(const double x) {
    if (std::fabs(x) < 0x1p-20) {
        wide::Wide power{x, 0.0};
        wide::Wide sum = power;
        for (int k = 2; k <= 12; ++k) {
            power = wide::multiply(power, x);
            const wide::Wide term = wide::divide(power, static_cast<double>(k));
            sum = (k % 2 == 0) ? wide::add(sum, wide::negate(term)) : wide::add(sum, term);
        }
        return sum;
    }
    const wide::Wide pair = wide::two_sum(1.0, x);
    const wide::Wide log_u = wide::log_reference(pair.hi);
    const wide::Wide ratio = wide::divide(wide::Wide{pair.lo, 0.0}, pair.hi);
    const wide::Wide correction = wide::add(ratio, wide::negate(wide::multiply(wide::multiply(ratio, ratio), 0.5)));
    const wide::Wide result = wide::add(log_u, correction);
    return result;
}

// hi + lo rounded once to float (as tests/math_tests.cpp's), with infinity
// above FLT_MAX + half an ulp.
float correctly_rounded_float(const wide::Wide exact) {
    const double overflow_point = 0x1.ffffffp127;
    if (std::fabs(exact.hi) > overflow_point ||
        (std::fabs(exact.hi) == overflow_point && (exact.lo >= 0.0) == (exact.hi > 0.0))) {
        return std::copysign(std::numeric_limits<float>::infinity(), static_cast<float>(exact.hi));
    }
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

wide::Wide unscaled(const wide::ScaledWide value) {
    const wide::Wide result{std::ldexp(value.mantissa.hi, value.exponent),
                            std::ldexp(value.mantissa.lo, value.exponent)};
    return result;
}

// ---- the float screen ------------------------------------------------------------

// The distance from hi + lo to the nearer end of the rounding interval of
// f (its float), or to the overflow point when f is infinite.
double boundary_distance(const double hi, const double lo, const float f) {
    if (std::isinf(f)) {
        const double result = std::fabs((std::fabs(hi) - 0x1.ffffffp127) + std::copysign(lo, hi));
        return result;
    }
    const float up = std::nextafter(f, std::numeric_limits<float>::infinity());
    const float down = std::nextafter(f, -std::numeric_limits<float>::infinity());
    const double up_end = std::isinf(up) ? 0x1.ffffffp127 : (static_cast<double>(f) + static_cast<double>(up)) * 0.5;
    const double down_end =
        std::isinf(down) ? -0x1.ffffffp127 : (static_cast<double>(f) + static_cast<double>(down)) * 0.5;
    const double result = std::min(std::fabs((hi - up_end) + lo), std::fabs((hi - down_end) + lo));
    return result;
}

// The core's double-double for a float x on the fast path; false off it.
using FloatCore = bool (*)(double x, double& hi, double& lo);
using WideReference = wide::Wide (*)(double x);
using PublicFloat = float (*)(float x);

bool exp2_core(const double x, double& hi, double& lo) {
    if (!(x > -150.0 && x < 128.0)) return false;
    double head = 0.0;
    double tail = 0.0;
    std::int64_t exponent = 0;
    core::region_exp2(x, 0.0, head, tail, exponent);
    const double scale = core::power_of_two(exponent);
    hi = head * scale;
    lo = tail * scale;
    return true;
}

STXLIB_MODULAR bool expm1_core(const double x, double& hi, double& lo) {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & 0x7fffffffffffffffULL;
    if (magnitude - core::expm1_tiny_bits < core::expm1_series_bits - core::expm1_tiny_bits) {
        core::expm1_series(x, hi, lo);
        return true;
    }
    if (magnitude >= core::expm1_series_bits && x <= 89.0 && x >= core::expm1_minus_one) {
        core::expm1_exp(x, hi, lo);
        return true;
    }
    return false;
}

bool log1p_core(const double x, double& hi, double& lo) {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(x) & 0x7fffffffffffffffULL;
    if (!(magnitude >= core::log1p_tiny_bits && x > -1.0 && x < std::numeric_limits<double>::infinity())) return false;
    core::log1p_wide(x, hi, lo);
    return true;
}

wide::Wide exp2_wide_reference(const double x) { return unscaled(exp2_reference(x)); }
wide::Wide expm1_wide_reference(const double x) { return unscaled(expm1_reference(x)); }

float public_exp2_f(const float x) { return stxlib::math::exp2(x); }
float public_expm1_f(const float x) { return stxlib::math::expm1(x); }
float public_log1p_f(const float x) { return stxlib::math::log1p(x); }

struct ScreenedFunction final {
    const char* name{nullptr};
    FloatCore core_function{nullptr};
    WideReference reference{nullptr};
    PublicFloat public_function{nullptr};
    double screen{0.0};
};

const ScreenedFunction screened[] = {
    {"exp2", exp2_core, exp2_wide_reference, public_exp2_f, exp2_screen},
    {"expm1", expm1_core, expm1_wide_reference, public_expm1_f, expm1_screen},
    {"log1p", log1p_core, log1p_reference, public_log1p_f, log1p_screen},
};

void float_screen(const std::uint64_t stride) {
    for (const ScreenedFunction& function : screened) {
        std::uint64_t screened_count = 0;
        std::uint64_t unsettled = 0;
        for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += stride) {
            const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            if (std::fpclassify(x) == FP_SUBNORMAL) continue;  // read as zero: special values below
            const double xd = static_cast<double>(x);
            double hi = 0.0;
            double lo = 0.0;
            if (!(*function.core_function)(xd, hi, lo)) continue;
            const float rounded = core::round_wide_to_float(hi, lo);
            require(same_bits((*function.public_function)(x), flushed(rounded)),
                    "float screen: the public function is not the core's rounding");
            screened_count += 1;
            const double distance = boundary_distance(hi, lo, rounded);
            if (distance > std::fabs(hi) * function.screen * (1.0 + 0x1p-40)) continue;
            unsettled += 1;
            const float expected = correctly_rounded_float((*function.reference)(xd));
            std::printf("  %s(%a): unsettled by the screen; core %a, reference %a\n", function.name,
                        static_cast<double>(x), static_cast<double>(rounded), static_cast<double>(expected));
            require(same_bits(rounded, expected), "float screen: an unsettled float is not correctly rounded");
        }
        std::printf("%s(float): %llu floats screened (stride %llu), %llu unsettled\n", function.name,
                    static_cast<unsigned long long>(screened_count), static_cast<unsigned long long>(stride),
                    static_cast<unsigned long long>(unsettled));
    }
}

// ---- double accuracy -------------------------------------------------------------

double geometric(Lcg& lcg, const double low_exponent, const double high_exponent) {
    const double exponent = low_exponent + (high_exponent - low_exponent) * lcg.unit();
    const double result = std::exp2(exponent);  // the input's spread only; any host value serves
    return result;
}

void exp2_double_accuracy(const int per_region) {
    const char* const names[] = {"[-1, 1]", "[-1022, 1022]", "|x| in [2^-54, 2^-9]", "[1022, 1024)"};
    Lcg lcg{3001};
    for (int region = 0; region < 4; ++region) {
        double worst = 0.0;
        double worst_x = 0.0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            if (region == 0) x = 2.0 * lcg.unit() - 1.0;
            if (region == 1) x = 2044.0 * lcg.unit() - 1022.0;
            if (region == 2) x = (lcg.unit() < 0.5 ? -1.0 : 1.0) * geometric(lcg, -54.0, -9.0);
            if (region == 3) x = 1022.0 + 2.0 * lcg.unit();
            const double result = stxlib::math::exp2(x);
            if (std::isinf(result)) {
                require(x >= 1023.99, "exp2(double): overflow below the largest double");
                continue;
            }
            const double error = scaled_ulps(result, exp2_reference(x));
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("exp2(double) %-22s %10d inputs  max %.7f ulp at %a\n", names[region], per_region, worst, worst_x);
        require(worst <= exp2_ulp_limit, "exp2(double): error above the proved bound");
    }
}

void expm1_double_accuracy(const int per_region) {
    const char* const names[] = {"|x| in [2^-54, 2^-5]", "|x| in [2^-5, 1]", "[-38, 45]", "(45, 709.7]",
                                 "2^-5 +- 2^-30"};
    Lcg lcg{3002};
    for (int region = 0; region < 5; ++region) {
        double worst = 0.0;
        double worst_x = 0.0;
        for (int i = 0; i < per_region; ++i) {
            const double sign = lcg.unit() < 0.5 ? -1.0 : 1.0;
            double x = 0.0;
            if (region == 0) x = sign * geometric(lcg, -54.0, -5.0);
            if (region == 1) x = sign * geometric(lcg, -5.0, 0.0);
            if (region == 2) x = 83.0 * lcg.unit() - 38.0;
            if (region == 3) x = 45.0 + 664.7 * lcg.unit();
            if (region == 4) x = sign * (0x1p-5 + (lcg.unit() - 0.5) * 0x1p-29);
            const double result = stxlib::math::expm1(x);
            const double error = scaled_ulps(result, expm1_reference(x));
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("expm1(double) %-22s %10d inputs  max %.7f ulp at %a\n", names[region], per_region, worst,
                    worst_x);
        require(worst <= expm1_ulp_limit, "expm1(double): error above the proved bound");
    }
}

void log1p_double_accuracy(const int per_region) {
    const char* const names[] = {"|x| in [2^-54, 2^-7]", "[-0.31, 0.37]", "-1 + [2^-53, 0.5]", "[0.37, 1e300]"};
    Lcg lcg{3003};
    for (int region = 0; region < 4; ++region) {
        double worst = 0.0;
        double worst_x = 0.0;
        for (int i = 0; i < per_region; ++i) {
            const double sign = lcg.unit() < 0.5 ? -1.0 : 1.0;
            double x = 0.0;
            if (region == 0) x = sign * geometric(lcg, -54.0, -7.0);
            if (region == 1) x = 0.68 * lcg.unit() - 0.31;
            if (region == 2) x = -1.0 + geometric(lcg, -53.0, -1.0);
            if (region == 3) x = geometric(lcg, -1.43, 996.5);
            const double result = stxlib::math::log1p(x);
            const double error = wide_ulps(result, log1p_reference(x));
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("log1p(double) %-22s %10d inputs  max %.7f ulp at %a\n", names[region], per_region, worst,
                    worst_x);
        require(worst <= log1p_ulp_limit, "log1p(double): error above the proved bound");
    }
}

// ---- special values --------------------------------------------------------------

void special_values() {
    namespace math = stxlib::math;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const float inf_f = std::numeric_limits<float>::infinity();
    const float nan_f = std::numeric_limits<float>::quiet_NaN();
    // exp2: exact powers, the ends, the subnormal rule.
    for (int n = -1022; n <= 1023; ++n) {
        require(math::exp2(static_cast<double>(n)) == std::ldexp(1.0, n), "exp2(double): 2^n is not exact");
    }
    for (int n = -126; n <= 127; ++n) {
        require(math::exp2(static_cast<float>(n)) == std::ldexp(1.0f, n), "exp2(float): 2^n is not exact");
    }
    require(same_bits(math::exp2(0.0), 1.0) && same_bits(math::exp2(-0.0), 1.0) && same_bits(math::exp2(inf), inf) &&
                same_bits(math::exp2(-inf), 0.0) && std::isnan(math::exp2(nan)) && same_bits(math::exp2(1024.0), inf) &&
                same_bits(math::exp2(-1022.5), 0.0) && same_bits(math::exp2(-1100.0), 0.0) &&
                same_bits(math::exp2(0x1p-1074), 1.0) && same_bits(math::exp2(0x1p-60), 1.0),
            "exp2(double): a special value");
    require(same_bits(math::exp2(0.0f), 1.0f) && same_bits(math::exp2(inf_f), inf_f) &&
                same_bits(math::exp2(-inf_f), 0.0f) && std::isnan(math::exp2(nan_f)) &&
                same_bits(math::exp2(128.0f), inf_f) && same_bits(math::exp2(-127.0f), 0.0f) &&
                same_bits(math::exp2(-150.0f), 0.0f) && same_bits(math::exp2(0x1p-149f), 1.0f),
            "exp2(float): a special value");
    // expm1.
    require(same_bits(math::expm1(0.0), 0.0) && same_bits(math::expm1(-0.0), -0.0) &&
                same_bits(math::expm1(0x1p-1074), 0.0) && same_bits(math::expm1(-0x1p-1074), -0.0) &&
                same_bits(math::expm1(-inf), -1.0) && same_bits(math::expm1(inf), inf) && std::isnan(math::expm1(nan)) &&
                same_bits(math::expm1(-40.0), -1.0) && same_bits(math::expm1(710.0), inf) &&
                same_bits(math::expm1(1e-300), 1e-300) && same_bits(math::expm1(-1e-300), -1e-300),
            "expm1(double): a special value");
    require(same_bits(math::expm1(0.0f), 0.0f) && same_bits(math::expm1(-0.0f), -0.0f) &&
                same_bits(math::expm1(0x1p-149f), 0.0f) && same_bits(math::expm1(-inf_f), -1.0f) &&
                same_bits(math::expm1(inf_f), inf_f) && std::isnan(math::expm1(nan_f)) &&
                same_bits(math::expm1(-40.0f), -1.0f) && same_bits(math::expm1(89.0f), inf_f) &&
                same_bits(math::expm1(1e-30f), 1e-30f),
            "expm1(float): a special value");
    // log1p.
    require(same_bits(math::log1p(0.0), 0.0) && same_bits(math::log1p(-0.0), -0.0) &&
                same_bits(math::log1p(0x1p-1074), 0.0) && same_bits(math::log1p(-1.0), -inf) &&
                std::isnan(math::log1p(-2.0)) && std::isnan(math::log1p(-inf)) && same_bits(math::log1p(inf), inf) &&
                std::isnan(math::log1p(nan)) && same_bits(math::log1p(1e-300), 1e-300) &&
                std::isfinite(math::log1p(std::numeric_limits<double>::max())),
            "log1p(double): a special value");
    require(same_bits(math::log1p(0.0f), 0.0f) && same_bits(math::log1p(-0.0f), -0.0f) &&
                same_bits(math::log1p(0x1p-149f), 0.0f) && same_bits(math::log1p(-1.0f), -inf_f) &&
                std::isnan(math::log1p(-2.0f)) && same_bits(math::log1p(inf_f), inf_f) &&
                same_bits(math::log1p(1e-30f), 1e-30f),
            "log1p(float): a special value");
    std::printf("exp2, expm1, log1p: special values hold\n");
}

// sin and cos are sincos' s and c, bit for bit.
void fronts(const int count) {
    Lcg lcg{3004};
    const double specials[] = {0.0, -0.0, 0x1p-1074, 1e-10, 1.0, 3.141592653589793, 0x1p32, 0x1.0000000000001p32,
                               std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
    int checked = 0;
    for (int i = 0; i < count + 10; ++i) {
        const double x = i < 10 ? specials[i] : (lcg.unit() - 0.5) * std::exp2(70.0 * lcg.unit() - 30.0);
        double s = 0.0;
        double c = 0.0;
        stxlib::math::sincos(x, s, c);
        require(same_bits(stxlib::math::sin(x), s) && same_bits(stxlib::math::cos(x), c),
                "sin/cos(double): not sincos' results");
        const float xf = static_cast<float>(x);
        float sf = 0.0f;
        float cf = 0.0f;
        stxlib::math::sincos(xf, sf, cf);
        require(same_bits(stxlib::math::sin(xf), sf) && same_bits(stxlib::math::cos(xf), cf),
                "sin/cos(float): not sincos' results");
        checked += 1;
    }
    std::printf("sin, cos: sincos' bits at %d inputs, both precisions\n", checked);
}

// ---- half-turn trigonometry and cosine losses -------------------------------------
//
// sincospi, sinpi, cospi, sincpi, cosm1 and cosm1pi
// (src/algorithms/math/half_turn.cpp). The limits below are tests, not
// proofs: the exact head is proved (tools/math/half_turn_certificate.py),
// the rounding of the low parts is measured. The study (2026-11-18, 172,288
// inputs against MPFR) measured 0.500020 (sinpi), 0.500113 (cospi),
// 0.500295 (sincpi), 0.501493 (cosm1pi) and 0.500100 (cosm1) ulp at worst; this sample found sincpi at 0.500748 until
// the rotation's second-order terms took the whole cell values (2026-11-22,
// half_turn.cpp), and now measures at most 0.500169 (cosm1pi).
constexpr double half_turn_ulp_limit = 0.5005;
constexpr double half_turn_loss_ulp_limit = 0.502;

// pi as a double-double (wide.hpp's pi/2 parts doubled, ~107 bits).
constexpr wide::Wide wide_pi{2.0 * wide::half_pi_1, 2.0 * wide::half_pi_2};

// sin(pi m) and cos(pi m) for 0 <= m < 2^52: m = n/2 + f exactly with
// |f| <= 1/4, then wide.hpp's series at pi f and the quarter turn n mod 4.
wide::WideSinCos half_turn_reference(const double m) {
    const double n = std::nearbyint(2.0 * m);
    const double f = m - 0.5 * n;  // exact
    const wide::WideSinCos base = wide::series(wide::multiply(wide_pi, f));
    const std::int64_t quarter = static_cast<std::int64_t>(std::fmod(n, 4.0));
    wide::WideSinCos result{};
    if (quarter == 0) {
        result = base;
    } else if (quarter == 1) {
        result = wide::WideSinCos{base.c, wide::negate(base.s)};
    } else if (quarter == 2) {
        result = wide::WideSinCos{wide::negate(base.s), wide::negate(base.c)};
    } else {
        result = wide::WideSinCos{wide::negate(base.c), base.s};
    }
    return result;
}

// -2 s^2 for a double-double s.
wide::Wide negative_twice_square_reference(const wide::Wide s) {
    const wide::Wide square = wide::multiply(s, s);
    const wide::Wide result = wide::negate(wide::add(square, square));
    return result;
}

// One half-turn input of the region: [0, 2) uniform; tiny (2^-30 to 2^-8);
// near the 1/128 lattice; large magnitudes (2^20 to 2^51). Signs alternate.
double half_turn_input(const int region, Lcg& lcg) {
    double m = 0.0;
    if (region == 0) m = 2.0 * lcg.unit();
    if (region == 1) m = geometric(lcg, -30.0, -8.0);
    if (region == 2) {
        const double lattice = std::floor(256.0 * lcg.unit()) / 128.0;
        m = lattice + (lcg.unit() - 0.5) * geometric(lcg, -50.0, -9.0);
        m = std::fabs(m);
    }
    if (region == 3) m = geometric(lcg, 20.0, 51.0);
    const double result = lcg.unit() < 0.5 ? -m : m;
    return result;
}

void track_worst(const double error, const double x, double& worst, double& worst_x) {
    if (error > worst) {
        worst = error;
        worst_x = x;
    }
}

void half_turn_accuracy(const int per_region) {
    namespace m = stxlib::math;
    const char* const names[] = {"[0, 2)", "[2^-30, 2^-8]", "1/128 lattice", "[2^20, 2^51]"};
    Lcg lcg{3011};
    for (int region = 0; region < 4; ++region) {
        double worst_sine = 0.0;
        double worst_sine_x = 0.0;
        double worst_cosine = 0.0;
        double worst_cosine_x = 0.0;
        double worst_sinc = 0.0;
        double worst_sinc_x = 0.0;
        double worst_loss = 0.0;
        double worst_loss_x = 0.0;
        for (int i = 0; i < per_region; ++i) {
            const double x = half_turn_input(region, lcg);
            const double magnitude = std::fabs(x);
            const wide::WideSinCos exact = half_turn_reference(magnitude);
            double sine = 0.0;
            double cosine = 0.0;
            m::sincospi(x, sine, cosine);
            require(same_bits(sine, m::sinpi(x)) && same_bits(cosine, m::cospi(x)), "sincospi: not sinpi's and cospi's bits");
            require(same_bits(m::sinpi(-x), -sine) && same_bits(m::cospi(-x), cosine), "sinpi odd, cospi even");
            const wide::Wide signed_sine = std::signbit(x) ? wide::negate(exact.s) : exact.s;
            if (exact.s.hi != 0.0) track_worst(wide_ulps(sine, signed_sine), x, worst_sine, worst_sine_x);
            if (exact.c.hi != 0.0) track_worst(wide_ulps(cosine, exact.c), x, worst_cosine, worst_cosine_x);
            if (magnitude < 0x1p52 && exact.s.hi != 0.0) {
                const wide::Wide angle = wide::multiply(wide_pi, magnitude);
                track_worst(wide_ulps(m::sincpi(x), wide::divide(exact.s, angle)), x, worst_sinc, worst_sinc_x);
            }
            const wide::Wide half_sine = half_turn_reference(0.5 * magnitude).s;
            if (half_sine.hi != 0.0) {
                track_worst(wide_ulps(m::cosm1pi(x), negative_twice_square_reference(half_sine)), x, worst_loss,
                            worst_loss_x);
            }
        }
        std::printf("half-turn %-15s %10d inputs  sinpi %.6f at %a  cospi %.6f at %a  sincpi %.6f at %a  "
                    "cosm1pi %.6f at %a\n",
                    names[region], per_region, worst_sine, worst_sine_x, worst_cosine, worst_cosine_x, worst_sinc,
                    worst_sinc_x, worst_loss, worst_loss_x);
        require(worst_sine <= half_turn_ulp_limit && worst_cosine <= half_turn_ulp_limit, "sinpi/cospi: error above the limit");
        require(worst_sinc <= half_turn_ulp_limit, "sincpi: error above the limit");
        require(worst_loss <= half_turn_loss_ulp_limit, "cosm1pi: error above the limit");
    }
    // The radian function cosm1, against wide.hpp's radian reference, for
    // |x| in [2^-7, 2^32].
    double worst_loss = 0.0;
    double worst_loss_x = 0.0;
    for (int i = 0; i < per_region; ++i) {
        const double x = (lcg.unit() < 0.5 ? -1.0 : 1.0) * geometric(lcg, -7.0, 32.0);
        const double magnitude = std::fabs(x);
        const wide::Wide half_sine = wide::reference(0.5 * magnitude).s;
        track_worst(wide_ulps(m::cosm1(x), negative_twice_square_reference(half_sine)), x, worst_loss, worst_loss_x);
    }
    std::printf("radian cosm1 %10d inputs  max %.6f ulp at %a\n", per_region, worst_loss, worst_loss_x);
    require(worst_loss <= half_turn_ulp_limit, "cosm1: error above the limit");
    // There is no radian sinc (owner, 2026-11-22): sin(x) / x is
    // sincpi(x / pi), with pi and the quotient rounded to double. For |x| in
    // [2^-7, 2], before the first zero, the argument's relative error of
    // about 1.4 * 2^-53 (pi, then the quotient) moves the result by at most
    // about 2.4 ulp of sin(x) / x (at x = 2), so with sincpi's own rounding
    // the recipe the header gives stays within 4 ulp there.
    double worst_recipe = 0.0;
    double worst_recipe_x = 0.0;
    for (int i = 0; i < per_region; ++i) {
        const double x = (lcg.unit() < 0.5 ? -1.0 : 1.0) * geometric(lcg, -7.0, 1.0);
        const double magnitude = std::fabs(x);
        const wide::WideSinCos exact = wide::reference(magnitude);
        track_worst(wide_ulps(m::sincpi(x / wide_pi.hi), wide::divide(exact.s, wide::Wide{magnitude, 0.0})), x,
                    worst_recipe, worst_recipe_x);
    }
    std::printf("sincpi(x / pi) %8d inputs  max %.6f ulp of sin(x) / x at %a\n", per_region, worst_recipe,
                worst_recipe_x);
    require(worst_recipe <= 4.0, "sincpi(x / pi): further than 4 ulp from sin(x) / x");
}

// Exact cases, special values, the reductions and the float functions.
void half_turn_special_values() {
    namespace m = stxlib::math;
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double tiny = std::numeric_limits<double>::denorm_min();
    // Whole and half numbers are exact, the sine's zeros with x's sign.
    for (int n = -64; n <= 64; ++n) {
        const double x = static_cast<double>(n);
        const double zero = n < 0 ? -0.0 : 0.0;
        const double parity = (n % 2 == 0) ? 1.0 : -1.0;
        require(same_bits(m::sinpi(x), zero) && m::cospi(x) == parity, "sinpi/cospi at a whole number");
        // sin(pi (n + 1/2)) = cos(pi n) = (-1)^n; cos(pi (n + 1/2)) = +0.
        require(same_bits(m::cospi(x + 0.5), 0.0) && m::sinpi(x + 0.5) == parity, "sinpi/cospi at a half number");
        require(same_bits(m::cosm1pi(x), n % 2 == 0 ? -0.0 : -2.0), "cosm1pi at a whole number");
        if (n != 0) require(same_bits(m::sincpi(x), 0.0), "sincpi at a nonzero whole number");
    }
    require(same_bits(m::sinpi(-tiny), -0.0) && same_bits(m::cospi(tiny), 1.0), "half-turn: subnormal argument");
    require(m::cospi(0x1.fffffffffffffp52) == -1.0 && m::cospi(0x1p53) == 1.0 && same_bits(m::sinpi(-0x1p60), -0.0),
            "half-turn: parity of large whole numbers");
    require(std::isnan(m::sinpi(inf)) && std::isnan(m::cospi(-inf)) && std::isnan(m::cospi(nan)) &&
                std::isnan(m::cosm1pi(inf)) && std::isnan(m::cosm1pi(nan)),
            "half-turn: nonfinite argument");
    require(m::sincpi(0.0) == 1.0 && m::sincpi(-0.0) == 1.0 && m::sincpi(tiny) == 1.0 && same_bits(m::sincpi(inf), 0.0) &&
                same_bits(m::sincpi(-inf), 0.0) && std::isnan(m::sincpi(nan)),
            "sincpi: special values");
    require(same_bits(m::cosm1(0.0), -0.0) && same_bits(m::cosm1(-0.0), -0.0) && same_bits(m::cosm1(tiny), -0.0) &&
                same_bits(m::cosm1(0x1p-600), -0.0) && std::isnan(m::cosm1(0x1.0000000000001p32)) &&
                std::isnan(m::cosm1(inf)) && std::isnan(m::cosm1(nan)),
            "cosm1: special values");
    require(same_bits(m::cosm1pi(0.0), -0.0) && same_bits(m::cosm1pi(-0.0), -0.0) && same_bits(m::cosm1pi(0x1p-600), -0.0),
            "cosm1pi: special values");
    // From 2^44 the phase is reduced modulo 2 in whole numbers: 2^44 + j/256
    // is exact (the spacing there is 2^-8; at 2^45 it is 2^-7, so odd j
    // would round) and must give j/256's bits.
    for (int j = 0; j < 512; ++j) {
        const double phase = static_cast<double>(j) / 256.0;
        const double far = 0x1p44 + phase;
        require(same_bits(m::sinpi(far), m::sinpi(phase)) && same_bits(m::cospi(far), m::cospi(phase)),
                "half-turn: the reduction modulo 2 from 2^44");
    }
    // Float: the double function of the widened argument, rounded once (and
    // flushed).
    Lcg lcg{3012};
    for (int i = 0; i < 4096; ++i) {
        const float x = static_cast<float>((lcg.unit() - 0.5) * std::exp2(40.0 * lcg.unit() - 20.0));
        const double wide_x = static_cast<double>(x);
        float s = 0.0f;
        float c = 0.0f;
        m::sincospi(x, s, c);
        require(same_bits(s, flushed(static_cast<float>(m::sinpi(wide_x)))) &&
                    same_bits(c, flushed(static_cast<float>(m::cospi(wide_x)))) && same_bits(m::sinpi(x), s) &&
                    same_bits(m::cospi(x), c),
                "sincospi(float): not the double result rounded once");
        require(same_bits(m::sincpi(x), flushed(static_cast<float>(m::sincpi(wide_x)))) &&
                    same_bits(m::cosm1(x), flushed(static_cast<float>(m::cosm1(wide_x)))) &&
                    same_bits(m::cosm1pi(x), flushed(static_cast<float>(m::cosm1pi(wide_x)))),
                "half-turn (float): not the double result rounded once");
    }
    require(same_bits(m::sinpi(-0.0f), -0.0f) && same_bits(m::cospi(0.5f), 0.0f) && m::cospi(0x1.fffffep23f) == -1.0f &&
                m::cospi(0x1p24f) == 1.0f,
            "half-turn (float): exact cases");
    require(m::sincpi(0.5f) == 0x1.45f306p-1f && m::sincpi(0.0f) == 1.0f &&
                m::cosm1pi(0x1p-36f) == -0x1.3bd3ccp-70f && m::cosm1(0x1p-36f) == -0x1p-73f,
            "half-turn (float): reference values");
    std::printf("half-turn: special values, reductions and float functions hold\n");
}

// A reference row's value: a differing one is printed and counted.
void check_reference(int& differing, const char* const name, const double x, const double result, const double expected) {
    if (!same_bits(result, expected)) {
        differing += 1;
        std::fprintf(stderr, "%s(%a): result %a, reference %a\n", name, x, result, expected);
    }
}

// Correctly rounded values from the study (MPFR 4.2.2 at 256 bits,
// 2026-11-18; the losses through the half-angle identity in the reference
// too). Every row is expected exactly. Each lies far from a rounding
// boundary for a kernel of this accuracy except sincpi(1 - 2^-53), 2^-54 ulp
// below a midpoint, which sincpi's near-whole-number path decides exactly.
void half_turn_reference_values() {
    namespace m = stxlib::math;
    struct Row final {
        double x;
        double sine;
        double cosine;
        double sinc;
        double pi_loss;
        double radian_loss;
    };
    const Row rows[] = {
        {0x1.3fce88ac7919cp-1022, 0x1.f659ef13b0045p-1021, 0x1p+0, 0x1p+0, -0x0p+0, -0x0p+0},
        {0x1p-511, 0x1.921fb54442d18p-510, 0x1p+0, 0x1p+0, -0x1.3bd3cc9be45dep-1020, -0x0p+0},
        {0x1p-36, 0x1.921fb54442d18p-35, 0x1p+0, 0x1p+0, -0x1.3bd3cc9be45dep-70, -0x1p-73},
        {0x1.fffffffffffffp-9, 0x1.921d1fcdec784p-7, 0x1.fff62169b92dbp-1, 0x1.fffcb5ccd351fp-1, -0x1.3bd2c8da4951p-14,
         -0x1.ffffd55556c15p-18},
        {0x1.a8p-2, 0x1.ed740e7684963p-1, 0x1.111d262b1f677p-2, 0x1.7b576ab7a364dp-1, -0x1.77716cea704c4p-1,
         -0x1.5a230f1cf30fcp-4},
        {0x1.fffffffffffffp-1, 0x1.921fb54442d18p-52, -0x1p+0, 0x1p-53, -0x1p+1, -0x1.d6bafe095f2e7p-2},
        {0x1.0000000000001p+0, -0x1.921fb54442d18p-51, -0x1p+0, -0x1.ffffffffffffep-53, -0x1p+1, -0x1.d6bafe095f2ecp-2},
        {0x1.921fb54442d18p+2, 0x1.8dbfb48815e49p-1, 0x1.4265a4a213109p-1, 0x1.42672fc9aa464p-5, -0x1.7b34b6bbd9deep-2,
         -0x1.377ce858a5d48p-105},
        {0x1.864p+18, 0x0p+0, 0x1p+0, 0x0p+0, -0x0p+0, -0x1.6ab2a5e19722p-2},
        {0x1.fffffffffffp+31, -0x1.921fb49ee4ea6p-12, 0x1.fffffd8858675p-1, -0x1.ffffff2d72ddp-46, -0x1.3bd3cc5af3e1dp-24,
         -0x1.e30eb761a9edcp+0},
        {0x1p+32, 0x0p+0, 0x1p+0, 0x0p+0, -0x0p+0, -0x1.e30b055806644p+0},
        {0x1.fffffffffffffp+43, -0x1.921f0fe670071p-8, 0x1.fffd8858e8a92p-1, -0x1.ffff2d72e6e74p-54,
         -0x1.3bd38bab6d94cp-16, -0x1.418596f856448p-4},
        {0x1.0000000000001p+49, 0x1.87de2a6aea963p-2, 0x1.d906bcf328d46p-1, 0x1.f2f10e3ce6d4p-53, -0x1.37ca1866b95cfp-4,
         -0x1.2b67bb57f1105p-4},
    };
    // Every differing value is printed before the first failure, so one run
    // lists them all.
    int differing = 0;
    for (const Row& row : rows) {
        double sine = 0.0;
        double cosine = 0.0;
        m::sincospi(row.x, sine, cosine);
        check_reference(differing, "sincospi (sine)", row.x, sine, flushed(row.sine));
        check_reference(differing, "sincospi (cosine)", row.x, cosine, row.cosine);
        check_reference(differing, "sincpi", row.x, m::sincpi(row.x), row.sinc);
        check_reference(differing, "cosm1pi", row.x, m::cosm1pi(row.x), flushed(row.pi_loss));
        if (row.x <= 0x1p32) {
            check_reference(differing, "cosm1", row.x, m::cosm1(row.x), flushed(row.radian_loss));
        } else {
            require(std::isnan(m::cosm1(row.x)), "cosm1: the angle limit");
        }
    }
    require(differing == 0, "half-turn: a reference value differs");
    std::printf("half-turn: %d reference rows hold\n", static_cast<int>(sizeof(rows) / sizeof(rows[0])));
}

// ---- determinism -------------------------------------------------------------

STXLIB_MODULAR void hash_word(std::uint64_t& hash, const std::uint64_t word) {
    hash ^= word;
    hash *= 1099511628211ULL;
}

// The hash's inputs, the same bits on every host: no host library call
// (std::exp2 here gave x, and the host's exp2 is not correctly rounded:
// macOS's misrounds 483 of the first 300,000 such exponents, Neo, Python
// 3.15.2 against a 45-digit decimal reference; wasm32's musl rounds
// differently, so the M4 and node hashed different inputs), and the two
// draws sequenced (the operands of one * are unsequenced in C++). The spread
// is 2^-12 to 2^12, linear within each octave: floor and ldexp are exact.
double completion_input(Lcg& lcg) {
    const double sign_part = lcg.unit() - 0.5;
    const double exponent = 24.0 * lcg.unit() - 12.0;
    const double whole = std::floor(exponent);
    const double scale = std::ldexp(1.0 + (exponent - whole), static_cast<int>(whole));
    const double result = sign_part * scale;
    return result;
}

std::uint64_t completion_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{95};
    for (int i = 0; i < 1000000; ++i) {
        const double x = completion_input(lcg);
        const float xf = static_cast<float>(x);
        hash_word(hash, std::bit_cast<std::uint64_t>(stxlib::math::exp2(x)));
        hash_word(hash, std::bit_cast<std::uint64_t>(stxlib::math::expm1(x)));
        hash_word(hash, std::bit_cast<std::uint64_t>(stxlib::math::log1p(std::fabs(x))));
        hash_word(hash, std::bit_cast<std::uint32_t>(stxlib::math::exp2(xf)));
        hash_word(hash, std::bit_cast<std::uint32_t>(stxlib::math::expm1(xf)));
        hash_word(hash, std::bit_cast<std::uint32_t>(stxlib::math::log1p(std::fabs(xf))));
    }
    return hash;
}

// Not frozen (0) since the inputs became host-independent (was
// 0x119df3c383ff0bfe on the M4 and 8bff36753c803718 on wasm32, from the
// host's std::exp2): the integrator's freezing run (STXLIB_FREEZE_HASHES=1,
// both product forms and both hosts agreeing) writes it here. Until then the
// check fails outside freezing mode (frozen_hash.hpp).
constexpr std::uint64_t expected_completion_output_hash = 0xca9124667dc5040fULL;

void determinism() {
    const std::uint64_t hash = completion_output_hash();
    std::printf("completion output hash %016llx\n", static_cast<unsigned long long>(hash));
    if (expected_completion_output_hash == 0) {
        std::printf("completion output hash not frozen yet\n");
        require(stxlib_tests::freezing_hashes(),
                "completions: output hash not frozen (set STXLIB_FREEZE_HASHES=1 only to freeze it)");
        return;
    }
    require(hash == expected_completion_output_hash, "completions: outputs differ from the frozen bits");
}

// ---- --bench -----------------------------------------------------------------

struct BenchBuffers final {
    std::vector<double> x{};
    std::vector<double> out{};
    std::vector<float> xf{};
    std::vector<float> outf{};
};

using BenchKernel = void (*)(BenchBuffers& buffers);

template <double (*Function)(double)>
void unary_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.out[i] = Function(b.x[i]);
}

template <float (*Function)(float)>
void unary_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.outf[i] = Function(b.xf[i]);
}

double owned_exp2_d(const double x) { return stxlib::math::exp2(x); }
double library_exp2_d(const double x) { return std::exp2(x); }
float owned_exp2_f(const float x) { return stxlib::math::exp2(x); }
float library_exp2_f(const float x) { return std::exp2(x); }
double owned_expm1_d(const double x) { return stxlib::math::expm1(x); }
double library_expm1_d(const double x) { return std::expm1(x); }
float owned_expm1_f(const float x) { return stxlib::math::expm1(x); }
float library_expm1_f(const float x) { return std::expm1(x); }
double owned_log1p_d(const double x) { return stxlib::math::log1p(x); }
double library_log1p_d(const double x) { return std::log1p(x); }
float owned_log1p_f(const float x) { return stxlib::math::log1p(x); }
float library_log1p_f(const float x) { return std::log1p(x); }
double owned_sin_d(const double x) { return stxlib::math::sin(x); }
double library_sin_d(const double x) { return std::sin(x); }
float owned_sin_f(const float x) { return stxlib::math::sin(x); }
float library_sin_f(const float x) { return std::sin(x); }
double owned_cos_d(const double x) { return stxlib::math::cos(x); }
double library_cos_d(const double x) { return std::cos(x); }
float owned_cos_f(const float x) { return stxlib::math::cos(x); }
float library_cos_f(const float x) { return std::cos(x); }

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
    double low{0.0};
    double high{0.0};
    BenchKernel library_double{nullptr};
    BenchKernel owned_double{nullptr};
    BenchKernel library_float{nullptr};
    BenchKernel owned_float{nullptr};
};

// The owner's ceiling: at most twice the C library's time.
constexpr double speed_ceiling = 2.0;

double bench() {
    const BenchRow rows[] = {
        {"exp2 [-10, 10]", -10.0, 10.0, unary_double<library_exp2_d>, unary_double<owned_exp2_d>,
         unary_float<library_exp2_f>, unary_float<owned_exp2_f>},
        {"expm1 [-1e-3, 1e-3]", -1e-3, 1e-3, unary_double<library_expm1_d>, unary_double<owned_expm1_d>,
         unary_float<library_expm1_f>, unary_float<owned_expm1_f>},
        {"expm1 [-10, 10]", -10.0, 10.0, unary_double<library_expm1_d>, unary_double<owned_expm1_d>,
         unary_float<library_expm1_f>, unary_float<owned_expm1_f>},
        {"log1p [-1e-3, 1e-3]", -1e-3, 1e-3, unary_double<library_log1p_d>, unary_double<owned_log1p_d>,
         unary_float<library_log1p_f>, unary_float<owned_log1p_f>},
        {"log1p [0, 100]", 0.0, 100.0, unary_double<library_log1p_d>, unary_double<owned_log1p_d>,
         unary_float<library_log1p_f>, unary_float<owned_log1p_f>},
        {"sin [-pi, pi]", -3.14159, 3.14159, unary_double<library_sin_d>, unary_double<owned_sin_d>,
         unary_float<library_sin_f>, unary_float<owned_sin_f>},
        {"cos [-pi, pi]", -3.14159, 3.14159, unary_double<library_cos_d>, unary_double<owned_cos_d>,
         unary_float<library_cos_f>, unary_float<owned_cos_f>},
    };
    const std::size_t count = std::size_t{1} << 20;
    std::printf("ns per call, %zu inputs, best of 15\n", count);
    std::printf("%-22s %9s %9s %6s %9s %9s %6s\n", "function", "std dbl", "owned", "ratio", "std flt", "owned", "ratio");
    double worst = 0.0;
    for (const BenchRow& row : rows) {
        BenchBuffers b{};
        Lcg lcg{12345};
        b.x.resize(count);
        b.out.resize(count);
        b.xf.resize(count);
        b.outf.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            b.x[i] = row.low + (row.high - row.low) * lcg.unit();
            b.xf[i] = static_cast<float>(b.x[i]);
        }
        const double std_double = best_ns_per_call(row.library_double, b);
        const double our_double = best_ns_per_call(row.owned_double, b);
        const double std_float = best_ns_per_call(row.library_float, b);
        const double our_float = best_ns_per_call(row.owned_float, b);
        std::printf("%-22s %9.3f %9.3f %6.2f %9.3f %9.3f %6.2f\n", row.name, std_double, our_double,
                    our_double / std_double, std_float, our_float, our_float / std_float);
        worst = std::max(worst, std::max(our_double / std_double, our_float / std_float));
    }
    return worst;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    const bool exhaustive = argument_count == 2 && std::strcmp(arguments[1], "--exhaustive") == 0;
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        const double worst = bench();
        std::printf("worst ratio %.2f (ceiling %.1f)\n", worst, speed_ceiling);
        return 0;
    }
    try {
        special_values();
        fronts(exhaustive ? 10000000 : 100000);
        exp2_double_accuracy(exhaustive ? 20000000 : 100000);
        expm1_double_accuracy(exhaustive ? 20000000 : 100000);
        log1p_double_accuracy(exhaustive ? 20000000 : 100000);
        half_turn_special_values();
        half_turn_reference_values();
        half_turn_accuracy(exhaustive ? 20000000 : 100000);
        float_screen(exhaustive ? 1 : 997);
        determinism();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "math completion tests failed: %s\n", failure.what());
        return 1;
    }
    std::puts("math completion tests passed");
    return 0;
}
