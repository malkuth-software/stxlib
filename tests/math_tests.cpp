// Accuracy, edge-case and determinism tests for stxlib::math, measured
// against the double-double references in tests/reference/wide.hpp.
//
//   math_tests               sampled checks (the ctest run)
//   math_tests --exhaustive  every float on each kernel's fast path and 20
//                            million doubles per region
//   math_tests --bench       ns per call against the C++ library
//   math_tests --bench-gate  the same, failing above 2x the C++ library on
//                            any gated region (the owner's ceiling)
//
// The determinism check hashes outputs over fixed inputs and compares the
// hash with a frozen value: a different architecture, compiler or fma use
// that changed any bit fails it.

#include "stxlib/math.hpp"

#include "reference/sincos_worst_cases.hpp"
#include "reference/wide.hpp"
#include "core/checked.hpp"
#include "algorithms/math/paths.hpp"
// The certified worst-case test calls the reduction directly. src/algorithms/math/
// compiles once per kernel variant (src/algorithms/math/variant.hpp); this test compiles
// its own copy as the native variant, which every variant matches bit for bit.
// The Dekker test target defines the variant already.
#if !defined(STXLIB_MATH_VARIANT)
#define STXLIB_MATH_VARIANT native
#endif
#include "algorithms/math/sincos_core.hpp"
// STXlib batch 3c8: the checks carried from stx_auxlib call the unflushed
// functions (src/algorithms/math/dispatch.cpp: the kernel the SIMD level
// selects, without the subnormal rule), so their measurements and frozen
// hashes still describe stx_auxlib's bits; flush_rule() and
// public_special_values() check the public functions of stxlib/math.hpp,
// which apply the rule (flush.cpp), against them exactly. In the subnormal
// regions (exp's, pow's and hypot's subnormal results, log's subnormal
// arguments) the double checks verify the flush instead of measuring ulp.
// The bench times the public functions.
#define STXLIB_MATH_DECLARE_VARIANT unflushed
#include "algorithms/math/variant_functions.hpp"
#undef STXLIB_MATH_DECLARE_VARIANT

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace unflushed = stxlib::math::unflushed;

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) throw std::runtime_error(message);
}

// Misrounded floats found by the float screens: each is printed and counted,
// the screen goes on, and main fails once every check has run, so one
// exhaustive run lists every misrounded float of every function (a throw at
// the first one hid the rest; R3).
std::uint64_t misrounded_floats = 0;

void report_misrounded(const char* const name, const double x, const double result, const double expected) {
    misrounded_floats += 1;
    std::fprintf(stderr, "%s(%a): result %a, correctly rounded %a\n", name, x, result, expected);
}

// A frozen output hash per kernel path (src/algorithms/math/paths.hpp):
// `expected` is the value for the paths this build's switches choose; 0
// means not frozen yet for those paths (printed with today's frozen value,
// to freeze from the first M4 run), so it does not fail.
void check_frozen(const char* const name, const std::uint64_t hash, const std::uint64_t expected,
                  const std::uint64_t today_value, const char* const message) {
    std::printf("%s output hash %016llx\n", name, static_cast<unsigned long long>(hash));
    if (expected == 0) {
        std::printf("%s output hash not frozen yet for this build's region paths (today's paths: %016llx)\n", name,
                    static_cast<unsigned long long>(today_value));
        return;
    }
    require(hash == expected, message);
}

// Deterministic inputs: a 64-bit LCG.
struct Lcg final {
    std::uint64_t state{0};

    // A 64-bit linear congruential step: arithmetic modulo 2^64 by design.
    STXLIB_MODULAR double unit() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double result = static_cast<double>(state >> 11) * 0x1.0p-53;
        return result;
    }

    // 64 bits from the high halves of two steps (STXlib batch 3c8, for
    // random bit patterns: every exponent equally likely).
    STXLIB_MODULAR std::uint64_t bits() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t high = state >> 32;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t result = (high << 32) | (state >> 32);
        return result;
    }
};

// |approx - exact| in ulps of the correctly rounded double.
double double_ulps(const double approx, const wide::Wide exact) {
    const double rounded = std::fabs(exact.hi + exact.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{approx, 0.0}, wide::negate(exact));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// sin and cos of a double: the stated bound, proved for every double
// 2^-27 <= |x| <= 2^32 by tools/math/sincos_certificate.py (the rotation's own
// error is at most 0.0011 ulp; the reduction is held to the rest). Measured
// worst over the sampled regions: 0.500415 ulp.
constexpr double double_ulp_limit = sincos_worst_cases::stated_bound_ulps;
// exp: measured 0.500001 ulp over 100 million inputs.
constexpr double exp_ulp_limit = 0.5005;
// log, log2, log10: measured 0.500000 ulp over 24 million inputs.
constexpr double log_ulp_limit = 0.5005;
// pow: the analytic bound, 0.5 + 0.076 ulp (src/algorithms/math/pow.cpp); measured
// 0.5027 ulp at worst over 2 million inputs against MPFR (2026-11-02).
constexpr double pow_ulp_limit = 0.576;
// hypot: measured 0.500000 ulp over 10 million inputs (none misrounded).
constexpr double hypot_ulp_limit = 0.5005;
// atan, atan2, asin, acos: measured 0.500000 ulp over 4 million inputs each.
constexpr double inverse_trig_ulp_limit = 0.5005;
// tan: the stated bound, proved like sincos' (the rotation and quotient
// together at most 0.00098 ulp with the 26-bit split). Measured worst over
// the sampled regions: 0.500154 ulp (0.499996 after the split). cbrt and the hyperbolic family (remaining_ulp_limit):
// measured 0.500000 ulp, sampled only.
constexpr double tan_ulp_limit = sincos_worst_cases::stated_bound_ulps;
constexpr double remaining_ulp_limit = 0.5005;

// Whether a float screen may trust the C++ library's double rounded to
// float as the correctly rounded float: not when that double lies within
// the library's own error of a float midpoint, where rounding twice can land
// on the wrong side. (The screens passed sincos(float) at +-9830.3984375 so:
// the library's sin, correctly rounded to double, is the midpoint itself,
// and so was the kernel's double; tools/math/sincos_float_screen.py found
// it.) Within 2^-50 of a midpoint (relative; the library is within an ulp,
// 2^-52), the reference decides.
bool screen_holds(const double library_value) {
    if (!std::isfinite(library_value) || library_value == 0.0) return true;
    const float rounded = static_cast<float>(library_value);
    if (!std::isfinite(rounded)) return true;
    const double value = static_cast<double>(rounded);
    const double up = static_cast<double>(std::nextafter(rounded, std::numeric_limits<float>::infinity()));
    const double down = static_cast<double>(std::nextafter(rounded, -std::numeric_limits<float>::infinity()));
    const double distance = std::min(std::fabs(library_value - (value + up) * 0.5),
                                     std::fabs(library_value - (value + down) * 0.5));
    const bool result = distance > std::fabs(library_value) * 0x1p-50;
    return result;
}

// hi + lo rounded once to float.
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

struct ReferenceAnchor final {
    double x{0.0};
    double sine{0.0};
    double cosine{0.0};
};

void check_reference() {
    Lcg lcg{7};
    double worst = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double x = (2.0 * lcg.unit() - 1.0) * 1000.0;
        const wide::WideSinCos r = wide::reference(x);
        const wide::Wide one = wide::add(wide::multiply(r.s, r.s), wide::multiply(r.c, r.c));
        const wide::Wide deviation = wide::add(one, wide::Wide{-1.0, 0.0});
        worst = std::max(worst, std::fabs(deviation.hi + deviation.lo));
    }
    require(worst < 1e-30, "reference: sin^2 + cos^2 departs from 1");
    // Recorded correctly rounded sin and cos, from a 420-digit Taylor series
    // of the exact double input (not the host C library, which may differ by
    // host). The reference rounds to within half an ulp of them, plus its own
    // error far below 0.01 ulp.
    const ReferenceAnchor anchors[] = {
        {0.5, 0x1.eaee8744b05f0p-2, 0x1.c1528065b7d50p-1},
        {1.0, 0x1.aed548f090ceep-1, 0x1.14a280fb5068cp-1},
        {2.0, 0x1.d18f6ead1b446p-1, -0x1.aa22657537205p-2},
        {3.0, 0x1.210386db6d55bp-3, -0x1.fae04be85e5d2p-1},
        {1e-8, 0x1.5798ee2308c3ap-27, 0x1.0p+0},
        {100.0, -0x1.03425b78c4db8p-1, 0x1.b981dbf665fdfp-1},
        {710.0, 0x1.f9bd0303f6fafp-15, 0x1.fffffff063930p-1},
    };
    for (const ReferenceAnchor& anchor : anchors) {
        const wide::WideSinCos r = wide::reference(anchor.x);
        require(double_ulps(anchor.sine, r.s) < 0.51 && double_ulps(anchor.cosine, r.c) < 0.51,
                "reference: disagrees with a recorded correctly rounded value");
    }
}

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

// STXlib's subnormal rule, written here independently of flush.cpp (by
// classification, not by bits): a subnormal value becomes zero of its sign.
bool subnormal(const double value) {
    return std::fpclassify(value) == FP_SUBNORMAL;
}

bool subnormal(const float value) {
    return std::fpclassify(value) == FP_SUBNORMAL;
}

double flushed(const double value) {
    if (subnormal(value)) return std::copysign(0.0, value);
    return value;
}

float flushed(const float value) {
    if (subnormal(value)) return std::copysign(0.0f, value);
    return value;
}

// Inputs off the computed range must return exactly what the stated rule
// says: x and 1 below 2^-27 (keeping the sign of zero), NaN above 2^32 and
// for infinities and NaN. Edges of the computed range must meet the bound.
void special_values() {
    const double tiny[] = {0.0, -0.0, 0x1p-1074, -0x1p-1030, 1e-300, 0x1.fffffffffffffp-28};
    for (const double x : tiny) {
        double s = 0.0;
        double c = 0.0;
        unflushed::sincos(x, s, c);
        require(same_bits(s, x) && same_bits(c, 1.0), "sincos(double): tiny input is not (x, 1)");
        const float xf = static_cast<float>(x);
        float sf = 0.0f;
        float cf = 0.0f;
        unflushed::sincos(xf, sf, cf);
        require(same_bits(sf, xf) && same_bits(cf, 1.0f), "sincos(float): tiny input is not (x, 1)");
    }
    const double refused[] = {0x1.0000000000001p32, -0x1.0000000000001p32, 1e300,
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()};
    for (const double x : refused) {
        double s = 0.0;
        double c = 0.0;
        unflushed::sincos(x, s, c);
        require(std::isnan(s) && std::isnan(c) && std::isnan(unflushed::tan(x)),
                "sincos/tan(double): an angle above 2^32 was not refused");
        const float xf = static_cast<float>(x);
        if (std::isfinite(xf) && std::fabs(xf) <= 0x1p32f) continue;
        float sf = 0.0f;
        float cf = 0.0f;
        unflushed::sincos(xf, sf, cf);
        require(std::isnan(sf) && std::isnan(cf) && std::isnan(unflushed::tan(xf)),
                "sincos/tan(float): an angle above 2^32 was not refused");
    }
    const double edges[] = {0x1p-27, -0x1p-27, 0x1p20, -0x1p20, 0x1.fffffffffffffp19,
                            0x1.0000000000001p20, 0x1p32, -0x1p32};
    for (const double x : edges) {
        double s = 0.0;
        double c = 0.0;
        unflushed::sincos(x, s, c);
        const wide::WideSinCos exact = wide::reference(x);
        require(std::max(double_ulps(s, exact.s), double_ulps(c, exact.c)) <= double_ulp_limit,
                "sincos(double): edge above the stated bound");
    }
}

struct Region final {
    const char* name{nullptr};
    double low{0.0};
    double high{0.0};
    // Whether --bench-gate holds this region to the 2x ceiling. Edge regions
    // where a C library has a dedicated shortcut (exp next to its overflow
    // and underflow thresholds) or that only owned code serves (sincos
    // beyond 2^20) are reported but not gated.
    bool gated{true};
};

const Region double_regions[] = {
    {"|x| < 2pi", -6.283185307179586, 6.283185307179586},
    {"|x| < 1000", -1000.0, 1000.0},
    {"|x| < 2^20", -1048576.0, 1048576.0},
    {"[0, 0.1)", 0.0, 0.1},
    {"pi/2 +- 0.05", 1.5207963267948966, 1.6207963267948966},
    {"pi +- 0.05", 3.0915926535897931, 3.1915926535897931},
    {"2^20..2^32", 1048576.0, 4294967296.0, false},
};

// The double nearest k * pi/128 for random k up to 2^38: the remainder is
// then tiny, the hardest case for the medium-range reduction.
void sincos_hard_reduction(const int count) {
    Lcg lcg{2038};
    double worst = 0.0;
    double worst_x = 0.0;
    for (int i = 0; i < count; ++i) {
        const double k = std::floor(lcg.unit() * 0x1p38);
        const wide::Wide step = wide::multiply(wide::Wide{wide::half_pi_1, wide::half_pi_2}, 1.0 / 64.0);
        const wide::Wide nearest = wide::multiply(step, k);
        const double x = nearest.hi + nearest.lo;
        if (std::fabs(x) > 0x1p32) continue;
        double s = 0.0;
        double c = 0.0;
        unflushed::sincos(x, s, c);
        const wide::WideSinCos exact = wide::reference(x);
        const double error = std::max(double_ulps(s, exact.s), double_ulps(c, exact.c));
        if (error > worst) {
            worst = error;
            worst_x = x;
        }
        require(double_ulps(unflushed::tan(x), wide::tan_reference(x)) <= tan_ulp_limit,
                "tan(double): hard reduction case above the stated bound");
    }
    std::printf("sincos(double) near k*pi/128 %10d inputs  max %.6f ulp at %a\n", count, worst, worst_x);
    require(worst <= double_ulp_limit, "sincos(double): hard reduction case above the stated bound");
}

// The certified worst cases (tests/reference/sincos_worst_cases.hpp, from
// tools/math/sincos_certificate.py): for every binade and residue class, the
// double nearest a multiple of pi/128. At each, and at its negation, the
// reduction must deliver r + r_lo within the certificate's contract of the
// exact remainder, and sin, cos and tan must be within the stated bound of
// the exact values the certificate computed (not of the reference).
void sincos_certified_worst_cases() {
    namespace detail = stxlib::math::STXLIB_MATH_VARIANT::detail;
    int checked = 0;
    for (const sincos_worst_cases::Case& entry : sincos_worst_cases::cases) {
        for (int sign = 0; sign < 2; ++sign) {
            const double flip = sign == 0 ? 1.0 : -1.0;
            const double x = flip * entry.x;
            std::uint64_t index = 0;
            double r = 0.0;
            double r_lo = 0.0;
            double q = 0.0;
            if (detail::sincos_fast_path(x)) {
                q = detail::sincos_reduce_fast(x, index, r, r_lo);
            } else {
                q = detail::sincos_step(x, index);
                detail::sincos_remainder_medium(x, q, r, r_lo);
            }
            require(q == flip * entry.q, "sincos worst case: the reduction chose another multiple");
            const wide::Wide exact_remainder{flip * entry.remainder_hi, flip * entry.remainder_lo};
            const wide::Wide difference = wide::add(wide::two_sum(r, r_lo), wide::negate(exact_remainder));
            const double reduction_error = std::fabs(difference.hi + difference.lo);
            const double allowed = std::min(sincos_worst_cases::contract_kappa * std::fabs(entry.remainder_hi),
                                            sincos_worst_cases::contract_general);
            if (reduction_error > allowed) {
                std::fprintf(stderr, "x = %a: reduction error %a above the contract %a\n", x, reduction_error,
                             allowed);
            }
            require(reduction_error <= allowed, "sincos worst case: reduction outside the certified contract");
            double s = 0.0;
            double c = 0.0;
            unflushed::sincos(x, s, c);
            const double t = unflushed::tan(x);
            const double sin_error = double_ulps(s, wide::Wide{flip * entry.sin_hi, flip * entry.sin_lo});
            const double cos_error = double_ulps(c, wide::Wide{entry.cos_hi, entry.cos_lo});
            const double tan_error = double_ulps(t, wide::Wide{flip * entry.tan_hi, flip * entry.tan_lo});
            if (sin_error > double_ulp_limit || cos_error > double_ulp_limit || tan_error > tan_ulp_limit) {
                std::fprintf(stderr, "x = %a: sin %.6f cos %.6f tan %.6f ulp\n", x, sin_error, cos_error, tan_error);
            }
            require(sin_error <= double_ulp_limit && cos_error <= double_ulp_limit,
                    "sincos worst case: above the stated bound");
            require(tan_error <= tan_ulp_limit, "tan worst case: above the stated bound");
            checked += 1;
        }
    }
    std::printf("sincos(double), tan(double): %d certified worst cases within %.2f ulp\n", checked,
                double_ulp_limit);
}

