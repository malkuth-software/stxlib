// Tests of exp2, expm1, log1p, sin and cos (stxlib/math.hpp; roadmap R1),
// against the double-double references of tests/reference/wide.hpp.
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

// ---- determinism -------------------------------------------------------------

STXLIB_MODULAR void hash_word(std::uint64_t& hash, const std::uint64_t word) {
    hash ^= word;
    hash *= 1099511628211ULL;
}

std::uint64_t completion_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{95};
    for (int i = 0; i < 1000000; ++i) {
        const double x = (lcg.unit() - 0.5) * std::exp2(24.0 * lcg.unit() - 12.0);
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

// Not yet frozen (0): the first M4 run of both builds sets it.
constexpr std::uint64_t expected_completion_output_hash = 0;

void determinism() {
    const std::uint64_t hash = completion_output_hash();
    std::printf("completion output hash %016llx\n", static_cast<unsigned long long>(hash));
    if (expected_completion_output_hash == 0) {
        std::printf("completion output hash not frozen yet\n");
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
        float_screen(exhaustive ? 1 : 997);
        determinism();
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "math completion tests failed: %s\n", failure.what());
        return 1;
    }
    std::puts("math completion tests passed");
    return 0;
}