void double_accuracy(const int per_region) {
    Lcg lcg{2026};
    for (const Region& region : double_regions) {
        double worst = 0.0;
        double worst_x = 0.0;
        std::uint64_t misrounded = 0;
        for (int i = 0; i < per_region; ++i) {
            const double x = region.low + (region.high - region.low) * lcg.unit();
            double s = 0.0;
            double c = 0.0;
            unflushed::sincos(x, s, c);
            const wide::WideSinCos exact = wide::reference(x);
            const double error = std::max(double_ulps(s, exact.s), double_ulps(c, exact.c));
            if (error > 0.5) misrounded += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("sincos(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                    region.name, per_region, worst, worst_x,
                    static_cast<unsigned long long>(misrounded));
        require(worst <= double_ulp_limit, "sincos(double): error above the stated bound");
    }
}

// Every `stride`-th float in [2^-27, 2^32], positive and negated: the result
// must be the correctly rounded sin and cos. The double libm result rounded
// to float screens; the reference decides where the screen disagrees.
void float_rounding(const std::uint32_t stride) {
    const std::uint32_t first = std::bit_cast<std::uint32_t>(0x1p-27f);
    const std::uint32_t last = std::bit_cast<std::uint32_t>(0x1p32f);
    std::uint64_t checked = 0;
    for (std::uint32_t bits = first; bits <= last; bits += stride) {
        const float magnitude = std::bit_cast<float>(bits);
        const float signs[2] = {magnitude, -magnitude};
        for (const float x : signs) {
            float s = 0.0f;
            float c = 0.0f;
            unflushed::sincos(x, s, c);
            const double xd = static_cast<double>(x);
            const double library_s = std::sin(xd);
            const double library_c = std::cos(xd);
            const float screen_s = static_cast<float>(library_s);
            const float screen_c = static_cast<float>(library_c);
            checked += 1;
            if (s == screen_s && c == screen_c && screen_holds(library_s) && screen_holds(library_c)) continue;
            const wide::WideSinCos exact = wide::reference(xd);
            const float expected_s = correctly_rounded_float(exact.s);
            const float expected_c = correctly_rounded_float(exact.c);
            if (s != expected_s) report_misrounded("sin", xd, static_cast<double>(s), static_cast<double>(expected_s));
            if (c != expected_c) report_misrounded("cos", xd, static_cast<double>(c), static_cast<double>(expected_c));
        }
    }
    std::printf("sincos(float) correctly rounded at %llu floats (stride %u)\n",
                static_cast<unsigned long long>(checked), stride);
}

// The float the double kernel misrounds, which sincos.cpp's exception table
// corrects (tools/math/sincos_float_screen.py found it; the screen above
// had passed it, see screen_holds): sin(9830.3984375) lies 2^-54 past a float
// midpoint.
void float_exceptions() {
    const float magnitude = 0x1.33333p13f;
    const float signs[2] = {magnitude, -magnitude};
    for (const float x : signs) {
        float s = 0.0f;
        float c = 0.0f;
        unflushed::sincos(x, s, c);
        const wide::WideSinCos exact = wide::reference(static_cast<double>(x));
        require(s == correctly_rounded_float(exact.s) && c == correctly_rounded_float(exact.c),
                "sincos(float): the exception at 9830.3984375 is not correctly rounded");
        require(std::bit_cast<std::uint32_t>(s) == (x > 0.0f ? 0xbeb1fa5du : 0x3eb1fa5du),
                "sincos(float): the exception at 9830.3984375 returned other bits");
    }
    std::printf("sincos(float) exception at +-9830.3984375 correct\n");
}

// FNV-1a over double and float outputs at fixed inputs, all on the fast or
// tiny paths (no C++ library fallback, whose bits are the host's).
// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{99};
    const double ranges[] = {6.283185307179586, 100.0, 1e6, 0.1};
    for (const double range : ranges) {
        for (int i = 0; i < 4000000; ++i) {
            const double x = (2.0 * lcg.unit() - 1.0) * range;
            double s = 0.0;
            double c = 0.0;
            unflushed::sincos(x, s, c);
            float sf = 0.0f;
            float cf = 0.0f;
            unflushed::sincos(static_cast<float>(x), sf, cf);
            const std::uint64_t words[4] = {std::bit_cast<std::uint64_t>(s),
                                            std::bit_cast<std::uint64_t>(c),
                                            std::bit_cast<std::uint32_t>(sf),
                                            std::bit_cast<std::uint32_t>(cf)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
// Replaced on 2026-11-02 (was 0xde89d2d2ed037d01): owner's 26-bit split,
// adopted 2026-11-02; 46/8M sincos and 64/4M tan outputs differ, all within
// the unchanged bound. Same value at the scalar, vector128 and avx2_fma
// levels and in the Dekker build (x86-64 MinGW).
constexpr std::uint64_t today_output_hash = 0x9cecbc587874960dULL;
// The region path of sincos(double) (trig_region.cpp): frozen on 2026-11-15
// (sbi, main f5cbc30, from the M4 run on 45b275b; fused and Dekker builds
// agree). It differs from today's (the polynomial chains round differently,
// within the same bound). Float sincos has one path.
constexpr std::uint64_t region_output_hash = 0xa3564fda3e1d73c8ULL;
constexpr std::uint64_t expected_output_hash = STXLIB_MATH_REGION_TRIG_DOUBLE ? region_output_hash
                                                                               : today_output_hash;

void determinism() {
    const std::uint64_t hash = output_hash();
    check_frozen("sincos", hash, expected_output_hash, today_output_hash,
                 "sincos: outputs differ from the frozen bits");
}

// ---- exp ----------------------------------------------------------------------

// |approx - exact| in ulps of the correctly rounded double, for a normal
// approx, compared at the reference's scale (scaling by 2^-exponent is exact).
double exp_ulps(const double approx, const wide::ScaledWide exact) {
    const double scaled = std::ldexp(approx, -exact.exponent);
    const double rounded = std::fabs(exact.mantissa.hi + exact.mantissa.lo);
    const double ulp = std::nextafter(rounded, std::numeric_limits<double>::infinity()) - rounded;
    const wide::Wide difference = wide::add(wide::Wide{scaled, 0.0}, wide::negate(exact.mantissa));
    const double result = std::fabs(difference.hi + difference.lo) / ulp;
    return result;
}

// mantissa * 2^exponent rounded once to float; valid while hi and lo stay
// normal doubles after scaling (|x| <= 110 here).
float exp_correctly_rounded_float(const wide::ScaledWide exact) {
    const wide::Wide value{std::ldexp(exact.mantissa.hi, exact.exponent),
                           std::ldexp(exact.mantissa.lo, exact.exponent)};
    const float result = correctly_rounded_float(value);
    return result;
}

void exp_check_reference() {
    const wide::ScaledWide one = wide::exp_reference(1.0);
    const wide::Wide e_value{std::ldexp(one.mantissa.hi, one.exponent),
                             std::ldexp(one.mantissa.lo, one.exponent)};
    const wide::Wide difference = wide::add(e_value, wide::negate(wide::e_constant));
    require(std::fabs(difference.hi + difference.lo) < 1e-31,
            "exp reference: e^1 departs from the independent constant for e");
    // e^n against e multiplied n times: catches an error in the ln 2 parts,
    // which grows with k.
    wide::Wide power{1.0, 0.0};
    for (int n = 1; n <= 600; ++n) {
        power = wide::multiply(power, wide::e_constant);
        const wide::ScaledWide exact = wide::exp_reference(static_cast<double>(n));
        const wide::Wide scaled{std::ldexp(exact.mantissa.hi, exact.exponent),
                                std::ldexp(exact.mantissa.lo, exact.exponent)};
        const wide::Wide gap = wide::add(scaled, wide::negate(power));
        require(std::fabs(gap.hi + gap.lo) / power.hi < 1e-29,
                "exp reference: e^n departs from e multiplied n times");
    }
}

// e^x rounded once onto the subnormal grid (multiples of 2^-1074), for
// results below 2^-1022: mantissa * 2^exponent in units of 2^-1074 is a
// normal double-double, rounded here to the nearest integer.
double exp_correctly_rounded_subnormal(const wide::ScaledWide exact) {
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

// exp owns every input: NaN, infinities, overflow, the subnormal range and
// underflow to zero follow the stated rules, and e^0 is exactly 1.
void exp_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    require(std::isnan(unflushed::exp(nan)) &&
                std::isnan(unflushed::exp(static_cast<float>(nan))),
            "exp: NaN did not return NaN");
    require(same_bits(unflushed::exp(infinity), infinity) &&
                same_bits(unflushed::exp(-infinity), 0.0),
            "exp(double): infinities do not give +infinity and +0");
    require(same_bits(unflushed::exp(static_cast<float>(infinity)),
                      std::numeric_limits<float>::infinity()) &&
                same_bits(unflushed::exp(static_cast<float>(-infinity)), 0.0f),
            "exp(float): infinities do not give +infinity and +0");
    const double overflowing[] = {709.7827128933841, 709.79, 710.0, 1e300};
    for (const double x : overflowing) {
        require(same_bits(unflushed::exp(x), infinity), "exp(double): did not overflow");
    }
    require(std::isfinite(unflushed::exp(709.782712893384)),
            "exp(double): overflowed below ln(DBL_MAX)");
    const double vanishing[] = {-745.1332191019412, -746.0, -800.0, -1e300};
    for (const double x : vanishing) {
        require(same_bits(unflushed::exp(x), 0.0), "exp(double): did not underflow to +0");
    }
    require(unflushed::exp(-745.13321910194) == 0x1p-1074,
            "exp(double): the smallest subnormal result is wrong");
    require(same_bits(unflushed::exp(0.0), 1.0) && same_bits(unflushed::exp(-0.0), 1.0),
            "exp(double): e^0 is not exactly 1");
    require(same_bits(unflushed::exp(0.0f), 1.0f), "exp(float): e^0 is not exactly 1");
    require(std::isinf(unflushed::exp(89.0f)), "exp(float): 89 did not overflow");
    require(unflushed::exp(-104.0f) == 0.0f, "exp(float): -104 did not underflow");
    const double edges[] = {708.0, -708.0, 1e-300, -1e-300, 0x1p-54, -0x1p-54, 709.78, -708.39};
    for (const double x : edges) {
        require(exp_ulps(unflushed::exp(x), wide::exp_reference(x)) <= exp_ulp_limit,
                "exp(double): edge above the stated bound");
    }
}

// The ranges beyond the fast path: (708, ln(DBL_MAX)] against the bound,
// and the subnormal results in [ln(2^-1075), ln(2^-1022)) against the
// correctly rounded subnormal.
void exp_double_outside(const int count) {
    Lcg lcg{2028};
    double worst = 0.0;
    for (int i = 0; i < count; ++i) {
        const double x = 708.0 + (709.7827128933840 - 708.0) * lcg.unit();
        worst = std::max(worst, exp_ulps(unflushed::exp(x), wide::exp_reference(x)));
    }
    std::printf("exp(double) (708, 709.78]   %10d inputs  max %.6f ulp\n", count, worst);
    require(worst <= exp_ulp_limit, "exp(double): error above the bound near overflow");
    // STXlib: the subnormal results flush to +0 (math.hpp), verified
    // exactly; within one subnormal ulp of 2^-1022 the kernel's rounding
    // decides between +0 and 2^-1022.
    std::uint64_t flushed_count = 0;
    std::uint64_t boundary_count = 0;
    for (int i = 0; i < count; ++i) {
        const double x = -745.1332191019412 + (745.1332191019412 - 708.3964185322641) * lcg.unit();
        const double y = stxlib::math::exp(x);
        require(same_bits(y, flushed(unflushed::exp(x))), "exp(double): not the flushed kernel result");
        const double nearest = exp_correctly_rounded_subnormal(wide::exp_reference(x));
        if (nearest < 0x1p-1022 - 0x1p-1074) {
            require(same_bits(y, 0.0), "exp(double): a subnormal result was not flushed to +0");
            flushed_count += 1;
        } else {
            require(same_bits(y, 0.0) || y >= 0x1p-1022, "exp(double): a subnormal result near 2^-1022");
            boundary_count += 1;
        }
    }
    std::printf("exp(double) subnormal range  %10d inputs  %llu flushed to +0, %llu at the 2^-1022 boundary\n",
                count, static_cast<unsigned long long>(flushed_count),
                static_cast<unsigned long long>(boundary_count));
}

const Region exp_regions[] = {
    {"|x| <= 708", -708.0, 708.0},
    {"|x| <= 1", -1.0, 1.0},
    {"|x| <= 1e-3", -1e-3, 1e-3},
    {"[700, 708]", 700.0, 708.0, false},
    {"[-708, -700]", -708.0, -700.0, false},
};

void exp_double_accuracy(const int per_region) {
    Lcg lcg{2027};
    for (const Region& region : exp_regions) {
        double worst = 0.0;
        double worst_x = 0.0;
        std::uint64_t misrounded = 0;
        for (int i = 0; i < per_region; ++i) {
            const double x = region.low + (region.high - region.low) * lcg.unit();
            const double error = exp_ulps(unflushed::exp(x), wide::exp_reference(x));
            if (error > 0.5) misrounded += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
            }
        }
        std::printf("exp(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                    region.name, per_region, worst, worst_x,
                    static_cast<unsigned long long>(misrounded));
        require(worst <= exp_ulp_limit, "exp(double): error above the stated bound");
    }
}

// Every `stride`-th finite float, both signs: the result must be the
// correctly rounded e^x (infinity, subnormals and zero included). Screened
// by the double libm result rounded to float; the reference decides where
// they disagree.
void exp_float_rounding(const std::uint32_t stride) {
    const std::uint32_t last = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    std::uint64_t checked = 0;
    for (std::uint64_t wide_bits = 0; wide_bits <= last; wide_bits += stride) {
        const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(wide_bits));
        const float signs[2] = {magnitude, -magnitude};
        for (const float x : signs) {
            const float y = unflushed::exp(x);
            const double xd = static_cast<double>(x);
            const double library = std::exp(xd);
            const float screen = static_cast<float>(library);
            checked += 1;
            if (y == screen && screen_holds(library)) continue;
            // Beyond these, float results are infinity or zero with margin.
            require(xd >= -110.0 && xd <= 90.0, "exp(float): wrong overflow or underflow");
            const float expected = exp_correctly_rounded_float(wide::exp_reference(xd));
            if (y != expected) report_misrounded("exp", xd, static_cast<double>(y), static_cast<double>(expected));
        }
    }
    std::printf("exp(float) correctly rounded at %llu floats (stride %u)\n",
                static_cast<unsigned long long>(checked), stride);
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t exp_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{98};
    // 760 covers overflow, the subnormal range and underflow: exp owns every
    // input, so no C++ library result enters the hash.
    const double ranges[] = {708.0, 50.0, 1.0, 1e-3, 760.0};
    for (const double range : ranges) {
        for (int i = 0; i < 4000000; ++i) {
            const double x = (2.0 * lcg.unit() - 1.0) * range;
            const double y = unflushed::exp(x);
            const float yf = unflushed::exp(static_cast<float>(x));
            const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y),
                                            std::bit_cast<std::uint32_t>(yf)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
// Replaced on 2026-11-02 (was 0xbad54eddef5ab9fe): owner's split, adopted
// 2026-11-02; 1/20M exp(double) outputs differ, within bound. Same value at
// the scalar, vector128 and avx2_fma levels and in the Dekker build (x86-64
// MinGW).
constexpr std::uint64_t expected_exp_output_hash = 0x1024ccb8a7c3c1adULL;

void exp_determinism() {
    const std::uint64_t hash = exp_output_hash();
    std::printf("exp output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_exp_output_hash, "exp: outputs differ from the frozen bits");
}

// ---- log, log2, log10 -----------------------------------------------------------

// One logarithm under test: the function pair and its base's 1/ln b as a
// double-double (1 for ln), which turns log_reference into this base.
using LogDouble = double (*)(double x);
using LogFloat = float (*)(float x);

struct LogKernel final {
    const char* name{nullptr};
    LogDouble owned_double{nullptr};
    LogFloat owned_float{nullptr};
    LogDouble library_double{nullptr};
    wide::Wide inverse{1.0, 0.0};
    LogDouble public_double{nullptr};  // STXlib: stxlib/math.hpp's, with the subnormal rule
};

double owned_log_d(const double x) { return unflushed::log(x); }
float owned_log_f(const float x) { return unflushed::log(x); }
double owned_log2_d(const double x) { return unflushed::log2(x); }
float owned_log2_f(const float x) { return unflushed::log2(x); }
double owned_log10_d(const double x) { return unflushed::log10(x); }
float owned_log10_f(const float x) { return unflushed::log10(x); }
double public_log_d(const double x) { return stxlib::math::log(x); }
double public_log2_d(const double x) { return stxlib::math::log2(x); }
double public_log10_d(const double x) { return stxlib::math::log10(x); }
double library_log_d(const double x) { return std::log(x); }
double library_log2_d(const double x) { return std::log2(x); }
double library_log10_d(const double x) { return std::log10(x); }

std::vector<LogKernel> log_kernels() {
    const wide::Wide ln2{wide::ln2_1, wide::ln2_2};
    const wide::Wide inv_ln2 = wide::divide(wide::Wide{1.0, 0.0}, ln2);
    const wide::Wide inv_ln10 = wide::divide(wide::Wide{1.0, 0.0}, wide::log_reference(10.0));
    std::vector<LogKernel> kernels{};
    kernels.push_back(LogKernel{"log", owned_log_d, owned_log_f, library_log_d, wide::Wide{1.0, 0.0}, public_log_d});
    kernels.push_back(LogKernel{"log2", owned_log2_d, owned_log2_f, library_log2_d, inv_ln2, public_log2_d});
    kernels.push_back(LogKernel{"log10", owned_log10_d, owned_log10_f, library_log10_d, inv_ln10, public_log10_d});
    return kernels;
}

wide::Wide log_exact(const LogKernel& kernel, const double x) {
    const wide::Wide natural = wide::log_reference(x);
    const wide::Wide result = wide::multiply(natural, kernel.inverse);
    return result;
}

// Logarithms own every input; the identities below are exact.
void log_special_values(const std::vector<LogKernel>& kernels) {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (const LogKernel& kernel : kernels) {
        require(same_bits((*kernel.owned_double)(0.0), -infinity) &&
                    same_bits((*kernel.owned_double)(-0.0), -infinity),
                "log(double): zero does not give -infinity");
        require(std::isnan((*kernel.owned_double)(-1.0)) &&
                    std::isnan((*kernel.owned_double)(-infinity)) &&
                    std::isnan((*kernel.owned_double)(nan)) &&
                    std::isnan((*kernel.owned_double)(-0x1p-1074)),
                "log(double): negative or NaN input does not give NaN");
        require(same_bits((*kernel.owned_double)(infinity), infinity),
                "log(double): +infinity does not give +infinity");
        require(same_bits((*kernel.owned_double)(1.0), 0.0) &&
                    same_bits((*kernel.owned_float)(1.0f), 0.0f),
                "log: log(1) is not +0");
        require(same_bits((*kernel.owned_float)(0.0f), -std::numeric_limits<float>::infinity()) &&
                    std::isnan((*kernel.owned_float)(-1.0f)) &&
                    same_bits((*kernel.owned_float)(std::numeric_limits<float>::infinity()),
                              std::numeric_limits<float>::infinity()),
                "log(float): special input broken");
        const double edges[] = {0x1p-1074, 0x1p-1022, 0x1.fffffffffffffp-1023, 0x1.fffffffffffffp1023,
                                0.5, 2.0, 0x1.0000000000001p0, 0x1.fffffffffffffp-1};
        for (const double x : edges) {
            const double error = double_ulps((*kernel.owned_double)(x), log_exact(kernel, x));
            require(error <= log_ulp_limit, "log(double): edge above the stated bound");
        }
    }
    for (int k = -1074; k <= 1023; ++k) {
        require(unflushed::log2(std::ldexp(1.0, k)) == static_cast<double>(k),
                "log2(double): log2(2^k) is not exactly k");
    }
    for (int k = -149; k <= 127; ++k) {
        require(unflushed::log2(std::ldexp(1.0f, k)) == static_cast<float>(k),
                "log2(float): log2(2^k) is not exactly k");
    }
    double power = 1.0;
    for (int n = 0; n <= 22; ++n) {
        require(unflushed::log10(power) == static_cast<double>(n),
                "log10(double): log10(10^n) is not exactly n");
        power *= 10.0;
    }
    float power_float = 1.0f;
    for (int n = 0; n <= 10; ++n) {
        require(unflushed::log10(power_float) == static_cast<float>(n),
                "log10(float): log10(10^n) is not exactly n");
        power_float *= 10.0f;
    }
}

// Inputs: random bit patterns over every exponent, [0.5, 2], 1 +- 1e-3 and
// subnormals.
double log_input(const int region, Lcg& lcg) {
    if (region == 0) {
        const double exponent = std::floor(lcg.unit() * 2046.0) - 1022.0;
        return std::ldexp(1.0 + lcg.unit(), static_cast<int>(exponent));
    }
    if (region == 1) return 0.5 + 1.5 * lcg.unit();
    if (region == 2) return 1.0 + (lcg.unit() - 0.5) * 2e-3;
    const double pattern = std::floor(lcg.unit() * 0x1p52) + 1.0;
    return std::ldexp(pattern, -1074);
}

const char* const log_region_names[] = {"all exponents", "[0.5, 2]", "1 +- 1e-3", "subnormal"};

void log_double_accuracy(const std::vector<LogKernel>& kernels, const int per_region) {
    for (const LogKernel& kernel : kernels) {
        Lcg lcg{2029};
        for (int region = 0; region < 4; ++region) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = log_input(region, lcg);
                if (region == 3) {
                    // STXlib: a subnormal argument reads as zero of its sign
                    // (math.hpp), so the public result is -infinity for
                    // either sign; verified exactly instead of measured.
                    const double infinity = std::numeric_limits<double>::infinity();
                    require(same_bits((*kernel.public_double)(x), -infinity) &&
                                same_bits((*kernel.public_double)(-x), -infinity),
                            "log(double): a subnormal argument did not give -infinity");
                    continue;
                }
                const double error = double_ulps((*kernel.owned_double)(x), log_exact(kernel, x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            if (region == 3) {
                std::printf("%s(double) %-14s %10d inputs  -infinity for both signs (verified)\n",
                            kernel.name, log_region_names[region], per_region);
                continue;
            }
            std::printf("%s(double) %-14s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, log_region_names[region], per_region, worst, worst_x,
                        static_cast<unsigned long long>(misrounded));
            require(worst <= log_ulp_limit, "log(double): error above the stated bound");
        }
    }
}

// Every `stride`-th positive finite float: the result must be the correctly
// rounded logarithm. Screened by the double libm result rounded to float.
void log_float_rounding(const std::vector<LogKernel>& kernels, const std::uint32_t stride) {
    const std::uint32_t last = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::max());
    for (const LogKernel& kernel : kernels) {
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = 1; pattern <= last; pattern += stride) {
            const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float y = (*kernel.owned_float)(x);
            const double xd = static_cast<double>(x);
            const double library = (*kernel.library_double)(xd);
            const float screen = static_cast<float>(library);
            checked += 1;
            if (y == screen && screen_holds(library)) continue;
            const float expected = correctly_rounded_float(log_exact(kernel, xd));
            if (y != expected) {
                report_misrounded(kernel.name, xd, static_cast<double>(y), static_cast<double>(expected));
            }
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t log_output_hash(const std::vector<LogKernel>& kernels) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const LogKernel& kernel : kernels) {
        Lcg lcg{97};
        for (int region = 0; region < 4; ++region) {
            for (int i = 0; i < 1000000; ++i) {
                const double x = log_input(region, lcg);
                const double y = (*kernel.owned_double)(x);
                const float yf = (*kernel.owned_float)(static_cast<float>(x));
                const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y),
                                                std::bit_cast<std::uint32_t>(yf)};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t today_log_output_hash = 0x2e4ea58329a77abcULL;
// The region path of the float logs (region.cpp): frozen on 2026-11-15 (sbi,
// main f5cbc30); equal to today's at every sampled input. Not because both
// were correctly rounded for every float, as this comment once predicted:
// today's log(float) misrounded 5 floats (R3), none of them in this sample,
// and today's cold fallback now calls the region kernel. A subnormal x takes
// today's kernel; math_region_tests --exhaustive compares every pattern.
constexpr std::uint64_t region_log_output_hash = 0x2e4ea58329a77abcULL;
constexpr std::uint64_t expected_log_output_hash = STXLIB_MATH_REGION_LOG_FLOAT ? region_log_output_hash
                                                                                : today_log_output_hash;

void log_determinism(const std::vector<LogKernel>& kernels) {
    const std::uint64_t hash = log_output_hash(kernels);
    check_frozen("log", hash, expected_log_output_hash, today_log_output_hash,
                 "log: outputs differ from the frozen bits");
}

// ---- pow ----------------------------------------------------------------------

// x^y = e^(y * log x) for positive x, as reference mantissa * 2^exponent.
wide::ScaledWide pow_reference(const double x, const double y) {
    const wide::Wide log_x = wide::log_reference(x);
    const wide::ScaledWide result = wide::exp_reference(wide::multiply(log_x, y));
    return result;
}

// A recorded special case of a two-argument function: indices into the
// check's input arrays and the expected result's bits.
struct BinarySpecial final {
    std::size_t first_index{0};
    std::size_t second_index{0};
    std::uint64_t expected_bits{0};
};

struct BinarySpecialFloat final {
    std::size_t first_index{0};
    std::size_t second_index{0};
    std::uint32_t expected_bits{0};
};

// Annex F: every combination of special x and y gives its recorded result
// (exact zeros, infinities, ones and NaN, so rounding plays no part). The
// results are recorded rather than taken from the host C library, so the
// check is the same on every host; any NaN matches a recorded NaN.
void pow_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double xs[] = {0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, -2.0, infinity, -infinity, nan,
                         0x1p-1074, -0x1p-1074, 0x1.fffffffffffffp1023, -3.0};
    const double ys[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 0.5, -0.5, infinity,
                         -infinity, nan, 0x1p53, -0x1p53, 0x1.0000000000001p52, 1.5};
    // {xs index, ys index, result bits}
    const BinarySpecial recorded[] = {
        {0, 0, 0x3ff0000000000000ULL}, {0, 1, 0x3ff0000000000000ULL},
        {0, 2, 0x0000000000000000ULL}, {0, 3, 0x7ff0000000000000ULL},
        {0, 4, 0x0000000000000000ULL}, {0, 5, 0x7ff0000000000000ULL},
        {0, 6, 0x0000000000000000ULL}, {0, 7, 0x7ff0000000000000ULL},
        {0, 8, 0x0000000000000000ULL}, {0, 9, 0x7ff0000000000000ULL},
        {0, 10, 0x0000000000000000ULL}, {0, 11, 0x7ff0000000000000ULL},
        {0, 12, 0x7ff8000000000000ULL}, {0, 13, 0x0000000000000000ULL},
        {0, 14, 0x7ff0000000000000ULL}, {0, 15, 0x0000000000000000ULL},
        {0, 16, 0x0000000000000000ULL}, {1, 0, 0x3ff0000000000000ULL},
        {1, 1, 0x3ff0000000000000ULL}, {1, 2, 0x8000000000000000ULL},
        {1, 3, 0xfff0000000000000ULL}, {1, 4, 0x0000000000000000ULL},
        {1, 5, 0x7ff0000000000000ULL}, {1, 6, 0x8000000000000000ULL},
        {1, 7, 0xfff0000000000000ULL}, {1, 8, 0x0000000000000000ULL},
        {1, 9, 0x7ff0000000000000ULL}, {1, 10, 0x0000000000000000ULL},
        {1, 11, 0x7ff0000000000000ULL}, {1, 12, 0x7ff8000000000000ULL},
        {1, 13, 0x0000000000000000ULL}, {1, 14, 0x7ff0000000000000ULL},
        {1, 15, 0x8000000000000000ULL}, {1, 16, 0x0000000000000000ULL},
        {2, 0, 0x3ff0000000000000ULL}, {2, 1, 0x3ff0000000000000ULL},
        {2, 2, 0x3ff0000000000000ULL}, {2, 3, 0x3ff0000000000000ULL},
        {2, 4, 0x3ff0000000000000ULL}, {2, 5, 0x3ff0000000000000ULL},
        {2, 6, 0x3ff0000000000000ULL}, {2, 7, 0x3ff0000000000000ULL},
        {2, 8, 0x3ff0000000000000ULL}, {2, 9, 0x3ff0000000000000ULL},
        {2, 10, 0x3ff0000000000000ULL}, {2, 11, 0x3ff0000000000000ULL},
        {2, 12, 0x3ff0000000000000ULL}, {2, 13, 0x3ff0000000000000ULL},
        {2, 14, 0x3ff0000000000000ULL}, {2, 15, 0x3ff0000000000000ULL},
        {2, 16, 0x3ff0000000000000ULL}, {3, 0, 0x3ff0000000000000ULL},
        {3, 1, 0x3ff0000000000000ULL}, {3, 2, 0xbff0000000000000ULL},
        {3, 3, 0xbff0000000000000ULL}, {3, 4, 0x3ff0000000000000ULL},
        {3, 5, 0x3ff0000000000000ULL}, {3, 6, 0xbff0000000000000ULL},
        {3, 7, 0xbff0000000000000ULL}, {3, 8, 0x7ff8000000000000ULL},
        {3, 9, 0x7ff8000000000000ULL}, {3, 10, 0x3ff0000000000000ULL},
        {3, 11, 0x3ff0000000000000ULL}, {3, 12, 0x7ff8000000000000ULL},
        {3, 13, 0x3ff0000000000000ULL}, {3, 14, 0x3ff0000000000000ULL},
        {3, 15, 0xbff0000000000000ULL}, {3, 16, 0x7ff8000000000000ULL},
        {4, 0, 0x3ff0000000000000ULL}, {4, 1, 0x3ff0000000000000ULL},
        {4, 10, 0x0000000000000000ULL}, {4, 11, 0x7ff0000000000000ULL},
        {4, 12, 0x7ff8000000000000ULL}, {4, 13, 0x0000000000000000ULL},
        {4, 14, 0x7ff0000000000000ULL}, {4, 15, 0x0000000000000000ULL},
        {5, 0, 0x3ff0000000000000ULL}, {5, 1, 0x3ff0000000000000ULL},
        {5, 8, 0x7ff8000000000000ULL}, {5, 9, 0x7ff8000000000000ULL},
        {5, 10, 0x0000000000000000ULL}, {5, 11, 0x7ff0000000000000ULL},
        {5, 12, 0x7ff8000000000000ULL}, {5, 13, 0x0000000000000000ULL},
        {5, 14, 0x7ff0000000000000ULL}, {5, 15, 0x8000000000000000ULL},
        {5, 16, 0x7ff8000000000000ULL}, {6, 0, 0x3ff0000000000000ULL},
        {6, 1, 0x3ff0000000000000ULL}, {6, 10, 0x7ff0000000000000ULL},
        {6, 11, 0x0000000000000000ULL}, {6, 12, 0x7ff8000000000000ULL},
        {6, 13, 0x7ff0000000000000ULL}, {6, 14, 0x0000000000000000ULL},
        {6, 15, 0x7ff0000000000000ULL}, {7, 0, 0x3ff0000000000000ULL},
        {7, 1, 0x3ff0000000000000ULL}, {7, 8, 0x7ff8000000000000ULL},
        {7, 9, 0x7ff8000000000000ULL}, {7, 10, 0x7ff0000000000000ULL},
        {7, 11, 0x0000000000000000ULL}, {7, 12, 0x7ff8000000000000ULL},
        {7, 13, 0x7ff0000000000000ULL}, {7, 14, 0x0000000000000000ULL},
        {7, 15, 0xfff0000000000000ULL}, {7, 16, 0x7ff8000000000000ULL},
        {8, 0, 0x3ff0000000000000ULL}, {8, 1, 0x3ff0000000000000ULL},
        {8, 2, 0x7ff0000000000000ULL}, {8, 3, 0x0000000000000000ULL},
        {8, 4, 0x7ff0000000000000ULL}, {8, 5, 0x0000000000000000ULL},
        {8, 6, 0x7ff0000000000000ULL}, {8, 7, 0x0000000000000000ULL},
        {8, 8, 0x7ff0000000000000ULL}, {8, 9, 0x0000000000000000ULL},
        {8, 10, 0x7ff0000000000000ULL}, {8, 11, 0x0000000000000000ULL},
        {8, 12, 0x7ff8000000000000ULL}, {8, 13, 0x7ff0000000000000ULL},
        {8, 14, 0x0000000000000000ULL}, {8, 15, 0x7ff0000000000000ULL},
        {8, 16, 0x7ff0000000000000ULL}, {9, 0, 0x3ff0000000000000ULL},
        {9, 1, 0x3ff0000000000000ULL}, {9, 2, 0xfff0000000000000ULL},
        {9, 3, 0x8000000000000000ULL}, {9, 4, 0x7ff0000000000000ULL},
        {9, 5, 0x0000000000000000ULL}, {9, 6, 0xfff0000000000000ULL},
        {9, 7, 0x8000000000000000ULL}, {9, 8, 0x7ff0000000000000ULL},
        {9, 9, 0x0000000000000000ULL}, {9, 10, 0x7ff0000000000000ULL},
        {9, 11, 0x0000000000000000ULL}, {9, 12, 0x7ff8000000000000ULL},
        {9, 13, 0x7ff0000000000000ULL}, {9, 14, 0x0000000000000000ULL},
        {9, 15, 0xfff0000000000000ULL}, {9, 16, 0x7ff0000000000000ULL},
        {10, 0, 0x3ff0000000000000ULL}, {10, 1, 0x3ff0000000000000ULL},
        {10, 2, 0x7ff8000000000000ULL}, {10, 3, 0x7ff8000000000000ULL},
        {10, 4, 0x7ff8000000000000ULL}, {10, 5, 0x7ff8000000000000ULL},
        {10, 6, 0x7ff8000000000000ULL}, {10, 7, 0x7ff8000000000000ULL},
        {10, 8, 0x7ff8000000000000ULL}, {10, 9, 0x7ff8000000000000ULL},
        {10, 10, 0x7ff8000000000000ULL}, {10, 11, 0x7ff8000000000000ULL},
        {10, 12, 0x7ff8000000000000ULL}, {10, 13, 0x7ff8000000000000ULL},
        {10, 14, 0x7ff8000000000000ULL}, {10, 15, 0x7ff8000000000000ULL},
        {10, 16, 0x7ff8000000000000ULL}, {11, 0, 0x3ff0000000000000ULL},
        {11, 1, 0x3ff0000000000000ULL}, {11, 3, 0x7ff0000000000000ULL},
        {11, 4, 0x0000000000000000ULL}, {11, 5, 0x7ff0000000000000ULL},
        {11, 6, 0x0000000000000000ULL}, {11, 7, 0x7ff0000000000000ULL},
        {11, 10, 0x0000000000000000ULL}, {11, 11, 0x7ff0000000000000ULL},
        {11, 12, 0x7ff8000000000000ULL}, {11, 13, 0x0000000000000000ULL},
        {11, 14, 0x7ff0000000000000ULL}, {11, 15, 0x0000000000000000ULL},
        {11, 16, 0x0000000000000000ULL}, {12, 0, 0x3ff0000000000000ULL},
        {12, 1, 0x3ff0000000000000ULL}, {12, 3, 0xfff0000000000000ULL},
        {12, 4, 0x0000000000000000ULL}, {12, 5, 0x7ff0000000000000ULL},
        {12, 6, 0x8000000000000000ULL}, {12, 7, 0xfff0000000000000ULL},
        {12, 8, 0x7ff8000000000000ULL}, {12, 9, 0x7ff8000000000000ULL},
        {12, 10, 0x0000000000000000ULL}, {12, 11, 0x7ff0000000000000ULL},
        {12, 12, 0x7ff8000000000000ULL}, {12, 13, 0x0000000000000000ULL},
        {12, 14, 0x7ff0000000000000ULL}, {12, 15, 0x8000000000000000ULL},
        {12, 16, 0x7ff8000000000000ULL}, {13, 0, 0x3ff0000000000000ULL},
        {13, 1, 0x3ff0000000000000ULL}, {13, 4, 0x7ff0000000000000ULL},
        {13, 5, 0x0000000000000000ULL}, {13, 6, 0x7ff0000000000000ULL},
        {13, 7, 0x0000000000000000ULL}, {13, 10, 0x7ff0000000000000ULL},
        {13, 11, 0x0000000000000000ULL}, {13, 12, 0x7ff8000000000000ULL},
        {13, 13, 0x7ff0000000000000ULL}, {13, 14, 0x0000000000000000ULL},
        {13, 15, 0x7ff0000000000000ULL}, {13, 16, 0x7ff0000000000000ULL},
        {14, 0, 0x3ff0000000000000ULL}, {14, 1, 0x3ff0000000000000ULL},
        {14, 8, 0x7ff8000000000000ULL}, {14, 9, 0x7ff8000000000000ULL},
        {14, 10, 0x7ff0000000000000ULL}, {14, 11, 0x0000000000000000ULL},
        {14, 12, 0x7ff8000000000000ULL}, {14, 13, 0x7ff0000000000000ULL},
        {14, 14, 0x0000000000000000ULL}, {14, 15, 0xfff0000000000000ULL},
        {14, 16, 0x7ff8000000000000ULL},
    };
    for (const BinarySpecial& special : recorded) {
        const double x = xs[special.first_index];
        const double y = ys[special.second_index];
        require(same_bits(unflushed::pow(x, y), std::bit_cast<double>(special.expected_bits)),
                "pow(double): special input differs from its recorded result");
    }
    // The same inputs rounded to float.
    const BinarySpecialFloat recorded_float[] = {
        {0, 0, 0x3f800000U}, {0, 1, 0x3f800000U}, {0, 2, 0x00000000U},
        {0, 3, 0x7f800000U}, {0, 4, 0x00000000U}, {0, 5, 0x7f800000U},
        {0, 6, 0x00000000U}, {0, 7, 0x7f800000U}, {0, 8, 0x00000000U},
        {0, 9, 0x7f800000U}, {0, 10, 0x00000000U}, {0, 11, 0x7f800000U},
        {0, 12, 0x7fc00000U}, {0, 13, 0x00000000U}, {0, 14, 0x7f800000U},
        {0, 15, 0x00000000U}, {0, 16, 0x00000000U}, {1, 0, 0x3f800000U},
        {1, 1, 0x3f800000U}, {1, 2, 0x80000000U}, {1, 3, 0xff800000U},
        {1, 4, 0x00000000U}, {1, 5, 0x7f800000U}, {1, 6, 0x80000000U},
        {1, 7, 0xff800000U}, {1, 8, 0x00000000U}, {1, 9, 0x7f800000U},
        {1, 10, 0x00000000U}, {1, 11, 0x7f800000U}, {1, 12, 0x7fc00000U},
        {1, 13, 0x00000000U}, {1, 14, 0x7f800000U}, {1, 15, 0x00000000U},
        {1, 16, 0x00000000U}, {2, 0, 0x3f800000U}, {2, 1, 0x3f800000U},
        {2, 2, 0x3f800000U}, {2, 3, 0x3f800000U}, {2, 4, 0x3f800000U},
        {2, 5, 0x3f800000U}, {2, 6, 0x3f800000U}, {2, 7, 0x3f800000U},
        {2, 8, 0x3f800000U}, {2, 9, 0x3f800000U}, {2, 10, 0x3f800000U},
        {2, 11, 0x3f800000U}, {2, 12, 0x3f800000U}, {2, 13, 0x3f800000U},
        {2, 14, 0x3f800000U}, {2, 15, 0x3f800000U}, {2, 16, 0x3f800000U},
        {3, 0, 0x3f800000U}, {3, 1, 0x3f800000U}, {3, 2, 0xbf800000U},
        {3, 3, 0xbf800000U}, {3, 4, 0x3f800000U}, {3, 5, 0x3f800000U},
        {3, 6, 0xbf800000U}, {3, 7, 0xbf800000U}, {3, 8, 0x7fc00000U},
        {3, 9, 0x7fc00000U}, {3, 10, 0x3f800000U}, {3, 11, 0x3f800000U},
        {3, 12, 0x7fc00000U}, {3, 13, 0x3f800000U}, {3, 14, 0x3f800000U},
        {3, 15, 0x3f800000U}, {3, 16, 0x7fc00000U}, {4, 0, 0x3f800000U},
        {4, 1, 0x3f800000U}, {4, 10, 0x00000000U}, {4, 11, 0x7f800000U},
        {4, 12, 0x7fc00000U}, {4, 13, 0x00000000U}, {4, 14, 0x7f800000U},
        {4, 15, 0x00000000U}, {5, 0, 0x3f800000U}, {5, 1, 0x3f800000U},
        {5, 8, 0x7fc00000U}, {5, 9, 0x7fc00000U}, {5, 10, 0x00000000U},
        {5, 11, 0x7f800000U}, {5, 12, 0x7fc00000U}, {5, 13, 0x00000000U},
        {5, 14, 0x7f800000U}, {5, 15, 0x00000000U}, {5, 16, 0x7fc00000U},
        {6, 0, 0x3f800000U}, {6, 1, 0x3f800000U}, {6, 10, 0x7f800000U},
        {6, 11, 0x00000000U}, {6, 12, 0x7fc00000U}, {6, 13, 0x7f800000U},
        {6, 14, 0x00000000U}, {6, 15, 0x7f800000U}, {7, 0, 0x3f800000U},
        {7, 1, 0x3f800000U}, {7, 8, 0x7fc00000U}, {7, 9, 0x7fc00000U},
        {7, 10, 0x7f800000U}, {7, 11, 0x00000000U}, {7, 12, 0x7fc00000U},
        {7, 13, 0x7f800000U}, {7, 14, 0x00000000U}, {7, 15, 0x7f800000U},
        {7, 16, 0x7fc00000U}, {8, 0, 0x3f800000U}, {8, 1, 0x3f800000U},
        {8, 2, 0x7f800000U}, {8, 3, 0x00000000U}, {8, 4, 0x7f800000U},
        {8, 5, 0x00000000U}, {8, 6, 0x7f800000U}, {8, 7, 0x00000000U},
        {8, 8, 0x7f800000U}, {8, 9, 0x00000000U}, {8, 10, 0x7f800000U},
        {8, 11, 0x00000000U}, {8, 12, 0x7fc00000U}, {8, 13, 0x7f800000U},
        {8, 14, 0x00000000U}, {8, 15, 0x7f800000U}, {8, 16, 0x7f800000U},
        {9, 0, 0x3f800000U}, {9, 1, 0x3f800000U}, {9, 2, 0xff800000U},
        {9, 3, 0x80000000U}, {9, 4, 0x7f800000U}, {9, 5, 0x00000000U},
        {9, 6, 0xff800000U}, {9, 7, 0x80000000U}, {9, 8, 0x7f800000U},
        {9, 9, 0x00000000U}, {9, 10, 0x7f800000U}, {9, 11, 0x00000000U},
        {9, 12, 0x7fc00000U}, {9, 13, 0x7f800000U}, {9, 14, 0x00000000U},
        {9, 15, 0x7f800000U}, {9, 16, 0x7f800000U}, {10, 0, 0x3f800000U},
        {10, 1, 0x3f800000U}, {10, 2, 0x7fc00000U}, {10, 3, 0x7fc00000U},
        {10, 4, 0x7fc00000U}, {10, 5, 0x7fc00000U}, {10, 6, 0x7fc00000U},
        {10, 7, 0x7fc00000U}, {10, 8, 0x7fc00000U}, {10, 9, 0x7fc00000U},
        {10, 10, 0x7fc00000U}, {10, 11, 0x7fc00000U}, {10, 12, 0x7fc00000U},
        {10, 13, 0x7fc00000U}, {10, 14, 0x7fc00000U}, {10, 15, 0x7fc00000U},
        {10, 16, 0x7fc00000U}, {11, 0, 0x3f800000U}, {11, 1, 0x3f800000U},
        {11, 3, 0x7f800000U}, {11, 4, 0x00000000U}, {11, 5, 0x7f800000U},
        {11, 6, 0x00000000U}, {11, 7, 0x7f800000U}, {11, 10, 0x00000000U},
        {11, 11, 0x7f800000U}, {11, 12, 0x7fc00000U}, {11, 13, 0x00000000U},
        {11, 14, 0x7f800000U}, {11, 15, 0x00000000U}, {11, 16, 0x00000000U},
        {12, 0, 0x3f800000U}, {12, 1, 0x3f800000U}, {12, 3, 0xff800000U},
        {12, 4, 0x00000000U}, {12, 5, 0x7f800000U}, {12, 6, 0x80000000U},
        {12, 7, 0xff800000U}, {12, 8, 0x00000000U}, {12, 9, 0x7f800000U},
        {12, 10, 0x00000000U}, {12, 11, 0x7f800000U}, {12, 12, 0x7fc00000U},
        {12, 13, 0x00000000U}, {12, 14, 0x7f800000U}, {12, 15, 0x00000000U},
        {12, 16, 0x00000000U}, {13, 0, 0x3f800000U}, {13, 1, 0x3f800000U},
        {13, 4, 0x7f800000U}, {13, 5, 0x00000000U}, {13, 6, 0x7f800000U},
        {13, 7, 0x00000000U}, {13, 10, 0x7f800000U}, {13, 11, 0x00000000U},
        {13, 12, 0x7fc00000U}, {13, 13, 0x7f800000U}, {13, 14, 0x00000000U},
        {13, 15, 0x7f800000U}, {13, 16, 0x7f800000U}, {14, 0, 0x3f800000U},
        {14, 1, 0x3f800000U}, {14, 8, 0x7fc00000U}, {14, 9, 0x7fc00000U},
        {14, 10, 0x7f800000U}, {14, 11, 0x00000000U}, {14, 12, 0x7fc00000U},
        {14, 13, 0x7f800000U}, {14, 14, 0x00000000U}, {14, 15, 0x7f800000U},
        {14, 16, 0x7fc00000U},
    };
    for (const BinarySpecialFloat& special : recorded_float) {
        const float xf = static_cast<float>(xs[special.first_index]);
        const float yf = static_cast<float>(ys[special.second_index]);
        require(same_bits(unflushed::pow(xf, yf), std::bit_cast<float>(special.expected_bits)),
                "pow(float): special input differs from its recorded result");
    }
    // Exactly representable results are exact.
    for (int n = -1074; n <= 1023; ++n) {
        require(unflushed::pow(2.0, static_cast<double>(n)) == std::ldexp(1.0, n),
                "pow(double): 2^n is not exact");
    }
    double power = 1.0;
    for (int n = 0; n <= 22; ++n) {
        require(unflushed::pow(10.0, static_cast<double>(n)) == power,
                "pow(double): 10^n is not exact");
        power *= 10.0;
    }
    for (int a = 1; a <= 100; ++a) {
        double exact = 1.0;
        for (int b = 0; b <= 12 && exact < 0x1p53; ++b) {
            require(unflushed::pow(static_cast<double>(a), static_cast<double>(b)) == exact &&
                        unflushed::pow(-static_cast<double>(a), static_cast<double>(b)) ==
                            (b % 2 == 1 ? -exact : exact),
                    "pow(double): small integer power is not exact");
            exact *= static_cast<double>(a);
        }
    }
    require(unflushed::pow(4.0, 0.5) == 2.0 && unflushed::pow(0.25, -0.5) == 2.0,
            "pow(double): exact square roots are not exact");
}

// The kernel's y range (2^-80 <= |y| < 2^64, pow.cpp): outside it the result
// is decided without the kernel, the same in the fused and Dekker builds
// (this test runs in both). Before 2026-11-13 pow(1, +-DBL_MAX) was NaN in
// the Dekker build (the split of y overflowed) and 1 in the fused one.
void pow_kernel_y_range() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double largest = std::numeric_limits<double>::max();
    const double below_one = 0x1.fffffffffffffp-1;  // 1 - 2^-53
    const double above_one = 0x1.0000000000001p0;   // 1 + 2^-52
    const double huge_ys[] = {0x1p64, 0x1.0000000000001p64, 0x1p996, 0x1p997, 0x1p1000, largest, infinity};
    for (const double huge : huge_ys) {
        for (const double sign : {1.0, -1.0}) {
            const double y = sign * huge;
            require(same_bits(unflushed::pow(1.0, y), 1.0), "pow(double): 1^y is not 1 for huge y");
            require(same_bits(unflushed::pow(-1.0, y), 1.0), "pow(double): (-1)^y is not 1 for huge (even) y");
            const double grows = sign > 0.0 ? infinity : 0.0;
            const double shrinks = sign > 0.0 ? 0.0 : infinity;
            require(same_bits(unflushed::pow(above_one, y), grows), "pow(double): (1 + 2^-52)^y for huge y");
            require(same_bits(unflushed::pow(below_one, y), shrinks), "pow(double): (1 - 2^-53)^y for huge y");
            require(same_bits(unflushed::pow(-2.0, y), grows), "pow(double): (-2)^y for huge (even) y");
            require(same_bits(unflushed::pow(-0.5, y), shrinks), "pow(double): (-0.5)^y for huge (even) y");
            require(same_bits(unflushed::pow(0x1p-1074, y), shrinks), "pow(double): subnormal^y for huge y");
            require(same_bits(unflushed::pow(largest, y), grows), "pow(double): DBL_MAX^y for huge y");
        }
    }
    // Just inside the top of the kernel's range: the kernel's own limits.
    const double top = 0x1.fffffffffffffp63;
    require(same_bits(unflushed::pow(above_one, top), infinity), "pow(double): (1 + 2^-52)^(2^64 - ulp)");
    require(same_bits(unflushed::pow(below_one, top), 0.0), "pow(double): (1 - 2^-53)^(2^64 - ulp)");
    require(same_bits(unflushed::pow(1.0, top), 1.0), "pow(double): 1^(2^64 - ulp)");
    // Below the kernel's range x^y rounds to 1; negative x has no real power.
    const double tiny_ys[] = {0x1.fffffffffffffp-81, 0x1p-100, 1e-300, 0x1p-1022, 0x1p-1074};
    const double bases[] = {0x1p-1074, 0x1p-1022, 0.5, below_one, above_one, 2.0, 1e300, largest};
    for (const double tiny : tiny_ys) {
        for (const double sign : {1.0, -1.0}) {
            const double y = sign * tiny;
            for (const double x : bases) {
                require(same_bits(unflushed::pow(x, y), 1.0), "pow(double): x^y is not 1 for tiny y");
                require(std::isnan(unflushed::pow(-x, y)), "pow(double): negative x with tiny y is not NaN");
            }
        }
    }
    // The bottom of the kernel's range: |y log2 x| <= 2^-70, still 1.
    for (const double x : bases) {
        require(same_bits(unflushed::pow(x, 0x1p-80), 1.0), "pow(double): x^(2^-80) is not 1");
        require(same_bits(unflushed::pow(x, -0x1p-80), 1.0), "pow(double): x^(-2^-80) is not 1");
    }
    std::printf("pow(double) outside the kernel's y range: decided, in both product forms\n");
}

// Inputs: x in [0.5, 2] with y in [-100, 100]; any exponent of x with
// |y log x| < 700; x within 1e-10 of 1 with |y| ~ 1e10; results near
// overflow; and subnormal results.
void pow_input(const int region, Lcg& lcg, double& x, double& y) {
    if (region == 0) {
        x = 0.5 + 1.5 * lcg.unit();
        y = (lcg.unit() - 0.5) * 200.0;
    } else if (region == 1) {
        const double exponent = std::floor(lcg.unit() * 2000.0) - 1000.0;
        x = std::ldexp(1.0 + lcg.unit(), static_cast<int>(exponent));
        y = (lcg.unit() - 0.5) * 1400.0 / std::fabs(unflushed::log(x));
    } else if (region == 2) {
        x = 1.0 + (lcg.unit() - 0.5) * 2e-10;
        y = (lcg.unit() - 0.5) * 2e10;
    } else if (region == 3) {
        x = 1.0 + lcg.unit();
        y = 709.0 / unflushed::log(x) * (0.999 + 0.0012 * lcg.unit());
    } else {
        x = 1.0 + lcg.unit();
        y = -1.0 * (708.5 + 36.5 * lcg.unit()) / unflushed::log(x);
    }
}

const char* const pow_region_names[] = {"x in [0.5, 2]", "any x", "x = 1 +- 1e-10",
                                        "near overflow", "subnormal"};

void pow_double_accuracy(const int per_region) {
    Lcg lcg{2030};
    for (int region = 0; region < 5; ++region) {
        double worst = 0.0;
        double worst_x = 0.0;
        double worst_y = 0.0;
        std::uint64_t misrounded = 0;
        int measured = 0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region, lcg, x, y);
            const double result = unflushed::pow(x, y);
            const wide::ScaledWide exact = pow_reference(x, y);
            double error = 0.0;
            if (region == 4) {
                // STXlib: subnormal results flush to +0 (math.hpp), verified
                // exactly instead of measured; x > 0, so the zero is +0.
                const double public_result = stxlib::math::pow(x, y);
                require(same_bits(public_result, flushed(result)), "pow(double): not the flushed kernel result");
                if (result >= 0x1p-1022) continue;
                const double nearest = exp_correctly_rounded_subnormal(exact);
                if (nearest < 0x1p-1022 - 0x1p-1074) {
                    require(same_bits(public_result, 0.0), "pow(double): a subnormal result was not flushed to +0");
                }
                require(same_bits(public_result, 0.0) || public_result >= 0x1p-1022,
                        "pow(double): a subnormal result near 2^-1022");
                misrounded += 1;
                measured += 1;
                continue;
            } else {
                if (!std::isfinite(result) || result < 0x1p-1022) continue;
                error = exp_ulps(result, exact);
                if (error > 0.5) misrounded += 1;
            }
            measured += 1;
            if (error > worst) {
                worst = error;
                worst_x = x;
                worst_y = y;
            }
        }
        std::printf("pow(double) %-15s %10d inputs  max %.6f ulp at (%a, %a)  %llu %s\n",
                    pow_region_names[region], measured, worst, worst_x, worst_y,
                    static_cast<unsigned long long>(misrounded),
                    region == 4 ? "flushed (verified, not measured)" : "above 0.5 ulp");
        require(worst <= pow_ulp_limit, "pow(double): error above the stated bound");
    }
}

// Random float pairs, with results across the float range: the result must
// be the correctly rounded x^y.
void pow_float_rounding(const int count) {
    Lcg lcg{2031};
    int checked = 0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(lcg.unit() * 16.0);
        const float y = static_cast<float>((lcg.unit() - 0.5) * 80.0);
        const float result = unflushed::pow(x, y);
        if (!(x > 0.0f) || !std::isfinite(result) || result < 0x1p-126f) continue;
        const wide::ScaledWide exact = pow_reference(static_cast<double>(x), static_cast<double>(y));
        require(result == exp_correctly_rounded_float(exact),
                "pow(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("pow(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t pow_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{96};
    for (int region = 0; region < 5; ++region) {
        for (int i = 0; i < 400000; ++i) {
            double x = 0.0;
            double y = 0.0;
            pow_input(region, lcg, x, y);
            const double result = unflushed::pow(x, y);
            const double negative = unflushed::pow(-x, std::floor(y));
            const float result_float = unflushed::pow(static_cast<float>(x), static_cast<float>(y));
            const std::uint64_t words[3] = {std::bit_cast<std::uint64_t>(result),
                                            std::bit_cast<std::uint64_t>(negative),
                                            std::bit_cast<std::uint32_t>(result_float)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-11-02 (Apple arm64, fma and Dekker builds, -O0 and -O2;
// x86-64 MinGW, reported by the Windows side). pow_input scales its
// exponents by the owned log, so the inputs no longer depend on the host's
// std::log (the 2026-10-31 value did, and differed on Windows).
// Replaced on 2026-11-02 (was 0xc5ae8756613caabf): log2 formulation
// (owner's experiment, adopted 2026-11-02); 6/400k near-overflow inputs
// differ, both within bound. Same value at the scalar, vector128 and
// avx2_fma levels and in the Dekker build (x86-64 MinGW).
constexpr std::uint64_t today_pow_output_hash = 0x3a845b9bad3d2534ULL;
// The hash mixes pow(double) and pow(float) outputs, and each has its own
// switch, so each of the four switch settings has its own value (R3: one
// region constant had served all three region settings). Frozen: both on the
// region path, 2026-11-15 (sbi, main f5cbc30, the all-ON build; it differs
// from today's: region pow(double) is 0.500097 ulp against 0.576). Not
// frozen yet (0, printed by check_frozen): only one of the two on the region
// path; the R3 run list builds each to freeze it.
constexpr std::uint64_t region_pow_double_only_output_hash = 0;
constexpr std::uint64_t region_pow_float_only_output_hash = 0;
constexpr std::uint64_t region_pow_output_hash = 0x9bc52c7be16de26fULL;

constexpr std::uint64_t pow_expected_hash(const bool double_on_region, const bool float_on_region) {
    std::uint64_t result = today_pow_output_hash;
    if (double_on_region && float_on_region) {
        result = region_pow_output_hash;
    } else if (double_on_region) {
        result = region_pow_double_only_output_hash;
    } else if (float_on_region) {
        result = region_pow_float_only_output_hash;
    }
    return result;
}

constexpr std::uint64_t expected_pow_output_hash =
    pow_expected_hash(STXLIB_MATH_REGION_POW_DOUBLE != 0, STXLIB_MATH_REGION_POW_FLOAT != 0);

void pow_determinism() {
    const std::uint64_t hash = pow_output_hash();
    check_frozen("pow", hash, expected_pow_output_hash, today_pow_output_hash,
                 "pow: outputs differ from the frozen bits");
}

// ---- hypot ------------------------------------------------------------------

// Annex F and exact cases, against recorded results (any NaN matches a
// recorded NaN).
void hypot_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double values[] = {0.0, -0.0, 1.0, -3.0, infinity, -infinity, nan, 0x1p-1074,
                             0x1.fffffffffffffp1023};
    // {x index, y index, result bits}, both indices into values
    const BinarySpecial recorded[] = {
        {0, 0, 0x0000000000000000ULL}, {0, 1, 0x0000000000000000ULL},
        {0, 2, 0x3ff0000000000000ULL}, {0, 3, 0x4008000000000000ULL},
        {0, 4, 0x7ff0000000000000ULL}, {0, 5, 0x7ff0000000000000ULL},
        {0, 6, 0x7ff8000000000000ULL}, {0, 7, 0x0000000000000001ULL},
        {0, 8, 0x7fefffffffffffffULL}, {1, 0, 0x0000000000000000ULL},
        {1, 1, 0x0000000000000000ULL}, {1, 2, 0x3ff0000000000000ULL},
        {1, 3, 0x4008000000000000ULL}, {1, 4, 0x7ff0000000000000ULL},
        {1, 5, 0x7ff0000000000000ULL}, {1, 6, 0x7ff8000000000000ULL},
        {1, 7, 0x0000000000000001ULL}, {1, 8, 0x7fefffffffffffffULL},
        {2, 0, 0x3ff0000000000000ULL}, {2, 1, 0x3ff0000000000000ULL},
        {2, 4, 0x7ff0000000000000ULL}, {2, 5, 0x7ff0000000000000ULL},
        {2, 6, 0x7ff8000000000000ULL}, {3, 0, 0x4008000000000000ULL},
        {3, 1, 0x4008000000000000ULL}, {3, 4, 0x7ff0000000000000ULL},
        {3, 5, 0x7ff0000000000000ULL}, {3, 6, 0x7ff8000000000000ULL},
        {4, 0, 0x7ff0000000000000ULL}, {4, 1, 0x7ff0000000000000ULL},
        {4, 2, 0x7ff0000000000000ULL}, {4, 3, 0x7ff0000000000000ULL},
        {4, 4, 0x7ff0000000000000ULL}, {4, 5, 0x7ff0000000000000ULL},
        {4, 6, 0x7ff0000000000000ULL}, {4, 7, 0x7ff0000000000000ULL},
        {4, 8, 0x7ff0000000000000ULL}, {5, 0, 0x7ff0000000000000ULL},
        {5, 1, 0x7ff0000000000000ULL}, {5, 2, 0x7ff0000000000000ULL},
        {5, 3, 0x7ff0000000000000ULL}, {5, 4, 0x7ff0000000000000ULL},
        {5, 5, 0x7ff0000000000000ULL}, {5, 6, 0x7ff0000000000000ULL},
        {5, 7, 0x7ff0000000000000ULL}, {5, 8, 0x7ff0000000000000ULL},
        {6, 0, 0x7ff8000000000000ULL}, {6, 1, 0x7ff8000000000000ULL},
        {6, 2, 0x7ff8000000000000ULL}, {6, 3, 0x7ff8000000000000ULL},
        {6, 4, 0x7ff0000000000000ULL}, {6, 5, 0x7ff0000000000000ULL},
        {6, 6, 0x7ff8000000000000ULL}, {6, 7, 0x7ff8000000000000ULL},
        {6, 8, 0x7ff8000000000000ULL}, {7, 0, 0x0000000000000001ULL},
        {7, 1, 0x0000000000000001ULL}, {7, 4, 0x7ff0000000000000ULL},
        {7, 5, 0x7ff0000000000000ULL}, {7, 6, 0x7ff8000000000000ULL},
        {8, 0, 0x7fefffffffffffffULL}, {8, 1, 0x7fefffffffffffffULL},
        {8, 4, 0x7ff0000000000000ULL}, {8, 5, 0x7ff0000000000000ULL},
        {8, 6, 0x7ff8000000000000ULL}, {8, 8, 0x7ff0000000000000ULL},
    };
    for (const BinarySpecial& special : recorded) {
        const double x = values[special.first_index];
        const double y = values[special.second_index];
        require(same_bits(unflushed::hypot(x, y), std::bit_cast<double>(special.expected_bits)),
                "hypot(double): special input differs from its recorded result");
    }
    const BinarySpecialFloat recorded_float[] = {
        {0, 0, 0x00000000U}, {0, 1, 0x00000000U}, {0, 2, 0x3f800000U},
        {0, 3, 0x40400000U}, {0, 4, 0x7f800000U}, {0, 5, 0x7f800000U},
        {0, 6, 0x7fc00000U}, {0, 7, 0x00000000U}, {0, 8, 0x7f800000U},
        {1, 0, 0x00000000U}, {1, 1, 0x00000000U}, {1, 2, 0x3f800000U},
        {1, 3, 0x40400000U}, {1, 4, 0x7f800000U}, {1, 5, 0x7f800000U},
        {1, 6, 0x7fc00000U}, {1, 7, 0x00000000U}, {1, 8, 0x7f800000U},
        {2, 0, 0x3f800000U}, {2, 1, 0x3f800000U}, {2, 4, 0x7f800000U},
        {2, 5, 0x7f800000U}, {2, 6, 0x7fc00000U}, {3, 0, 0x40400000U},
        {3, 1, 0x40400000U}, {3, 4, 0x7f800000U}, {3, 5, 0x7f800000U},
        {3, 6, 0x7fc00000U}, {4, 0, 0x7f800000U}, {4, 1, 0x7f800000U},
        {4, 2, 0x7f800000U}, {4, 3, 0x7f800000U}, {4, 4, 0x7f800000U},
        {4, 5, 0x7f800000U}, {4, 6, 0x7f800000U}, {4, 7, 0x7f800000U},
        {4, 8, 0x7f800000U}, {5, 0, 0x7f800000U}, {5, 1, 0x7f800000U},
        {5, 2, 0x7f800000U}, {5, 3, 0x7f800000U}, {5, 4, 0x7f800000U},
        {5, 5, 0x7f800000U}, {5, 6, 0x7f800000U}, {5, 7, 0x7f800000U},
        {5, 8, 0x7f800000U}, {6, 0, 0x7fc00000U}, {6, 1, 0x7fc00000U},
        {6, 2, 0x7fc00000U}, {6, 3, 0x7fc00000U}, {6, 4, 0x7f800000U},
        {6, 5, 0x7f800000U}, {6, 6, 0x7fc00000U}, {6, 7, 0x7fc00000U},
        {6, 8, 0x7f800000U}, {7, 0, 0x00000000U}, {7, 1, 0x00000000U},
        {7, 4, 0x7f800000U}, {7, 5, 0x7f800000U}, {7, 6, 0x7fc00000U},
        {8, 0, 0x7f800000U}, {8, 1, 0x7f800000U}, {8, 4, 0x7f800000U},
        {8, 5, 0x7f800000U}, {8, 6, 0x7f800000U}, {8, 8, 0x7f800000U},
    };
    for (const BinarySpecialFloat& special : recorded_float) {
        const float xf = static_cast<float>(values[special.first_index]);
        const float yf = static_cast<float>(values[special.second_index]);
        require(same_bits(unflushed::hypot(xf, yf), std::bit_cast<float>(special.expected_bits)),
                "hypot(float): special input differs from its recorded result");
    }
    // Pythagorean triples are exact, at any common power of two.
    const double triples[][3] = {{3.0, 4.0, 5.0}, {5.0, 12.0, 13.0}, {8.0, 15.0, 17.0},
                                 {20.0, 21.0, 29.0}, {119.0, 120.0, 169.0}};
    for (const double* const triple : triples) {
        const int scales[] = {-1060, -500, 0, 500, 1010};
        for (const int scale : scales) {
            const double x = std::ldexp(triple[0], scale);
            const double y = std::ldexp(triple[1], scale);
            require(unflushed::hypot(x, y) == std::ldexp(triple[2], scale) &&
                        unflushed::hypot(-y, x) == std::ldexp(triple[2], scale),
                    "hypot(double): Pythagorean triple is not exact");
        }
        require(unflushed::hypot(static_cast<float>(triple[0]), static_cast<float>(triple[1])) ==
                    static_cast<float>(triple[2]),
                "hypot(float): Pythagorean triple is not exact");
    }
    require(std::isinf(unflushed::hypot(0x1.fffffffffffffp1023, 0x1.fffffffffffffp1023)),
            "hypot(double): did not overflow");
    // A subnormal larger operand in [2^-1023, 2^-1022) with a normal result:
    // scale_wide's normal-after-all branch at exponent -1023 returned 0
    // (audit 2026-11-15, A-stxlib-algorithms-01-F1). The correctly rounded
    // value is 0x1.0f876ccdf6cd9p-1022 (sqrt(4.5) rounded, exact scaling,
    // decided exactly in rationals); the kernel's bound allows a neighbour.
    {
        const double expected = 0x1.0f876ccdf6cd9p-1022;
        const double low_operand = unflushed::hypot(0x1.8p-1023, 0x1.8p-1023);
        require(low_operand >= std::nextafter(expected, 0.0) && low_operand <= std::nextafter(expected, 1.0),
                "hypot(double): a normal result from subnormal operands near 2^-1022 is wrong");
    }
}

void hypot_input(const int region, Lcg& lcg, double& x, double& y) {
    if (region == 0) {
        const double exponent = std::floor(lcg.unit() * 2000.0) - 1000.0;
        x = std::ldexp(lcg.unit(), static_cast<int>(exponent));
        y = x * std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 30.0));
    } else if (region == 1) {
        x = 1.0 + lcg.unit();
        y = x * (1.0 - 1e-3 * lcg.unit());
    } else if (region == 2) {
        x = 1.0 + lcg.unit();
        y = x * std::ldexp(1.0 + lcg.unit(), -28);
    } else if (region == 3) {
        x = std::ldexp(1.0 + lcg.unit(), 1022);
        y = std::ldexp(1.0 + lcg.unit(), 1021);
    } else {
        x = std::ldexp(lcg.unit(), -1060);
        y = std::ldexp(lcg.unit(), -1062);
    }
}

const char* const hypot_region_names[] = {"wide exponents", "ratio ~ 1", "ratio ~ 2^-27",
                                          "near overflow", "subnormal"};

void hypot_double_accuracy(const int per_region) {
    Lcg lcg{2032};
    for (int region = 0; region < 5; ++region) {
        double worst = 0.0;
        std::uint64_t misrounded = 0;
        std::uint64_t flushed_count = 0;
        int measured = 0;
        for (int i = 0; i < per_region; ++i) {
            double x = 0.0;
            double y = 0.0;
            hypot_input(region, lcg, x, y);
            if (x == 0.0 && y == 0.0) continue;
            const double result = unflushed::hypot(x, y);
            if (!std::isfinite(result)) continue;
            const wide::ScaledWide exact = wide::hypot_reference(x, y);
            double error = 0.0;
            if (subnormal(x) || subnormal(y) || result < 0x1p-1022) {
                // STXlib: subnormal operands read as zero and subnormal
                // results flush (math.hpp), verified exactly instead of
                // measured.
                const double public_result = stxlib::math::hypot(x, y);
                require(same_bits(public_result, flushed(unflushed::hypot(flushed(x), flushed(y)))),
                        "hypot(double): not the flushed kernel result");
                require(!subnormal(public_result), "hypot(double): a subnormal result");
                if (subnormal(x) && subnormal(y)) {
                    require(same_bits(public_result, 0.0), "hypot(double): two subnormals did not give +0");
                }
                flushed_count += 1;
                continue;
            } else {
                error = exp_ulps(result, exact);
                if (error > 0.5) misrounded += 1;
            }
            measured += 1;
            worst = std::max(worst, error);
        }
        std::printf("hypot(double) %-15s %10d inputs  max %.6f ulp  %llu not correctly rounded  %llu flushed\n",
                    hypot_region_names[region], measured, worst,
                    static_cast<unsigned long long>(misrounded),
                    static_cast<unsigned long long>(flushed_count));
        require(worst <= hypot_ulp_limit, "hypot(double): error above the stated bound");
    }
}

void hypot_float_rounding(const int count) {
    Lcg lcg{2033};
    int checked = 0;
    for (int i = 0; i < count; ++i) {
        const float x = static_cast<float>(std::ldexp(lcg.unit(), static_cast<int>(lcg.unit() * 250.0) - 125));
        float y = static_cast<float>(x * std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 14.0)));
        if (i % 3 == 0) y = static_cast<float>(lcg.unit());
        const float result = unflushed::hypot(x, y);
        if (!std::isfinite(result) || result == 0.0f) continue;
        const float expected = exp_correctly_rounded_float(
            wide::hypot_reference(static_cast<double>(x), static_cast<double>(y)));
        require(result == expected, "hypot(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("hypot(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t hypot_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    Lcg lcg{95};
    for (int region = 0; region < 5; ++region) {
        for (int i = 0; i < 400000; ++i) {
            double x = 0.0;
            double y = 0.0;
            hypot_input(region, lcg, x, y);
            const double result = unflushed::hypot(x, -y);
            const float result_float = unflushed::hypot(static_cast<float>(x), static_cast<float>(y));
            const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(result),
                                            std::bit_cast<std::uint32_t>(result_float)};
            for (const std::uint64_t word : words) {
                hash ^= word;
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t today_hypot_output_hash = 0x1c19c8b2f3910d6cULL;
// The region path of float hypot (region.cpp): frozen on 2026-11-15 (sbi,
// main f5cbc30); equal to today's at every sampled pair (region correctly
// rounded by construction, today's checked at sampled pairs; subnormal
// operands take today's kernel). Off by default since R3: identical results,
// 2.1 times slower (R2 bench).
constexpr std::uint64_t region_hypot_output_hash = 0x1c19c8b2f3910d6cULL;
constexpr std::uint64_t expected_hypot_output_hash = STXLIB_MATH_REGION_HYPOT_FLOAT ? region_hypot_output_hash
                                                                                    : today_hypot_output_hash;

void hypot_determinism() {
    const std::uint64_t hash = hypot_output_hash();
    check_frozen("hypot", hash, expected_hypot_output_hash, today_hypot_output_hash,
                 "hypot: outputs differ from the frozen bits");
}

// ---- atan, atan2, asin, acos ------------------------------------------------------

using UnaryDouble = double (*)(double x);
using UnaryFloat = float (*)(float x);
using UnaryReference = wide::Wide (*)(double x);

double owned_atan_d(const double x) { return unflushed::atan(x); }
float owned_atan_f(const float x) { return unflushed::atan(x); }
double owned_asin_d(const double x) { return unflushed::asin(x); }
float owned_asin_f(const float x) { return unflushed::asin(x); }
double owned_acos_d(const double x) { return unflushed::acos(x); }
float owned_acos_f(const float x) { return unflushed::acos(x); }
double library_atan_d(const double x) { return std::atan(x); }
double library_asin_d(const double x) { return std::asin(x); }
double library_acos_d(const double x) { return std::acos(x); }

wide::Wide atan_exact(const double x) {
    const wide::Wide magnitude = wide::atan_reference(wide::Wide{std::fabs(x), 0.0});
    if (x < 0.0) return wide::negate(magnitude);
    return magnitude;
}

struct UnaryKernel final {
    const char* name{nullptr};
    UnaryDouble owned_double{nullptr};
    UnaryFloat owned_float{nullptr};
    UnaryDouble library_double{nullptr};
    UnaryReference exact{nullptr};
    double domain{0.0};  // |x| <= domain is the finite domain (0: unbounded)
};

const UnaryKernel inverse_trig_kernels[] = {
    {"atan", owned_atan_d, owned_atan_f, library_atan_d, atan_exact, 0.0},
    {"asin", owned_asin_d, owned_asin_f, library_asin_d, wide::asin_reference, 1.0},
    {"acos", owned_acos_d, owned_acos_f, library_acos_d, wide::acos_reference, 1.0},
};

// A recorded special case of an inverse trig kernel: the kernel's index in
// inverse_trig_kernels, the input's index, and the double and float results.
struct InverseTrigSpecial final {
    std::size_t kernel_index{0};
    std::size_t input_index{0};
    std::uint64_t double_bits{0};
    std::uint32_t float_bits{0};
};

// Annex F: special inputs give their recorded results (zeros, NaN, or pi
// multiples rounded from one constant). The results are recorded rather than
// taken from the host C library; any NaN matches a recorded NaN. Ordinary
// values (atan(+-2), asin and acos inside the domain) are checked for
// accuracy elsewhere.
void inverse_trig_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double unary_inputs[] = {0.0, -0.0, 1.0, -1.0, 2.0, -2.0, infinity, -infinity, nan,
                                   0x1p-1074, -0x1p-30, 0x1p61, -0x1p70};
    // {kernel index, unary_inputs index, double result bits, float result bits}
    const InverseTrigSpecial recorded[] = {
        {0, 0, 0x0000000000000000ULL, 0x00000000U}, {0, 1, 0x8000000000000000ULL, 0x80000000U},
        {0, 6, 0x3ff921fb54442d18ULL, 0x3fc90fdbU}, {0, 7, 0xbff921fb54442d18ULL, 0xbfc90fdbU},
        {0, 8, 0x7ff8000000000000ULL, 0x7fc00000U}, {0, 9, 0x0000000000000001ULL, 0x00000000U},
        {0, 10, 0xbe10000000000000ULL, 0xb0800000U}, {0, 11, 0x3ff921fb54442d18ULL, 0x3fc90fdbU},
        {0, 12, 0xbff921fb54442d18ULL, 0xbfc90fdbU}, {1, 0, 0x0000000000000000ULL, 0x00000000U},
        {1, 1, 0x8000000000000000ULL, 0x80000000U}, {1, 2, 0x3ff921fb54442d18ULL, 0x3fc90fdbU},
        {1, 3, 0xbff921fb54442d18ULL, 0xbfc90fdbU}, {1, 4, 0x7ff8000000000000ULL, 0x7fc00000U},
        {1, 5, 0x7ff8000000000000ULL, 0x7fc00000U}, {1, 6, 0x7ff8000000000000ULL, 0x7fc00000U},
        {1, 7, 0x7ff8000000000000ULL, 0x7fc00000U}, {1, 8, 0x7ff8000000000000ULL, 0x7fc00000U},
        {1, 9, 0x0000000000000001ULL, 0x00000000U}, {1, 10, 0xbe10000000000000ULL, 0xb0800000U},
        {1, 11, 0x7ff8000000000000ULL, 0x7fc00000U}, {1, 12, 0x7ff8000000000000ULL, 0x7fc00000U},
        {2, 0, 0x3ff921fb54442d18ULL, 0x3fc90fdbU}, {2, 1, 0x3ff921fb54442d18ULL, 0x3fc90fdbU},
        {2, 2, 0x0000000000000000ULL, 0x00000000U}, {2, 3, 0x400921fb54442d18ULL, 0x40490fdbU},
        {2, 4, 0x7ff8000000000000ULL, 0x7fc00000U}, {2, 5, 0x7ff8000000000000ULL, 0x7fc00000U},
        {2, 6, 0x7ff8000000000000ULL, 0x7fc00000U}, {2, 7, 0x7ff8000000000000ULL, 0x7fc00000U},
        {2, 8, 0x7ff8000000000000ULL, 0x7fc00000U}, {2, 9, 0x3ff921fb54442d18ULL, 0x3fc90fdbU},
        {2, 10, 0x3ff921fb54842d18ULL, 0x3fc90fdbU}, {2, 11, 0x7ff8000000000000ULL, 0x7fc00000U},
        {2, 12, 0x7ff8000000000000ULL, 0x7fc00000U},
    };
    for (const InverseTrigSpecial& special : recorded) {
        const UnaryKernel& kernel = inverse_trig_kernels[special.kernel_index];
        const double x = unary_inputs[special.input_index];
        require(same_bits((*kernel.owned_double)(x), std::bit_cast<double>(special.double_bits)),
                "inverse trig (double): special input differs from its recorded result");
        const float xf = static_cast<float>(x);
        require(same_bits((*kernel.owned_float)(xf), std::bit_cast<float>(special.float_bits)),
                "inverse trig (float): special input differs from its recorded result");
    }
    const double atan2_inputs[] = {0.0, -0.0, 1.0, -1.0, infinity, -infinity, nan, 0x1p-1074,
                                   -0x1.fffffffffffffp1023};
    // {y index, x index, result bits}, both indices into atan2_inputs
    const BinarySpecial recorded_atan2[] = {
        {0, 0, 0x0000000000000000ULL}, {0, 1, 0x400921fb54442d18ULL},
        {0, 2, 0x0000000000000000ULL}, {0, 3, 0x400921fb54442d18ULL},
        {0, 4, 0x0000000000000000ULL}, {0, 5, 0x400921fb54442d18ULL},
        {0, 6, 0x7ff8000000000000ULL}, {0, 7, 0x0000000000000000ULL},
        {0, 8, 0x400921fb54442d18ULL}, {1, 0, 0x8000000000000000ULL},
        {1, 1, 0xc00921fb54442d18ULL}, {1, 2, 0x8000000000000000ULL},
        {1, 3, 0xc00921fb54442d18ULL}, {1, 4, 0x8000000000000000ULL},
        {1, 5, 0xc00921fb54442d18ULL}, {1, 6, 0x7ff8000000000000ULL},
        {1, 7, 0x8000000000000000ULL}, {1, 8, 0xc00921fb54442d18ULL},
        {2, 0, 0x3ff921fb54442d18ULL}, {2, 1, 0x3ff921fb54442d18ULL},
        {2, 4, 0x0000000000000000ULL}, {2, 5, 0x400921fb54442d18ULL},
        {2, 6, 0x7ff8000000000000ULL}, {3, 0, 0xbff921fb54442d18ULL},
        {3, 1, 0xbff921fb54442d18ULL}, {3, 4, 0x8000000000000000ULL},
        {3, 5, 0xc00921fb54442d18ULL}, {3, 6, 0x7ff8000000000000ULL},
        {4, 0, 0x3ff921fb54442d18ULL}, {4, 1, 0x3ff921fb54442d18ULL},
        {4, 2, 0x3ff921fb54442d18ULL}, {4, 3, 0x3ff921fb54442d18ULL},
        {4, 4, 0x3fe921fb54442d18ULL}, {4, 5, 0x4002d97c7f3321d2ULL},
        {4, 6, 0x7ff8000000000000ULL}, {4, 7, 0x3ff921fb54442d18ULL},
        {4, 8, 0x3ff921fb54442d18ULL}, {5, 0, 0xbff921fb54442d18ULL},
        {5, 1, 0xbff921fb54442d18ULL}, {5, 2, 0xbff921fb54442d18ULL},
        {5, 3, 0xbff921fb54442d18ULL}, {5, 4, 0xbfe921fb54442d18ULL},
        {5, 5, 0xc002d97c7f3321d2ULL}, {5, 6, 0x7ff8000000000000ULL},
        {5, 7, 0xbff921fb54442d18ULL}, {5, 8, 0xbff921fb54442d18ULL},
        {6, 0, 0x7ff8000000000000ULL}, {6, 1, 0x7ff8000000000000ULL},
        {6, 2, 0x7ff8000000000000ULL}, {6, 3, 0x7ff8000000000000ULL},
        {6, 4, 0x7ff8000000000000ULL}, {6, 5, 0x7ff8000000000000ULL},
        {6, 6, 0x7ff8000000000000ULL}, {6, 7, 0x7ff8000000000000ULL},
        {6, 8, 0x7ff8000000000000ULL}, {7, 0, 0x3ff921fb54442d18ULL},
        {7, 1, 0x3ff921fb54442d18ULL}, {7, 4, 0x0000000000000000ULL},
        {7, 5, 0x400921fb54442d18ULL}, {7, 6, 0x7ff8000000000000ULL},
        {8, 0, 0xbff921fb54442d18ULL}, {8, 1, 0xbff921fb54442d18ULL},
        {8, 4, 0x8000000000000000ULL}, {8, 5, 0xc00921fb54442d18ULL},
        {8, 6, 0x7ff8000000000000ULL},
    };
    for (const BinarySpecial& special : recorded_atan2) {
        const double y = atan2_inputs[special.first_index];
        const double x = atan2_inputs[special.second_index];
        require(same_bits(unflushed::atan2(y, x), std::bit_cast<double>(special.expected_bits)),
                "atan2(double): special input differs from its recorded result");
    }
    require(same_bits(unflushed::acos(1.0), 0.0) && same_bits(unflushed::acos(1.0f), 0.0f),
            "acos: acos(1) is not +0");
}

double inverse_trig_input(const UnaryKernel& kernel, const int region, Lcg& lcg) {
    if (kernel.domain == 0.0) {
        const double magnitude = std::ldexp(lcg.unit(), static_cast<int>(lcg.unit() * 80.0) - 40);
        return region == 0 ? magnitude : -magnitude;
    }
    if (region == 0) return 2.0 * lcg.unit() - 1.0;
    // Next to +-1, where 1 - x^2 cancels.
    const double gap = std::ldexp(lcg.unit(), -static_cast<int>(lcg.unit() * 50.0));
    return lcg.unit() < 0.5 ? 1.0 - gap : gap - 1.0;
}

void inverse_trig_double_accuracy(const int per_region) {
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        Lcg lcg{2034};
        for (int region = 0; region < 2; ++region) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = inverse_trig_input(kernel, region, lcg);
                const double error = double_ulps((*kernel.owned_double)(x), (*kernel.exact)(x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            const char* const region_name = kernel.domain == 0.0 ? (region == 0 ? "positive" : "negative")
                                                                 : (region == 0 ? "[-1, 1]" : "near +-1");
            std::printf("%s(double) %-10s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, region_name, per_region, worst, worst_x,
                        static_cast<unsigned long long>(misrounded));
            require(worst <= inverse_trig_ulp_limit, "inverse trig (double): error above the bound");
        }
    }
    Lcg lcg{2035};
    double worst = 0.0;
    for (int i = 0; i < per_region; ++i) {
        const double x = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const double y = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        if (x == 0.0 || y == 0.0) continue;
        worst = std::max(worst, double_ulps(unflushed::atan2(y, x), wide::atan2_reference(y, x)));
    }
    std::printf("atan2(double) %10d pairs  max %.6f ulp\n", per_region, worst);
    require(worst <= inverse_trig_ulp_limit, "atan2(double): error above the bound");
}

// Every `stride`-th float in each function's domain, both signs.
void inverse_trig_float_rounding(const std::uint32_t stride) {
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        const float top = kernel.domain == 0.0 ? std::numeric_limits<float>::max() : 1.0f;
        const std::uint32_t last = std::bit_cast<std::uint32_t>(top);
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = 0; pattern <= last; pattern += stride) {
            const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float signs[2] = {magnitude, -magnitude};
            for (const float x : signs) {
                const float y = (*kernel.owned_float)(x);
                const double xd = static_cast<double>(x);
                const double library = (*kernel.library_double)(xd);
                const float screen = static_cast<float>(library);
                checked += 1;
                if (y == screen && screen_holds(library)) continue;
                const float expected = correctly_rounded_float((*kernel.exact)(xd));
                if (y != expected) {
                    report_misrounded(kernel.name, xd, static_cast<double>(y), static_cast<double>(expected));
                }
            }
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
    Lcg lcg{2036};
    int checked = 0;
    for (int i = 0; i < 300000; ++i) {
        const float x = static_cast<float>(std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 60.0) - 30));
        const float y = static_cast<float>(std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 60.0) - 30));
        if (x == 0.0f || y == 0.0f) continue;
        const float result = unflushed::atan2(y, x);
        require(result == correctly_rounded_float(wide::atan2_reference(static_cast<double>(y), static_cast<double>(x))),
                "atan2(float): result is not correctly rounded");
        checked += 1;
    }
    std::printf("atan2(float) correctly rounded at %d random pairs\n", checked);
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t inverse_trig_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const UnaryKernel& kernel : inverse_trig_kernels) {
        Lcg lcg{94};
        for (int region = 0; region < 2; ++region) {
            for (int i = 0; i < 500000; ++i) {
                const double x = inverse_trig_input(kernel, region, lcg);
                const double y = (*kernel.owned_double)(x);
                const float yf = (*kernel.owned_float)(static_cast<float>(x));
                const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(y), std::bit_cast<std::uint32_t>(yf)};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    Lcg lcg{93};
    for (int i = 0; i < 1000000; ++i) {
        const double x = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const double y = std::ldexp(lcg.unit() - 0.5, static_cast<int>(lcg.unit() * 40.0) - 20);
        const std::uint64_t words[2] = {std::bit_cast<std::uint64_t>(unflushed::atan2(y, x)),
                                        std::bit_cast<std::uint32_t>(unflushed::atan2(
                                            static_cast<float>(y), static_cast<float>(x)))};
        for (const std::uint64_t word : words) {
            hash ^= word;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
constexpr std::uint64_t expected_inverse_trig_output_hash = 0xcaa4628c93d69869ULL;

void inverse_trig_determinism() {
    const std::uint64_t hash = inverse_trig_output_hash();
    std::printf("inverse trig output hash %016llx\n", static_cast<unsigned long long>(hash));
    require(hash == expected_inverse_trig_output_hash, "inverse trig: outputs differ from the frozen bits");
}

// ---- tan, cbrt, hyperbolic ------------------------------------------------------------

double owned_tan_d(const double x) { return unflushed::tan(x); }
float owned_tan_f(const float x) { return unflushed::tan(x); }
double owned_sinh_d(const double x) { return unflushed::sinh(x); }
float owned_sinh_f(const float x) { return unflushed::sinh(x); }
double owned_cosh_d(const double x) { return unflushed::cosh(x); }
float owned_cosh_f(const float x) { return unflushed::cosh(x); }
double owned_tanh_d(const double x) { return unflushed::tanh(x); }
float owned_tanh_f(const float x) { return unflushed::tanh(x); }
double owned_asinh_d(const double x) { return unflushed::asinh(x); }
float owned_asinh_f(const float x) { return unflushed::asinh(x); }
double owned_acosh_d(const double x) { return unflushed::acosh(x); }
float owned_acosh_f(const float x) { return unflushed::acosh(x); }
double owned_atanh_d(const double x) { return unflushed::atanh(x); }
float owned_atanh_f(const float x) { return unflushed::atanh(x); }
double owned_cbrt_d(const double x) { return unflushed::cbrt(x); }
float owned_cbrt_f(const float x) { return unflushed::cbrt(x); }
double library_tan_d(const double x) { return std::tan(x); }
double library_sinh_d(const double x) { return std::sinh(x); }
double library_cosh_d(const double x) { return std::cosh(x); }
double library_tanh_d(const double x) { return std::tanh(x); }
double library_asinh_d(const double x) { return std::asinh(x); }
double library_acosh_d(const double x) { return std::acosh(x); }
double library_atanh_d(const double x) { return std::atanh(x); }
double library_cbrt_d(const double x) { return std::cbrt(x); }

// cbrt's reference as a plain double-double (results stay normal).
wide::Wide cbrt_exact(const double x) {
    const wide::ScaledWide value = wide::cbrt_reference(x);
    const wide::Wide result{std::ldexp(value.mantissa.hi, value.exponent),
                            std::ldexp(value.mantissa.lo, value.exponent)};
    return result;
}

// A sampled region: uniform in [low, high], or (logarithmic) magnitudes
// 2^[low, high) with both signs.
struct SampleRegion final {
    double low{0.0};
    double high{0.0};
    bool logarithmic{false};
};

struct RegionKernel final {
    const char* name{nullptr};
    UnaryDouble owned_double{nullptr};
    UnaryFloat owned_float{nullptr};
    UnaryDouble library_double{nullptr};
    UnaryReference exact{nullptr};
    SampleRegion regions[3]{};
    float float_low{0.0f};   // float exhaustive range (magnitudes, both signs unless
    float float_high{0.0f};  // float_low >= 1, which marks acosh's one-sided domain)
};

const RegionKernel remaining_kernels[] = {
    {"tan", owned_tan_d, owned_tan_f, library_tan_d, wide::tan_reference,
     {{-20.0, 20.0, false}, {1.5, 1.6415, false}, {-26.0, 32.0, true}}, 0x1p-27f, 0x1p32f},
    {"cbrt", owned_cbrt_d, owned_cbrt_f, library_cbrt_d, cbrt_exact,
     {{-1000.0, 1000.0, true}, {0.0, 8.0, false}, {-1074.0, -1022.0, true}}, 0.0f, 3.4e38f},
    {"sinh", owned_sinh_d, owned_sinh_f, library_sinh_d, wide::sinh_reference,
     {{-26.0, 9.0, true}, {-700.0, 700.0, false}, {-0.125, 0.125, false}}, 0.0f, 90.0f},
    {"cosh", owned_cosh_d, owned_cosh_f, library_cosh_d, wide::cosh_reference,
     {{-26.0, 9.0, true}, {-700.0, 700.0, false}, {-1.0, 1.0, false}}, 0.0f, 90.0f},
    {"tanh", owned_tanh_d, owned_tanh_f, library_tanh_d, wide::tanh_reference,
     {{-26.0, 5.0, true}, {-0.125, 0.125, false}, {-22.0, 22.0, false}}, 0.0f, 23.0f},
    {"asinh", owned_asinh_d, owned_asinh_f, library_asinh_d, wide::asinh_reference,
     {{-26.0, 27.0, true}, {-1.0, 1.0, false}, {-1e8, 1e8, false}}, 0.0f, 0x1p28f},
    {"acosh", owned_acosh_d, owned_acosh_f, library_acosh_d, wide::acosh_reference,
     {{1.0, 3.0, false}, {1.0, 1.000001, false}, {2.0, 1e8, false}}, 1.0f, 0x1p28f},
    {"atanh", owned_atanh_d, owned_atanh_f, library_atanh_d, wide::atanh_reference,
     {{-26.0, -1.0, true}, {0.99, 0.9999999, false}, {-0.5, 0.5, false}}, 0.0f, 1.0f},
};

double region_input(const SampleRegion& region, Lcg& lcg, const int i) {
    if (!region.logarithmic) return region.low + (region.high - region.low) * lcg.unit();
    const double exponent = std::floor(region.low + (region.high - region.low) * lcg.unit());
    const double magnitude = std::ldexp(0.5 + 0.5 * lcg.unit(), static_cast<int>(exponent));
    return (i % 2 == 0) ? magnitude : -magnitude;
}

// Recorded results for inputs whose results are zeros, ones, infinities, NaN
// or tiny (|x| < 2^-27). They are recorded rather than taken from the host C
// library, which differs between hosts: UCRT's cbrt(-0x1p-40) is one ulp
// below the correctly rounded -0x1.965fea53d6e3dp-14 (long double cbrtl
// agrees with the recorded value), and UCRT's tan(+-infinity) is a negative
// NaN. Any NaN matches a recorded NaN, since C leaves its sign unspecified.
struct SpecialValue final {
    UnaryDouble owned{nullptr};
    double x{0.0};
    std::uint64_t expected_bits{0};
};

void remaining_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const SpecialValue recorded[] = {
        {owned_tan_d, 0.0, 0x0000000000000000ULL},
        {owned_tan_d, -0.0, 0x8000000000000000ULL},
        {owned_tan_d, infinity, 0x7ff8000000000000ULL},
        {owned_tan_d, -infinity, 0x7ff8000000000000ULL},
        {owned_tan_d, nan, 0x7ff8000000000000ULL},
        {owned_tan_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_tan_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_cbrt_d, 0.0, 0x0000000000000000ULL},
        {owned_cbrt_d, -0.0, 0x8000000000000000ULL},
        {owned_cbrt_d, 1.0, 0x3ff0000000000000ULL},
        {owned_cbrt_d, -1.0, 0xbff0000000000000ULL},
        {owned_cbrt_d, infinity, 0x7ff0000000000000ULL},
        {owned_cbrt_d, -infinity, 0xfff0000000000000ULL},
        {owned_cbrt_d, nan, 0x7ff8000000000000ULL},
        {owned_cbrt_d, 0x1p-1074, 0x2990000000000000ULL},
        {owned_cbrt_d, -0x1p-40, 0xbf1965fea53d6e3dULL},
        {owned_sinh_d, 0.0, 0x0000000000000000ULL},
        {owned_sinh_d, -0.0, 0x8000000000000000ULL},
        {owned_sinh_d, infinity, 0x7ff0000000000000ULL},
        {owned_sinh_d, -infinity, 0xfff0000000000000ULL},
        {owned_sinh_d, nan, 0x7ff8000000000000ULL},
        {owned_sinh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_sinh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_sinh_d, 800.0, 0x7ff0000000000000ULL},
        {owned_sinh_d, -800.0, 0xfff0000000000000ULL},
        {owned_sinh_d, 1e300, 0x7ff0000000000000ULL},
        {owned_cosh_d, 0.0, 0x3ff0000000000000ULL},
        {owned_cosh_d, -0.0, 0x3ff0000000000000ULL},
        {owned_cosh_d, infinity, 0x7ff0000000000000ULL},
        {owned_cosh_d, -infinity, 0x7ff0000000000000ULL},
        {owned_cosh_d, nan, 0x7ff8000000000000ULL},
        {owned_cosh_d, 0x1p-1074, 0x3ff0000000000000ULL},
        {owned_cosh_d, -0x1p-40, 0x3ff0000000000000ULL},
        {owned_cosh_d, 800.0, 0x7ff0000000000000ULL},
        {owned_cosh_d, -800.0, 0x7ff0000000000000ULL},
        {owned_cosh_d, 1e300, 0x7ff0000000000000ULL},
        {owned_tanh_d, 0.0, 0x0000000000000000ULL},
        {owned_tanh_d, -0.0, 0x8000000000000000ULL},
        {owned_tanh_d, infinity, 0x3ff0000000000000ULL},
        {owned_tanh_d, -infinity, 0xbff0000000000000ULL},
        {owned_tanh_d, nan, 0x7ff8000000000000ULL},
        {owned_tanh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_tanh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_tanh_d, 800.0, 0x3ff0000000000000ULL},
        {owned_tanh_d, -800.0, 0xbff0000000000000ULL},
        {owned_tanh_d, 1e300, 0x3ff0000000000000ULL},
        {owned_asinh_d, 0.0, 0x0000000000000000ULL},
        {owned_asinh_d, -0.0, 0x8000000000000000ULL},
        {owned_asinh_d, infinity, 0x7ff0000000000000ULL},
        {owned_asinh_d, -infinity, 0xfff0000000000000ULL},
        {owned_asinh_d, nan, 0x7ff8000000000000ULL},
        {owned_asinh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_asinh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_acosh_d, 0.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, -0.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, 1.0, 0x0000000000000000ULL},
        {owned_acosh_d, -1.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, -2.0, 0x7ff8000000000000ULL},
        {owned_acosh_d, infinity, 0x7ff0000000000000ULL},
        {owned_acosh_d, -infinity, 0x7ff8000000000000ULL},
        {owned_acosh_d, nan, 0x7ff8000000000000ULL},
        {owned_acosh_d, 0x1p-1074, 0x7ff8000000000000ULL},
        {owned_acosh_d, -0x1p-40, 0x7ff8000000000000ULL},
        {owned_acosh_d, -800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, 0.0, 0x0000000000000000ULL},
        {owned_atanh_d, -0.0, 0x8000000000000000ULL},
        {owned_atanh_d, 1.0, 0x7ff0000000000000ULL},
        {owned_atanh_d, -1.0, 0xfff0000000000000ULL},
        {owned_atanh_d, 2.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, -2.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, infinity, 0x7ff8000000000000ULL},
        {owned_atanh_d, -infinity, 0x7ff8000000000000ULL},
        {owned_atanh_d, nan, 0x7ff8000000000000ULL},
        {owned_atanh_d, 0x1p-1074, 0x0000000000000001ULL},
        {owned_atanh_d, -0x1p-40, 0xbd70000000000000ULL},
        {owned_atanh_d, 800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, -800.0, 0x7ff8000000000000ULL},
        {owned_atanh_d, 1e300, 0x7ff8000000000000ULL},
    };
    for (const SpecialValue& special : recorded) {
        const double expected = std::bit_cast<double>(special.expected_bits);
        require(same_bits((*special.owned)(special.x), expected),
                "tan/cbrt/hyperbolic (double): special input differs from its recorded result");
    }
    for (int k = 1; k <= 200; ++k) {
        const double cube = static_cast<double>(k) * static_cast<double>(k) * static_cast<double>(k);
        require(unflushed::cbrt(cube) == static_cast<double>(k) &&
                    unflushed::cbrt(-cube) == -static_cast<double>(k) &&
                    unflushed::cbrt(static_cast<float>(cube)) == static_cast<float>(k),
                "cbrt: an exact cube's root is not exact");
    }
    require(unflushed::cbrt(0x1p-1074) == 0x1p-358, "cbrt(double): 2^-1074 has no exact root");
}

void remaining_double_accuracy(const int per_region) {
    for (const RegionKernel& kernel : remaining_kernels) {
        Lcg lcg{2037};
        for (const SampleRegion& region : kernel.regions) {
            double worst = 0.0;
            double worst_x = 0.0;
            std::uint64_t misrounded = 0;
            for (int i = 0; i < per_region; ++i) {
                const double x = region_input(region, lcg, i);
                const double error = double_ulps((*kernel.owned_double)(x), (*kernel.exact)(x));
                if (error > 0.5) misrounded += 1;
                if (error > worst) {
                    worst = error;
                    worst_x = x;
                }
            }
            std::printf("%s(double) [%g, %g]%s %10d inputs  max %.6f ulp at %a  %llu above 0.5 ulp\n",
                        kernel.name, region.low, region.high, region.logarithmic ? " (2^)" : "",
                        per_region, worst, worst_x, static_cast<unsigned long long>(misrounded));
            const double limit = std::strcmp(kernel.name, "tan") == 0 ? tan_ulp_limit : remaining_ulp_limit;
            require(worst <= limit, "tan/cbrt/hyperbolic (double): error above the stated bound");
        }
    }
}

// Every `stride`-th float with magnitude in each kernel's float range (both
// signs, except acosh's one-sided domain).
void remaining_float_rounding(const std::uint32_t stride) {
    for (const RegionKernel& kernel : remaining_kernels) {
        const std::uint32_t first = std::bit_cast<std::uint32_t>(kernel.float_low);
        const std::uint32_t last = std::bit_cast<std::uint32_t>(kernel.float_high);
        const bool one_sided = kernel.float_low >= 1.0f;
        std::uint64_t checked = 0;
        for (std::uint64_t pattern = first; pattern <= last; pattern += stride) {
            const float magnitude = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
            const float signs[2] = {magnitude, -magnitude};
            for (int sign = 0; sign < (one_sided ? 1 : 2); ++sign) {
                const float x = signs[sign];
                const float y = (*kernel.owned_float)(x);
                const double xd = static_cast<double>(x);
                const double library = (*kernel.library_double)(xd);
                const float screen = static_cast<float>(library);
                checked += 1;
                if (same_bits(y, screen) && screen_holds(library)) continue;
                const float expected = correctly_rounded_float((*kernel.exact)(xd));
                if (!std::isfinite(y) || y != expected) {
                    report_misrounded(kernel.name, xd, static_cast<double>(y), static_cast<double>(expected));
                }
            }
        }
        std::printf("%s(float) correctly rounded at %llu floats (stride %u)\n", kernel.name,
                    static_cast<unsigned long long>(checked), stride);
    }
}

// FNV-1a: multiplication modulo 2^64 by design.
STXLIB_MODULAR std::uint64_t remaining_output_hash() {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const RegionKernel& kernel : remaining_kernels) {
        Lcg lcg{92};
        for (const SampleRegion& region : kernel.regions) {
            for (int i = 0; i < 300000; ++i) {
                const double x = region_input(region, lcg, i);
                const std::uint64_t words[2] = {
                    std::bit_cast<std::uint64_t>((*kernel.owned_double)(x)),
                    std::bit_cast<std::uint32_t>((*kernel.owned_float)(static_cast<float>(x)))};
                for (const std::uint64_t word : words) {
                    hash ^= word;
                    hash *= 1099511628211ULL;
                }
            }
        }
    }
    return hash;
}

// Frozen on 2026-10-31 (Apple arm64, fma and Dekker builds, -O0 and -O2).
// Replaced on 2026-11-02 (was 0xd4817d61597e5a9d; tan changed): owner's 26-bit split,
// adopted 2026-11-02; 46/8M sincos and 64/4M tan outputs differ, all within
// the unchanged bound. Same value at the scalar, vector128 and avx2_fma
// levels and in the Dekker build (x86-64 MinGW).
constexpr std::uint64_t today_remaining_output_hash = 0x46fd05aac7dac755ULL;
// The region path of tan(double) (trig_region.cpp): frozen on 2026-11-15
// (sbi, main f5cbc30). Equal to today's: at every sampled input region tan
// gives today's bits, though R2 predicted Estrin's scheme would round some
// differently (both within the same bound; sampled, not proved equal).
constexpr std::uint64_t region_remaining_output_hash = 0x46fd05aac7dac755ULL;
constexpr std::uint64_t expected_remaining_output_hash =
    STXLIB_MATH_REGION_TRIG_DOUBLE ? region_remaining_output_hash : today_remaining_output_hash;

void remaining_determinism() {
    const std::uint64_t hash = remaining_output_hash();
    check_frozen("tan/cbrt/hyperbolic", hash, expected_remaining_output_hash, today_remaining_output_hash,
                 "tan/cbrt/hyperbolic: outputs differ from the frozen bits");
}

// ---- the subnormal rule (STXlib batch 3c8) ------------------------------------
//
// Every public function of stxlib/math.hpp reads a subnormal argument as zero
// of its sign and returns a would-be-subnormal result as zero of its sign
// (owner decision 2026-11-13). Checked exactly, bit for bit: the public
// result equals flushed(unflushed(flushed(arguments))), with this file's own
// flushed(), and is never subnormal. --exhaustive takes every float for the
// one-argument float functions (the subnormal range included), every
// subnormal float against fixed partners for the two-argument ones, and 20
// million doubles per category; the ctest run strides through the same.

using FloatUnary = float (*)(float) noexcept;
using DoubleUnary = double (*)(double) noexcept;
using FloatBinary = float (*)(float, float) noexcept;
using DoubleBinary = double (*)(double, double) noexcept;

struct FlushUnary final {
    const char* name{nullptr};
    FloatUnary public_float{nullptr};
    FloatUnary unflushed_float{nullptr};
    DoubleUnary public_double{nullptr};
    DoubleUnary unflushed_double{nullptr};
};

struct FlushBinary final {
    const char* name{nullptr};
    FloatBinary public_float{nullptr};
    FloatBinary unflushed_float{nullptr};
    DoubleBinary public_double{nullptr};
    DoubleBinary unflushed_double{nullptr};
};

const FlushUnary flush_unary[] = {
    {"exp", stxlib::math::exp, unflushed::exp, stxlib::math::exp, unflushed::exp},
    {"log", stxlib::math::log, unflushed::log, stxlib::math::log, unflushed::log},
    {"log2", stxlib::math::log2, unflushed::log2, stxlib::math::log2, unflushed::log2},
    {"log10", stxlib::math::log10, unflushed::log10, stxlib::math::log10, unflushed::log10},
    {"atan", stxlib::math::atan, unflushed::atan, stxlib::math::atan, unflushed::atan},
    {"asin", stxlib::math::asin, unflushed::asin, stxlib::math::asin, unflushed::asin},
    {"acos", stxlib::math::acos, unflushed::acos, stxlib::math::acos, unflushed::acos},
    {"tan", stxlib::math::tan, unflushed::tan, stxlib::math::tan, unflushed::tan},
    {"cbrt", stxlib::math::cbrt, unflushed::cbrt, stxlib::math::cbrt, unflushed::cbrt},
    {"sinh", stxlib::math::sinh, unflushed::sinh, stxlib::math::sinh, unflushed::sinh},
    {"cosh", stxlib::math::cosh, unflushed::cosh, stxlib::math::cosh, unflushed::cosh},
    {"tanh", stxlib::math::tanh, unflushed::tanh, stxlib::math::tanh, unflushed::tanh},
    {"asinh", stxlib::math::asinh, unflushed::asinh, stxlib::math::asinh, unflushed::asinh},
    {"acosh", stxlib::math::acosh, unflushed::acosh, stxlib::math::acosh, unflushed::acosh},
    {"atanh", stxlib::math::atanh, unflushed::atanh, stxlib::math::atanh, unflushed::atanh},
};

const FlushBinary flush_binary[] = {
    {"pow", stxlib::math::pow, unflushed::pow, stxlib::math::pow, unflushed::pow},
    {"hypot", stxlib::math::hypot, unflushed::hypot, stxlib::math::hypot, unflushed::hypot},
    {"atan2", stxlib::math::atan2, unflushed::atan2, stxlib::math::atan2, unflushed::atan2},
};

// Partners for the two-argument functions: zeros, subnormals at both ends,
// the smallest normal, ordinary values, exponents that take pow's results
// into the subnormal range, huge values (atan2's tiny quotients),
// infinities and NaN.
const double flush_partners[] = {0.0, -0.0, 0x1p-1074, -0x1p-1074, 0x0.fffffffffffffp-1022,
                                 -0x0.8p-1022, 0x1p-149, -0x1p-140, 0x1p-1022, -0x1p-1022,
                                 0x1p-126, 1.0, -1.0, 0.5, 2.0, -3.0, 1074.0, -1074.0, 1060.5,
                                 140.0, -140.0, 149.5, 1e30, 0x1p1000, 0x1.fffffep127,
                                 std::numeric_limits<double>::infinity(),
                                 -std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()};

void flush_failed(const char* name, const char* type, const double x, const double y) {
    std::fprintf(stderr, "subnormal rule broken: %s(%s) at x = %a, y = %a\n", name, type, x, y);
    require(false, "math: a public result breaks the subnormal rule");
}

void check_unary_float(const FlushUnary& unary, const float x) {
    const float result = (*unary.public_float)(x);
    const float expected = flushed((*unary.unflushed_float)(flushed(x)));
    if (!same_bits(result, expected) || subnormal(result)) flush_failed(unary.name, "float", x, 0.0);
}

void check_unary_double(const FlushUnary& unary, const double x) {
    const double result = (*unary.public_double)(x);
    const double expected = flushed((*unary.unflushed_double)(flushed(x)));
    if (!same_bits(result, expected) || subnormal(result)) flush_failed(unary.name, "double", x, 0.0);
}

void check_binary_float(const FlushBinary& binary, const float x, const float y) {
    const float result = (*binary.public_float)(x, y);
    const float expected = flushed((*binary.unflushed_float)(flushed(x), flushed(y)));
    if (!same_bits(result, expected) || subnormal(result)) flush_failed(binary.name, "float", x, y);
}

void check_binary_double(const FlushBinary& binary, const double x, const double y) {
    const double result = (*binary.public_double)(x, y);
    const double expected = flushed((*binary.unflushed_double)(flushed(x), flushed(y)));
    if (!same_bits(result, expected) || subnormal(result)) flush_failed(binary.name, "double", x, y);
}

void check_sincos_float(const float x) {
    float s = 0.0f;
    float c = 0.0f;
    stxlib::math::sincos(x, s, c);
    float unflushed_s = 0.0f;
    float unflushed_c = 0.0f;
    unflushed::sincos(flushed(x), unflushed_s, unflushed_c);
    if (!same_bits(s, flushed(unflushed_s)) || !same_bits(c, flushed(unflushed_c)) || subnormal(s) ||
        subnormal(c)) {
        flush_failed("sincos", "float", x, 0.0);
    }
}

void check_sincos_double(const double x) {
    double s = 0.0;
    double c = 0.0;
    stxlib::math::sincos(x, s, c);
    double unflushed_s = 0.0;
    double unflushed_c = 0.0;
    unflushed::sincos(flushed(x), unflushed_s, unflushed_c);
    if (!same_bits(s, flushed(unflushed_s)) || !same_bits(c, flushed(unflushed_c)) || subnormal(s) ||
        subnormal(c)) {
        flush_failed("sincos", "double", x, 0.0);
    }
}

// A random subnormal double of either sign (biased exponent 0, significand
// not 0).
double random_subnormal(Lcg& lcg) {
    const std::uint64_t pattern = lcg.bits();
    std::uint64_t significand = pattern & 0x000fffffffffffffULL;
    if (significand == 0) significand = 1;
    const double result = std::bit_cast<double>((pattern & 0x8000000000000000ULL) | significand);
    return result;
}

void flush_rule(const bool exhaustive) {
    // Floats, one argument: every bit pattern (stride 1) or a stride, and
    // every subnormal pattern at a finer stride in the ctest run.
    const std::uint64_t float_stride = exhaustive ? 1 : 997;
    const std::uint64_t subnormal_stride = exhaustive ? 1 : 61;
    std::uint64_t float_checked = 0;
    for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += float_stride) {
        const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
        for (const FlushUnary& unary : flush_unary) check_unary_float(unary, x);
        check_sincos_float(x);
        float_checked += 1;
    }
    std::uint64_t subnormal_checked = 0;
    for (std::uint64_t significand = 1; significand < 0x800000ULL; significand += subnormal_stride) {
        const std::uint32_t signs[2] = {0U, 0x80000000U};
        for (const std::uint32_t sign : signs) {
            const float x = std::bit_cast<float>(sign | static_cast<std::uint32_t>(significand));
            for (const FlushUnary& unary : flush_unary) check_unary_float(unary, x);
            check_sincos_float(x);
            for (const FlushBinary& binary : flush_binary) {
                for (const double partner : flush_partners) {
                    const float other = static_cast<float>(partner);
                    check_binary_float(binary, x, other);
                    check_binary_float(binary, other, x);
                }
            }
            subnormal_checked += 1;
        }
    }
    // Floats, two arguments: a stride through every pattern against the
    // partners, both ways round.
    const std::uint64_t binary_stride = exhaustive ? 4099 : 65537;
    for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += binary_stride) {
        const float x = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
        for (const FlushBinary& binary : flush_binary) {
            for (const double partner : flush_partners) {
                const float other = static_cast<float>(partner);
                check_binary_float(binary, x, other);
                check_binary_float(binary, other, x);
            }
        }
    }
    // Doubles: random bit patterns (every exponent), random subnormals, and
    // the arguments whose results fall in the subnormal range (exp below
    // ln(2^-1022); pow with results there, both signs of x).
    const int count = exhaustive ? 20000000 : 100000;
    Lcg lcg{3008};
    for (int i = 0; i < count; ++i) {
        const double any = std::bit_cast<double>(lcg.bits());
        const double tiny = random_subnormal(lcg);
        const std::size_t partner_count = sizeof(flush_partners) / sizeof(flush_partners[0]);
        const double partner = flush_partners[static_cast<std::size_t>(lcg.unit() * static_cast<double>(partner_count))];
        const double other = std::bit_cast<double>(lcg.bits());
        for (const FlushUnary& unary : flush_unary) {
            check_unary_double(unary, any);
            check_unary_double(unary, tiny);
        }
        check_sincos_double(any);
        check_sincos_double(tiny);
        for (const FlushBinary& binary : flush_binary) {
            check_binary_double(binary, any, other);
            check_binary_double(binary, tiny, partner);
            check_binary_double(binary, partner, tiny);
            check_binary_double(binary, any, partner);
            check_binary_double(binary, partner, any);
        }
        const double underflowing = -746.0 + 37.7 * lcg.unit();
        check_unary_double(flush_unary[0], underflowing);
        const double base = 1.0 + lcg.unit();
        const double power = -1.0 * (708.0 + 37.5 * lcg.unit()) / unflushed::log(base);
        check_binary_double(flush_binary[0], base, power);
        check_binary_double(flush_binary[0], -base, std::floor(power));
        check_binary_double(flush_binary[2], std::ldexp(1.0 + lcg.unit(), -1000), std::ldexp(1.0 + lcg.unit(), 100));
    }
    std::printf("subnormal rule  %llu float patterns, %llu subnormal floats, %d rounds of 51 double cases: exact\n",
                static_cast<unsigned long long>(float_checked),
                static_cast<unsigned long long>(subnormal_checked), count);
}

// The subnormal outcomes math.hpp states, one by one.
void public_special_values() {
    const double infinity = std::numeric_limits<double>::infinity();
    const double pi = 3.141592653589793;
    double s = 0.0;
    double c = 0.0;
    stxlib::math::sincos(0x1p-1074, s, c);
    require(same_bits(s, 0.0) && same_bits(c, 1.0), "sincos(double): a subnormal angle is not (+0, 1)");
    stxlib::math::sincos(-0x1p-1030, s, c);
    require(same_bits(s, -0.0) && same_bits(c, 1.0), "sincos(double): a subnormal angle is not (-0, 1)");
    float sf = 0.0f;
    float cf = 0.0f;
    stxlib::math::sincos(-0x1p-149f, sf, cf);
    require(same_bits(sf, -0.0f) && same_bits(cf, 1.0f), "sincos(float): a subnormal angle is not (-0, 1)");
    require(same_bits(stxlib::math::exp(-745.13321910194), 0.0) &&
                unflushed::exp(-745.13321910194) == 0x1p-1074,
            "exp(double): the smallest subnormal result is not flushed to +0");
    require(same_bits(stxlib::math::exp(0x1p-1074), 1.0) && same_bits(stxlib::math::exp(-0x1p-1074), 1.0),
            "exp(double): a subnormal argument does not give 1");
    require(same_bits(stxlib::math::exp(-100.0f), 0.0f) && stxlib::math::exp(-87.0f) >= 0x1p-126f,
            "exp(float): the subnormal range is not flushed to +0");
    require(same_bits(stxlib::math::log(0x1p-1074), -infinity) &&
                same_bits(stxlib::math::log(-0x1p-1074), -infinity) &&
                same_bits(stxlib::math::log2(0x1p-1030), -infinity) &&
                same_bits(stxlib::math::log10(-0x0.fffffffffffffp-1022), -infinity) &&
                std::isinf(stxlib::math::log(0x1p-149f)) && stxlib::math::log(0x1p-149f) < 0.0f,
            "log: a subnormal argument does not give -infinity");
    require(same_bits(stxlib::math::pow(0x1p-1074, -1.0), infinity) &&
                same_bits(stxlib::math::pow(-0x1p-1074, -1.0), -infinity) &&
                same_bits(stxlib::math::pow(2.0, 0x1p-1074), 1.0) &&
                same_bits(stxlib::math::pow(0.5, 1074.0), 0.0) && unflushed::pow(0.5, 1074.0) == 0x1p-1074 &&
                same_bits(stxlib::math::pow(-0.5, 1073.0), -0.0) &&
                stxlib::math::pow(0.5, 1022.0) == 0x1p-1022 &&
                same_bits(stxlib::math::pow(0.5f, 140.0f), 0.0f),
            "pow: the subnormal rule's cases");
    require(same_bits(stxlib::math::hypot(0x1p-1074, -0x1p-1060), 0.0) &&
                same_bits(stxlib::math::hypot(3.0, 0x1p-1074), 3.0),
            "hypot: subnormal operands are not read as zero");
    require(same_bits(stxlib::math::atan(-0x1p-1074), -0.0) && same_bits(stxlib::math::asin(0x1p-1074), 0.0) &&
                same_bits(stxlib::math::acos(0x1p-1074), stxlib::math::acos(0.0)) &&
                same_bits(stxlib::math::atan2(0x1p-1000, 0x1p100), 0.0) &&
                stxlib::math::atan2(-0x1p-1074, -1.0) == -pi &&
                same_bits(stxlib::math::atan2(-0x1p-1074, -1.0), stxlib::math::atan2(-0.0, -1.0)),
            "inverse trigonometric: the subnormal rule's cases");
    require(same_bits(stxlib::math::tan(-0x1p-1074), -0.0) && same_bits(stxlib::math::tan(0x1p-1022), 0x1p-1022),
            "tan(double): the subnormal rule's cases");
    require(same_bits(stxlib::math::cbrt(0x1p-1074), 0.0) && same_bits(stxlib::math::cbrt(-0x1p-1074), -0.0) &&
                unflushed::cbrt(0x1p-1074) == 0x1p-358,
            "cbrt(double): a subnormal argument does not give its zero");
    require(same_bits(stxlib::math::sinh(0x1p-1074), 0.0) && same_bits(stxlib::math::tanh(-0x1p-1074), -0.0) &&
                same_bits(stxlib::math::asinh(0x1p-1074), 0.0) &&
                same_bits(stxlib::math::atanh(-0x1p-1074), -0.0) &&
                same_bits(stxlib::math::cosh(0x1p-1074), 1.0) && std::isnan(stxlib::math::acosh(0x1p-1074)) &&
                same_bits(stxlib::math::sinh(0x1p-1022), 0x1p-1022),
            "hyperbolic: the subnormal rule's cases");
    std::printf("subnormal rule  stated cases hold\n");
}

// ---- --bench ----------------------------------------------------------------

struct BenchBuffers final {
    std::vector<double> x{};
    std::vector<float> xf{};
    std::vector<double> s{};
    std::vector<double> c{};
    std::vector<float> sf{};
    std::vector<float> cf{};
};

void library_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) {
        b.s[i] = std::sin(b.x[i]);
        b.c[i] = std::cos(b.x[i]);
    }
}

void owned_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) stxlib::math::sincos(b.x[i], b.s[i], b.c[i]);
}

void library_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) {
        b.sf[i] = std::sin(b.xf[i]);
        b.cf[i] = std::cos(b.xf[i]);
    }
}

void owned_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) stxlib::math::sincos(b.xf[i], b.sf[i], b.cf[i]);
}

void library_exp_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::exp(b.x[i]);
}

void owned_exp_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stxlib::math::exp(b.x[i]);
}

void library_exp_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::exp(b.xf[i]);
}

void owned_exp_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stxlib::math::exp(b.xf[i]);
}

void library_log_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::log(b.x[i]);
}

void owned_log_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stxlib::math::log(b.x[i]);
}

void library_log_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::log(b.xf[i]);
}

void owned_log_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stxlib::math::log(b.xf[i]);
}

// pow takes x from the region and y from a fixed spread (stored in c / cf).
void library_pow_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::pow(b.x[i], b.c[i]);
}

void owned_pow_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stxlib::math::pow(b.x[i], b.c[i]);
}

void library_pow_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::pow(b.xf[i], b.cf[i]);
}

void owned_pow_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stxlib::math::pow(b.xf[i], b.cf[i]);
}

void library_hypot_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::hypot(b.x[i], b.c[i]);
}

void owned_hypot_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stxlib::math::hypot(b.x[i], b.c[i]);
}

void library_hypot_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::hypot(b.xf[i], b.cf[i]);
}

void owned_hypot_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stxlib::math::hypot(b.xf[i], b.cf[i]);
}

using BenchKernel = void (*)(BenchBuffers& buffers);

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

const Region log_bench_regions[] = {
    {"[1e-300, 1e300]", 1e-300, 1e300},
    {"[0.5, 2]", 0.5, 2.0},
    {"1 +- 1e-3", 0.999, 1.001},
};

const Region pow_bench_regions[] = {
    {"x in [0.5, 2]", 0.5, 2.0},
    {"x in [0, 100]", 0.0, 100.0},
};

struct BenchSet final {
    const char* title{nullptr};
    BenchKernel library_double{nullptr};
    BenchKernel owned_double{nullptr};
    BenchKernel library_float{nullptr};
    BenchKernel owned_float{nullptr};
    const Region* regions{nullptr};
    std::size_t region_count{0};
};

// The owner's ceiling: no owned function may take more than twice as long
// as the C library's on the same inputs.
constexpr double speed_ceiling = 2.0;

// Prints one set's table with owned/library ratios; returns the largest
// ratio among gated regions.
double bench_set(const BenchSet& set) {
    const std::size_t count = std::size_t{1} << 20;
    std::printf("\n%s, ns per call, %zu inputs, best of 15\n", set.title, count);
    std::printf("%-16s %10s %10s %7s %10s %10s %7s\n", "region", "std dbl", "owned dbl", "ratio",
                "std flt", "owned flt", "ratio");
    double worst = 0.0;
    for (std::size_t r = 0; r < set.region_count; ++r) {
        const Region& region = set.regions[r];
        BenchBuffers b{};
        Lcg lcg{12345};
        b.x.resize(count);
        b.xf.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            b.x[i] = region.low + (region.high - region.low) * lcg.unit();
            b.xf[i] = static_cast<float>(b.x[i]);
        }
        b.s.resize(count);
        b.c.resize(count);
        b.sf.resize(count);
        b.cf.resize(count);
        // Second operand for pow, hypot and atan2: in [-8, 8].
        for (std::size_t i = 0; i < count; ++i) {
            b.c[i] = (lcg.unit() - 0.5) * 16.0;
            b.cf[i] = static_cast<float>(b.c[i]);
        }
        const double std_double = best_ns_per_call(set.library_double, b);
        const double our_double = best_ns_per_call(set.owned_double, b);
        const double std_float = best_ns_per_call(set.library_float, b);
        const double our_float = best_ns_per_call(set.owned_float, b);
        const double double_ratio = our_double / std_double;
        const double float_ratio = our_float / std_float;
        std::printf("%-16s %10.3f %10.3f %7.2f %10.3f %10.3f %7.2f%s\n", region.name, std_double,
                    our_double, double_ratio, std_float, our_float, float_ratio,
                    region.gated ? "" : "  (not gated)");
        if (region.gated) worst = std::max(worst, std::max(double_ratio, float_ratio));
    }
    return worst;
}

// Unary kernels by template parameter, so each loop calls its function
// directly.
template <double (*Function)(double)>
void unary_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = Function(b.x[i]);
}

template <float (*Function)(float)>
void unary_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = Function(b.xf[i]);
}

float library_atan_f(const float x) { return std::atan(x); }
float library_asin_f(const float x) { return std::asin(x); }
float library_acos_f(const float x) { return std::acos(x); }
float library_tan_f(const float x) { return std::tan(x); }
float library_cbrt_f(const float x) { return std::cbrt(x); }
float library_sinh_f(const float x) { return std::sinh(x); }
float library_cosh_f(const float x) { return std::cosh(x); }
float library_tanh_f(const float x) { return std::tanh(x); }
float library_asinh_f(const float x) { return std::asinh(x); }
float library_acosh_f(const float x) { return std::acosh(x); }
float library_atanh_f(const float x) { return std::atanh(x); }
float library_log2_f(const float x) { return std::log2(x); }
float library_log10_f(const float x) { return std::log10(x); }

void library_atan2_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = std::atan2(b.x[i], b.c[i]);
}

void owned_atan2_double(BenchBuffers& b) {
    const std::size_t count = b.x.size();
    for (std::size_t i = 0; i < count; ++i) b.s[i] = stxlib::math::atan2(b.x[i], b.c[i]);
}

void library_atan2_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = std::atan2(b.xf[i], b.cf[i]);
}

void owned_atan2_float(BenchBuffers& b) {
    const std::size_t count = b.xf.size();
    for (std::size_t i = 0; i < count; ++i) b.sf[i] = stxlib::math::atan2(b.xf[i], b.cf[i]);
}

const Region signed_ten[] = {{"[-10, 10]", -10.0, 10.0}};
const Region unit_interval[] = {{"[-1, 1]", -1.0, 1.0}};
const Region open_unit[] = {{"[-0.99, 0.99]", -0.99, 0.99}};
const Region tan_region[] = {{"[-1.5, 1.5]", -1.5, 1.5}};
const Region cbrt_region[] = {{"[0, 100]", 0.0, 100.0}};
const Region hyperbolic_region[] = {{"[-5, 5]", -5.0, 5.0}};
const Region tanh_region[] = {{"[-3, 3]", -3.0, 3.0}};
const Region acosh_region[] = {{"[1, 10]", 1.0, 10.0}};
const Region positive_region[] = {{"[0.5, 2]", 0.5, 2.0}, {"[1e-3, 1e3]", 1e-3, 1e3}};

double bench() {
    const BenchSet sets[] = {
        {"sincos (sin + cos pair)", library_double, owned_double, library_float, owned_float,
         double_regions, sizeof(double_regions) / sizeof(double_regions[0])},
        {"exp", library_exp_double, owned_exp_double, library_exp_float, owned_exp_float,
         exp_regions, sizeof(exp_regions) / sizeof(exp_regions[0])},
        {"log", library_log_double, owned_log_double, library_log_float, owned_log_float,
         log_bench_regions, sizeof(log_bench_regions) / sizeof(log_bench_regions[0])},
        {"log2", unary_double<library_log2_d>, unary_double<owned_log2_d>, unary_float<library_log2_f>,
         unary_float<owned_log2_f>, positive_region, 2},
        {"log10", unary_double<library_log10_d>, unary_double<owned_log10_d>,
         unary_float<library_log10_f>, unary_float<owned_log10_f>, positive_region, 2},
        {"pow (y in [-8, 8])", library_pow_double, owned_pow_double, library_pow_float,
         owned_pow_float, pow_bench_regions, sizeof(pow_bench_regions) / sizeof(pow_bench_regions[0])},
        {"hypot (second operand in [-8, 8])", library_hypot_double, owned_hypot_double,
         library_hypot_float, owned_hypot_float, pow_bench_regions,
         sizeof(pow_bench_regions) / sizeof(pow_bench_regions[0])},
        {"atan", unary_double<library_atan_d>, unary_double<owned_atan_d>, unary_float<library_atan_f>,
         unary_float<owned_atan_f>, signed_ten, 1},
        {"atan2 (x in [-8, 8])", library_atan2_double, owned_atan2_double, library_atan2_float,
         owned_atan2_float, signed_ten, 1},
        {"asin", unary_double<library_asin_d>, unary_double<owned_asin_d>, unary_float<library_asin_f>,
         unary_float<owned_asin_f>, unit_interval, 1},
        {"acos", unary_double<library_acos_d>, unary_double<owned_acos_d>, unary_float<library_acos_f>,
         unary_float<owned_acos_f>, unit_interval, 1},
        {"tan", unary_double<library_tan_d>, unary_double<owned_tan_d>, unary_float<library_tan_f>,
         unary_float<owned_tan_f>, tan_region, 1},
        {"cbrt", unary_double<library_cbrt_d>, unary_double<owned_cbrt_d>, unary_float<library_cbrt_f>,
         unary_float<owned_cbrt_f>, cbrt_region, 1},
        {"sinh", unary_double<library_sinh_d>, unary_double<owned_sinh_d>, unary_float<library_sinh_f>,
         unary_float<owned_sinh_f>, hyperbolic_region, 1},
        {"cosh", unary_double<library_cosh_d>, unary_double<owned_cosh_d>, unary_float<library_cosh_f>,
         unary_float<owned_cosh_f>, hyperbolic_region, 1},
        {"tanh", unary_double<library_tanh_d>, unary_double<owned_tanh_d>, unary_float<library_tanh_f>,
         unary_float<owned_tanh_f>, tanh_region, 1},
        {"asinh", unary_double<library_asinh_d>, unary_double<owned_asinh_d>,
         unary_float<library_asinh_f>, unary_float<owned_asinh_f>, signed_ten, 1},
        {"acosh", unary_double<library_acosh_d>, unary_double<owned_acosh_d>,
         unary_float<library_acosh_f>, unary_float<owned_acosh_f>, acosh_region, 1},
        {"atanh", unary_double<library_atanh_d>, unary_double<owned_atanh_d>,
         unary_float<library_atanh_f>, unary_float<owned_atanh_f>, open_unit, 1},
    };
    double worst = 0.0;
    for (const BenchSet& set : sets) worst = std::max(worst, bench_set(set));
    std::printf("\nworst gated ratio %.2f (ceiling %.1f)\n", worst, speed_ceiling);
    return worst;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    const bool exhaustive = argument_count == 2 && std::strcmp(arguments[1], "--exhaustive") == 0;
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench") == 0) {
        (void)bench();
        return 0;
    }
    if (argument_count == 2 && std::strcmp(arguments[1], "--bench-gate") == 0) {
        const double worst = bench();
        if (worst > speed_ceiling) {
            std::fprintf(stderr, "math speed gate failed: %.2fx the C library\n", worst);
            return 1;
        }
        std::puts("math speed gate passed");
        return 0;
    }
    try {
        check_reference();
        special_values();
        determinism();
        sincos_certified_worst_cases();
        double_accuracy(exhaustive ? 20000000 : 200000);
        sincos_hard_reduction(exhaustive ? 20000000 : 200000);
        float_exceptions();
        float_rounding(exhaustive ? 1 : 97);
        exp_check_reference();
        exp_special_values();
        exp_determinism();
        exp_double_accuracy(exhaustive ? 20000000 : 200000);
        exp_double_outside(exhaustive ? 20000000 : 200000);
        exp_float_rounding(exhaustive ? 1 : 97);
        const std::vector<LogKernel> logs = log_kernels();
        log_special_values(logs);
        log_determinism(logs);
        log_double_accuracy(logs, exhaustive ? 20000000 : 100000);
        log_float_rounding(logs, exhaustive ? 1 : 997);
        pow_special_values();
        pow_kernel_y_range();
        pow_determinism();
        pow_double_accuracy(exhaustive ? 20000000 : 100000);
        pow_float_rounding(exhaustive ? 100000000 : 200000);
        hypot_special_values();
        hypot_determinism();
        hypot_double_accuracy(exhaustive ? 20000000 : 100000);
        hypot_float_rounding(exhaustive ? 100000000 : 300000);
        inverse_trig_special_values();
        inverse_trig_determinism();
        inverse_trig_double_accuracy(exhaustive ? 20000000 : 100000);
        inverse_trig_float_rounding(exhaustive ? 1 : 997);
        remaining_special_values();
        remaining_determinism();
        remaining_double_accuracy(exhaustive ? 5000000 : 50000);
        remaining_float_rounding(exhaustive ? 1 : 1999);
        public_special_values();
        flush_rule(exhaustive);
        std::printf("float screens: %llu misrounded floats\n", static_cast<unsigned long long>(misrounded_floats));
        require(misrounded_floats == 0, "float screens: results not correctly rounded (listed on stderr)");
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "math tests failed: %s\n", failure.what());
        return 1;
    }
    std::puts("math tests passed");
    return 0;
}
