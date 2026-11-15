// stxlib_bit_identity: the cross-host bit-identity check
// (planning/stages/A1-float-discipline.md, section 3, "Cross-host bit
// identity in CI"; F1 = 1 and F5 = 1, 2026-11-13). It generalises the math
// tests' frozen output hashes to every module with promised floating-point
// results: each module hashes its outputs over fixed inputs, and every host,
// SIMD level and product form must print the same line.
//
// Output, one line per item: `<item> <hash, 16 hex digits> <values hashed>`,
// after `#` lines that say what ran (level, product form, math build). The
// hash is FNV-1a 64 over each value's bits as 8 little-endian bytes (a
// float's 32 bits widened), so it does not depend on the host's byte order.
// A NaN result is hashed as one canonical NaN: the hardware's NaN sign and
// payload differ between architectures (x86-64's default NaN is negative,
// arm64's positive), and no STXlib contract promises them. Text is hashed
// as its length and bytes.
//
// tools/compare_bit_identity.py runs this program and compares its lines
// with tests/bit_identity_expected.txt; an item frozen there must match
// exactly. The inputs are fixed: an item's inputs never change once frozen
// (a new input set is a new item).

#include "core/float_exact.hpp"
#include "core/float_reduce.hpp"

#include "core/checked.hpp"
#include "algorithms/math/paths.hpp"
#include "stxlib/math.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/random.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/text.hpp"
#include "stxlib/thread_pool.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <utility>
#include <vector>

namespace {

namespace fe = stxlib::float_exact;
namespace fr = stxlib::float_reduce;
namespace math = stxlib::math;
namespace text = stxlib::text;
using stxlib::usize;

// ---- Hashing --------------------------------------------------------------------

struct Digest final {
    std::uint64_t state{0xcbf29ce484222325ULL};
    std::uint64_t values{0};

    // FNV-1a: arithmetic modulo 2^64 by design.
    STXLIB_MODULAR void byte(const std::uint64_t value) {
        state = (state ^ (value & 0xffU)) * 0x100000001b3ULL;
    }
    void bytes_of(const std::uint64_t word) {
        for (int index = 0; index < 8; ++index) byte(word >> (8 * index));
    }
    void word(const std::uint64_t value) {
        bytes_of(value);
        values = values + 1;
    }
    void real(const double value) {
        const std::uint64_t bits = std::isnan(value) ? 0x7ff8000000000000ULL : std::bit_cast<std::uint64_t>(value);
        word(bits);
    }
    void real(const float value) {
        const std::uint32_t bits = std::isnan(value) ? 0x7fc00000U : std::bit_cast<std::uint32_t>(value);
        word(bits);
    }
    void sign(const int value) { word(static_cast<std::uint64_t>(static_cast<std::int64_t>(value))); }
    void bytes(const char* const first, const usize count) {
        bytes_of(count);
        for (usize index = 0; index < count; ++index) byte(static_cast<unsigned char>(first[index]));
        values = values + 1;
    }
};

void print(const char* const item, const Digest& digest) {
    std::printf("%s %016llx %llu\n", item, static_cast<unsigned long long>(digest.state),
                static_cast<unsigned long long>(digest.values));
}

// ---- Inputs ---------------------------------------------------------------------

struct Lcg final {
    std::uint64_t state{0};

    // A 64-bit linear congruential step: arithmetic modulo 2^64 by design.
    STXLIB_MODULAR std::uint64_t step() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t result = state;
        return result;
    }
    // 64 bits from the high halves of two steps.
    std::uint64_t bits() {
        const std::uint64_t high = step() >> 32;
        const std::uint64_t low = step() >> 32;
        const std::uint64_t result = (high << 32) | low;
        return result;
    }
    double unit() {
        const double result = static_cast<double>(step() >> 11) * 0x1.0p-53;
        return result;
    }
};

// The next double toward `direction`, kept in the predicates' range: a zero
// moves to 2^-40 of that sign instead of to a subnormal.
double perturbed(const double value, const double direction) {
    if (value == 0.0) return direction > 0.0 ? 0x1.0p-40 : -0x1.0p-40;
    const double result = std::nextafter(value, direction);
    return result;
}

double between(Lcg& generator, const double low, const double high) {
    const double result = low + (high - low) * generator.unit();
    return result;
}

// A double with a random sign and significand and an exponent in
// [low, high]; ldexp is exact for a normal result.
double with_exponent(Lcg& generator, const int low, const int high) {
    const std::uint64_t bits = generator.bits();
    const double significand = 1.0 + static_cast<double>(bits >> 12) * 0x1.0p-52;
    const int span = high - low + 1;
    const int exponent = low + static_cast<int>((bits >> 1) % static_cast<std::uint64_t>(span));
    const double magnitude = std::ldexp(significand, exponent);
    const double result = (bits & 1) != 0 ? -magnitude : magnitude;
    return result;
}

float float_with_exponent(Lcg& generator, const int low, const int high) {
    const std::uint64_t bits = generator.bits();
    const float significand = 1.0f + static_cast<float>(bits >> 41) * 0x1.0p-23f;
    const int span = high - low + 1;
    const int exponent = low + static_cast<int>((bits >> 1) % static_cast<std::uint64_t>(span));
    const float magnitude = std::ldexp(significand, exponent);
    const float result = (bits & 1) != 0 ? -magnitude : magnitude;
    return result;
}

std::vector<double> double_inputs(const std::uint64_t seed) {
    const double specials[] = {0.0,
                               -0.0,
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(),
                               1.0,
                               -1.0,
                               0.5,
                               -0.5,
                               2.0,
                               std::numeric_limits<double>::min(),
                               -std::numeric_limits<double>::min(),
                               std::numeric_limits<double>::denorm_min(),
                               -std::numeric_limits<double>::denorm_min(),
                               0x1.fffffffffffffp-1023,
                               std::numeric_limits<double>::max(),
                               -std::numeric_limits<double>::max(),
                               1e-300,
                               1e300,
                               3.141592653589793,
                               709.78,
                               -745.13,
                               1e22,
                               0x1.0p-30};
    std::vector<double> result(specials, specials + sizeof(specials) / sizeof(specials[0]));
    Lcg generator{seed};
    for (int index = 0; index < 2048; ++index) result.push_back(std::bit_cast<double>(generator.bits()));
    for (int index = 0; index < 1024; ++index) result.push_back(between(generator, -1.0, 1.0));
    for (int index = 0; index < 1024; ++index) result.push_back(between(generator, -50.0, 50.0));
    for (int index = 0; index < 1024; ++index) result.push_back(between(generator, -1e6, 1e6));
    return result;
}

std::vector<float> float_inputs(const std::uint64_t seed) {
    const float specials[] = {0.0f,
                              -0.0f,
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN(),
                              1.0f,
                              -1.0f,
                              0.5f,
                              -0.5f,
                              2.0f,
                              std::numeric_limits<float>::min(),
                              -std::numeric_limits<float>::min(),
                              std::numeric_limits<float>::denorm_min(),
                              -std::numeric_limits<float>::denorm_min(),
                              std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max(),
                              3.1415927f,
                              88.72f,
                              -103.97f,
                              1e-30f};
    std::vector<float> result(specials, specials + sizeof(specials) / sizeof(specials[0]));
    Lcg generator{seed};
    for (int index = 0; index < 2048; ++index) {
        result.push_back(std::bit_cast<float>(static_cast<std::uint32_t>(generator.bits() >> 32)));
    }
    for (int index = 0; index < 1024; ++index) result.push_back(static_cast<float>(between(generator, -1.0, 1.0)));
    for (int index = 0; index < 1024; ++index) result.push_back(static_cast<float>(between(generator, -50.0, 50.0)));
    for (int index = 0; index < 1024; ++index) result.push_back(static_cast<float>(between(generator, -1e6, 1e6)));
    return result;
}

// ---- Math -----------------------------------------------------------------------

// The functions with two kernel paths (src/algorithms/math/paths.hpp) print
// under the path the build's switches choose: `<item>` for today's kernel,
// `<item>@region` for the region kernel. Each path has its own frozen line;
// tools/compare_bit_identity.py accepts either path of an item.
#if STXLIB_MATH_REGION_POW_DOUBLE
#define STXLIB_POW_DOUBLE_ITEM "math.pow.double@region"
#else
#define STXLIB_POW_DOUBLE_ITEM "math.pow.double"
#endif
#if STXLIB_MATH_REGION_POW_FLOAT
#define STXLIB_POW_FLOAT_ITEM "math.pow.float@region"
#else
#define STXLIB_POW_FLOAT_ITEM "math.pow.float"
#endif
#if STXLIB_MATH_REGION_LOG_FLOAT
#define STXLIB_LOG_FLOAT_PATH "@region"
#else
#define STXLIB_LOG_FLOAT_PATH ""
#endif
#if STXLIB_MATH_REGION_HYPOT_FLOAT
#define STXLIB_HYPOT_FLOAT_ITEM "math.hypot.float@region"
#else
#define STXLIB_HYPOT_FLOAT_ITEM "math.hypot.float"
#endif
#if STXLIB_MATH_REGION_TRIG_DOUBLE
#define STXLIB_TRIG_DOUBLE_PATH "@region"
#else
#define STXLIB_TRIG_DOUBLE_PATH ""
#endif

struct UnaryDouble final {
    const char* item;
    double (*function)(double) noexcept;
};
struct UnaryFloat final {
    const char* item;
    float (*function)(float) noexcept;
};
struct BinaryDouble final {
    const char* item;
    double (*function)(double, double) noexcept;
};
struct BinaryFloat final {
    const char* item;
    float (*function)(float, float) noexcept;
};

typedef double (*DoubleFunction)(double) noexcept;
typedef float (*FloatFunction)(float) noexcept;
typedef double (*DoublePair)(double, double) noexcept;
typedef float (*FloatPair)(float, float) noexcept;

void math_items() {
    const std::vector<double> doubles = double_inputs(0x6d617468);
    const std::vector<float> floats = float_inputs(0x666c6f61);

    const UnaryDouble unary_doubles[] = {
        {"math.exp.double", static_cast<DoubleFunction>(&math::exp)},
        {"math.log.double", static_cast<DoubleFunction>(&math::log)},
        {"math.log2.double", static_cast<DoubleFunction>(&math::log2)},
        {"math.log10.double", static_cast<DoubleFunction>(&math::log10)},
        {"math.atan.double", static_cast<DoubleFunction>(&math::atan)},
        {"math.asin.double", static_cast<DoubleFunction>(&math::asin)},
        {"math.acos.double", static_cast<DoubleFunction>(&math::acos)},
        {"math.tan.double" STXLIB_TRIG_DOUBLE_PATH, static_cast<DoubleFunction>(&math::tan)},
        {"math.cbrt.double", static_cast<DoubleFunction>(&math::cbrt)},
        {"math.sinh.double", static_cast<DoubleFunction>(&math::sinh)},
        {"math.cosh.double", static_cast<DoubleFunction>(&math::cosh)},
        {"math.tanh.double", static_cast<DoubleFunction>(&math::tanh)},
        {"math.asinh.double", static_cast<DoubleFunction>(&math::asinh)},
        {"math.acosh.double", static_cast<DoubleFunction>(&math::acosh)},
        {"math.atanh.double", static_cast<DoubleFunction>(&math::atanh)},
        {"math.exp2.double", static_cast<DoubleFunction>(&math::exp2)},
        {"math.expm1.double", static_cast<DoubleFunction>(&math::expm1)},
        {"math.log1p.double", static_cast<DoubleFunction>(&math::log1p)},
        {"math.sin.double" STXLIB_TRIG_DOUBLE_PATH, static_cast<DoubleFunction>(&math::sin)},
        {"math.cos.double" STXLIB_TRIG_DOUBLE_PATH, static_cast<DoubleFunction>(&math::cos)},
    };
    for (const UnaryDouble& entry : unary_doubles) {
        Digest digest;
        for (const double input : doubles) digest.real(entry.function(input));
        print(entry.item, digest);
    }
    {
        Digest digest;
        for (const double input : doubles) {
            double sine = 0.0;
            double cosine = 0.0;
            math::sincos(input, sine, cosine);
            digest.real(sine);
            digest.real(cosine);
        }
        print("math.sincos.double" STXLIB_TRIG_DOUBLE_PATH, digest);
    }
    const BinaryDouble binary_doubles[] = {
        {STXLIB_POW_DOUBLE_ITEM, static_cast<DoublePair>(&math::pow)},
        {"math.hypot.double", static_cast<DoublePair>(&math::hypot)},
        {"math.atan2.double", static_cast<DoublePair>(&math::atan2)},
    };
    for (const BinaryDouble& entry : binary_doubles) {
        Digest digest;
        const usize count = doubles.size();
        for (usize index = 0; index < count; ++index) {
            digest.real(entry.function(doubles[index], doubles[(index * 7 + 3) % count]));
        }
        for (usize first = 0; first < 24; ++first) {
            for (usize second = 0; second < 24; ++second) digest.real(entry.function(doubles[first], doubles[second]));
        }
        print(entry.item, digest);
    }

    const UnaryFloat unary_floats[] = {
        {"math.exp.float", static_cast<FloatFunction>(&math::exp)},
        {"math.log.float" STXLIB_LOG_FLOAT_PATH, static_cast<FloatFunction>(&math::log)},
        {"math.log2.float" STXLIB_LOG_FLOAT_PATH, static_cast<FloatFunction>(&math::log2)},
        {"math.log10.float" STXLIB_LOG_FLOAT_PATH, static_cast<FloatFunction>(&math::log10)},
        {"math.atan.float", static_cast<FloatFunction>(&math::atan)},
        {"math.asin.float", static_cast<FloatFunction>(&math::asin)},
        {"math.acos.float", static_cast<FloatFunction>(&math::acos)},
        {"math.tan.float", static_cast<FloatFunction>(&math::tan)},
        {"math.cbrt.float", static_cast<FloatFunction>(&math::cbrt)},
        {"math.sinh.float", static_cast<FloatFunction>(&math::sinh)},
        {"math.cosh.float", static_cast<FloatFunction>(&math::cosh)},
        {"math.tanh.float", static_cast<FloatFunction>(&math::tanh)},
        {"math.asinh.float", static_cast<FloatFunction>(&math::asinh)},
        {"math.acosh.float", static_cast<FloatFunction>(&math::acosh)},
        {"math.atanh.float", static_cast<FloatFunction>(&math::atanh)},
        {"math.exp2.float", static_cast<FloatFunction>(&math::exp2)},
        {"math.expm1.float", static_cast<FloatFunction>(&math::expm1)},
        {"math.log1p.float", static_cast<FloatFunction>(&math::log1p)},
        {"math.sin.float", static_cast<FloatFunction>(&math::sin)},
        {"math.cos.float", static_cast<FloatFunction>(&math::cos)},
    };
    for (const UnaryFloat& entry : unary_floats) {
        Digest digest;
        for (const float input : floats) digest.real(entry.function(input));
        print(entry.item, digest);
    }
    {
        Digest digest;
        for (const float input : floats) {
            float sine = 0.0f;
            float cosine = 0.0f;
            math::sincos(input, sine, cosine);
            digest.real(sine);
            digest.real(cosine);
        }
        print("math.sincos.float", digest);
    }
    const BinaryFloat binary_floats[] = {
        {STXLIB_POW_FLOAT_ITEM, static_cast<FloatPair>(&math::pow)},
        {STXLIB_HYPOT_FLOAT_ITEM, static_cast<FloatPair>(&math::hypot)},
        {"math.atan2.float", static_cast<FloatPair>(&math::atan2)},
    };
    for (const BinaryFloat& entry : binary_floats) {
        Digest digest;
        const usize count = floats.size();
        for (usize index = 0; index < count; ++index) {
            digest.real(entry.function(floats[index], floats[(index * 7 + 3) % count]));
        }
        for (usize first = 0; first < 20; ++first) {
            for (usize second = 0; second < 20; ++second) digest.real(entry.function(floats[first], floats[second]));
        }
        print(entry.item, digest);
    }
}

// ---- Exact primitives and predicates --------------------------------------------

void exact_items() {
    {
        Lcg generator{0x73756d73};
        Digest sums;
        Digest products;
        Digest splits;
        for (int index = 0; index < 20000; ++index) {
            const double a = with_exponent(generator, -400, 400);
            const double b = with_exponent(generator, -400, 400);
            const fe::Pair<double> sum = fe::two_sum(a, b);
            const double larger = std::fabs(a) >= std::fabs(b) ? a : b;
            const double smaller = std::fabs(a) >= std::fabs(b) ? b : a;
            const fe::Pair<double> fast = fe::fast_two_sum(larger, smaller);
            const fe::Pair<double> product = fe::two_product(a, b);
            const fe::Pair<double> parts = fe::split(a);
            sums.real(sum.value);
            sums.real(sum.error);
            sums.real(fast.value);
            sums.real(fast.error);
            products.real(product.value);
            products.real(product.error);
            splits.real(parts.value);
            splits.real(parts.error);
        }
        print("exact.two_sum.double", sums);
        print("exact.two_product.double", products);
        print("exact.split.double", splits);
    }
    {
        Lcg generator{0x666c7473};
        Digest sums;
        Digest products;
        for (int index = 0; index < 20000; ++index) {
            const float a = float_with_exponent(generator, -40, 40);
            const float b = float_with_exponent(generator, -40, 40);
            const fe::Pair<float> sum = fe::two_sum(a, b);
            const fe::Pair<float> product = fe::two_product(a, b);
            sums.real(sum.value);
            sums.real(sum.error);
            products.real(product.value);
            products.real(product.error);
        }
        print("exact.two_sum.float", sums);
        print("exact.two_product.float", products);
    }
    {
        // Expansions: (a*b + c*d) * g exactly, component by component.
        Lcg generator{0x65787061};
        Digest digest;
        for (int index = 0; index < 5000; ++index) {
            const double a = with_exponent(generator, -200, 200);
            const double b = with_exponent(generator, -200, 200);
            const double c = with_exponent(generator, -200, 200);
            const double d = with_exponent(generator, -200, 200);
            const double g = with_exponent(generator, -200, 200);
            const fe::Pair<double> left = fe::two_product(a, b);
            const fe::Pair<double> right = fe::two_product(c, d);
            const double e[2] = {left.error, left.value};
            const double f[2] = {right.error, right.value};
            double h[4] = {};
            const usize h_count = fe::expansion_sum(e, 2, f, 2, h);
            double scaled[8] = {};
            const usize scaled_count = fe::scale_expansion(h, h_count, g, scaled);
            digest.word(h_count);
            for (usize component = 0; component < h_count; ++component) digest.real(h[component]);
            digest.word(scaled_count);
            for (usize component = 0; component < scaled_count; ++component) digest.real(scaled[component]);
            digest.sign(fe::expansion_sign(scaled, scaled_count));
        }
        print("exact.expansions.double", digest);
    }
    {
        Lcg generator{0x636f6d70};
        Digest digest;
        for (int round = 0; round < 50; ++round) {
            fe::KahanSum<double> kahan;
            fe::NeumaierSum<double> neumaier;
            for (int index = 0; index < 1000; ++index) {
                const double term = with_exponent(generator, -60, 60);
                kahan.add(term);
                neumaier.add(term);
            }
            digest.real(kahan.value());
            digest.real(neumaier.value());
        }
        print("exact.compensated.double", digest);
    }
    {
        // Points on a 2^-20 grid in [-1024, 1024], then exactly collinear
        // and exactly cocircular sets and their one-ulp perturbations, where
        // the filter cannot decide and the exact stage answers.
        Lcg generator{0x70726564};
        Digest orient;
        Digest circle;
        Digest orient_float;
        Digest circle_float;
        for (int index = 0; index < 20000; ++index) {
            double point[8];
            for (double& coordinate : point) {
                coordinate = static_cast<double>(static_cast<std::int64_t>(generator.bits() >> 33) - (1LL << 30)) *
                             0x1.0p-20;
            }
            orient.sign(fe::orient2d(point[0], point[1], point[2], point[3], point[4], point[5]));
            circle.sign(fe::incircle(point[0], point[1], point[2], point[3], point[4], point[5], point[6], point[7]));
            // Collinear: c = a + t (b - a), t = k / 1024, exact on the grid.
            const double t = static_cast<double>(generator.step() >> 54) * 0x1.0p-10;
            const double cx = point[0] + t * (point[2] - point[0]);
            const double cy = point[1] + t * (point[3] - point[1]);
            orient.sign(fe::orient2d(point[0], point[1], point[2], point[3], cx, cy));
            orient.sign(fe::orient2d(point[0], point[1], point[2], point[3], perturbed(cx, 2048.0), cy));
            orient.sign(fe::orient2d(point[0], point[1], point[2], point[3], cx, perturbed(cy, -2048.0)));
            const float fpoint[6] = {static_cast<float>(point[0]), static_cast<float>(point[1]),
                                     static_cast<float>(point[2]), static_cast<float>(point[3]),
                                     static_cast<float>(point[4]), static_cast<float>(point[5])};
            orient_float.sign(fe::orient2d(fpoint[0], fpoint[1], fpoint[2], fpoint[3], fpoint[4], fpoint[5]));
            circle_float.sign(fe::incircle(fpoint[0], fpoint[1], fpoint[2], fpoint[3], fpoint[4], fpoint[5],
                                           static_cast<float>(point[6]), static_cast<float>(point[7])));
        }
        // Cocircular: integer points on x^2 + y^2 = 25, scaled and shifted
        // by grid values, and one-ulp perturbations of the fourth point.
        const double circle_points[12][2] = {{5, 0},  {4, 3},   {3, 4},   {0, 5},  {-3, 4}, {-4, 3},
                                             {-5, 0}, {-4, -3}, {-3, -4}, {0, -5}, {3, -4}, {4, -3}};
        for (int index = 0; index < 2000; ++index) {
            const double scale = static_cast<double>(1 + (generator.step() >> 50)) * 0x1.0p-8;
            const double shift_x = static_cast<double>(static_cast<std::int64_t>(generator.step() >> 44) - (1LL << 19)) *
                                   0x1.0p-10;
            const double shift_y = static_cast<double>(static_cast<std::int64_t>(generator.step() >> 44) - (1LL << 19)) *
                                   0x1.0p-10;
            usize chosen[4];
            for (usize& pick : chosen) pick = static_cast<usize>((generator.step() >> 33) % 12);
            double x[4];
            double y[4];
            for (usize corner = 0; corner < 4; ++corner) {
                x[corner] = circle_points[chosen[corner]][0] * scale + shift_x;
                y[corner] = circle_points[chosen[corner]][1] * scale + shift_y;
            }
            circle.sign(fe::incircle(x[0], y[0], x[1], y[1], x[2], y[2], x[3], y[3]));
            circle.sign(fe::incircle(x[0], y[0], x[1], y[1], x[2], y[2], perturbed(x[3], 4096.0), y[3]));
            circle.sign(fe::incircle(x[0], y[0], x[1], y[1], x[2], y[2], x[3], perturbed(y[3], -4096.0)));
        }
        print("exact.orient2d.double", orient);
        print("exact.incircle.double", circle);
        print("exact.orient2d.float", orient_float);
        print("exact.incircle.float", circle_float);
    }
}

// ---- Reductions -----------------------------------------------------------------

int reduce_items() {
    Lcg generator{0x72656475};
    const usize large = 40 * 4096 + 77;
    std::vector<double> left(large);
    std::vector<double> right(large);
    std::vector<float> narrow(large);
    for (usize index = 0; index < large; ++index) {
        left[index] = with_exponent(generator, -40, 40);
        right[index] = with_exponent(generator, -40, 40);
        narrow[index] = static_cast<float>(left[index]);
    }
    Digest doubles;
    Digest floats;
    for (usize count = 0; count <= 100; ++count) {
        doubles.real(fr::ordered_sum(left.data(), count));
        doubles.real(fr::ordered_dot(left.data(), right.data(), count));
        floats.real(fr::ordered_sum(narrow.data(), count));
        floats.real(fr::ordered_dot(narrow.data(), narrow.data() + 1, count));
    }
    const stxlib::View<double> left_view(left.data(), large);
    const stxlib::View<double> right_view(right.data(), large);
    const stxlib::View<float> narrow_view(narrow.data(), large);
    const double chunked_sum = fr::chunked_sum(left_view);
    const double chunked_dot = fr::chunked_dot(left_view, right_view);
    const float chunked_narrow = fr::chunked_sum(narrow_view);
    doubles.real(chunked_sum);
    doubles.real(chunked_dot);
    floats.real(chunked_narrow);
    print("reduce.ordered.double", doubles);
    print("reduce.ordered.float", floats);

    // The pool gives the serial chunked bits at 3 threads; a disagreement
    // is a failure on this host, whatever the frozen values say.
    stxlib::Result<stxlib::AtomicPool> created = stxlib::AtomicPool::create(3);
    if (!created.succeeded()) {
        std::printf("# reduce.pooled: no pool\n");
        return 1;
    }
    stxlib::AtomicPool pool = std::move(created).take_value();
    const stxlib::Result<double> pooled_sum = fr::pooled_sum(pool, left_view);
    const stxlib::Result<double> pooled_dot = fr::pooled_dot(pool, left_view, right_view);
    const stxlib::Result<float> pooled_narrow = fr::pooled_sum(pool, narrow_view);
    const bool agreed = pooled_sum.succeeded() && pooled_dot.succeeded() && pooled_narrow.succeeded() &&
                        std::bit_cast<std::uint64_t>(*pooled_sum.value()) == std::bit_cast<std::uint64_t>(chunked_sum) &&
                        std::bit_cast<std::uint64_t>(*pooled_dot.value()) == std::bit_cast<std::uint64_t>(chunked_dot) &&
                        std::bit_cast<std::uint32_t>(*pooled_narrow.value()) ==
                            std::bit_cast<std::uint32_t>(chunked_narrow);
    if (!agreed) {
        std::printf("# reduce.pooled: the pool's result differs from the serial chunked result\n");
        return 1;
    }
    Digest pooled;
    pooled.real(*pooled_sum.value());
    pooled.real(*pooled_dot.value());
    pooled.real(*pooled_narrow.value());
    print("reduce.pooled", pooled);
    return 0;
}

// ---- Numeric text ---------------------------------------------------------------

stxlib::TextView view_of(const char* const bytes, const usize count) {
    stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(bytes, count);
    if (!validated.succeeded()) return stxlib::TextView();
    const stxlib::TextView result = std::move(validated).take_value();
    return result;
}

void hash_formatted(Digest& digest, const text::Formatted& formatted) {
    const stxlib::TextView* const written = formatted.text();
    if (written == nullptr) {
        digest.word(0xffffffffffffffffULL);
        return;
    }
    digest.bytes((*written).data(), (*written).count());
}

void text_items() {
    const std::vector<double> doubles = double_inputs(0x74657874);
    const std::vector<float> floats = float_inputs(0x74787466);
    Digest shortest;
    Digest formatted;
    Digest floats_digest;
    char buffer[512];
    for (const double value : doubles) {
        const text::NumberText number = text::to_text(value);
        const stxlib::TextView view = number.view();
        shortest.bytes(view.data(), view.count());
        const text::Parsed<double> back = text::parse_double(view);
        if (back.succeeded()) {
            shortest.real(*back.value());
        } else {
            shortest.word(static_cast<std::uint64_t>(*back.failure()));
        }
        text::RealFormat scientific;
        scientific.notation = text::Notation::scientific;
        scientific.precision = 17;
        hash_formatted(formatted, text::format_real(stxlib::MutableView<char>(buffer), value, scientific));
        if (std::fabs(value) < 1e15) {
            text::RealFormat fixed;
            fixed.notation = text::Notation::fixed;
            fixed.precision = 3;
            hash_formatted(formatted, text::format_real(stxlib::MutableView<char>(buffer), value, fixed));
        }
    }
    for (const float value : floats) {
        const text::NumberText number = text::to_text(value);
        const stxlib::TextView view = number.view();
        floats_digest.bytes(view.data(), view.count());
        const text::Parsed<float> back = text::parse_float(view);
        if (back.succeeded()) {
            floats_digest.real(*back.value());
        } else {
            floats_digest.word(static_cast<std::uint64_t>(*back.failure()));
        }
    }
    // Parsing decimal texts of 17 to 25 digits, exponents -330..310 (the
    // rounding boundaries, the subnormal flush and overflow).
    Lcg generator{0x70617273};
    Digest parsed;
    for (int index = 0; index < 20000; ++index) {
        char digits[64];
        usize length = 0;
        if ((generator.step() >> 63) != 0) digits[length++] = '-';
        digits[length++] = static_cast<char>('1' + (generator.step() >> 33) % 9);
        digits[length++] = '.';
        const usize fraction = 16 + static_cast<usize>((generator.step() >> 33) % 9);
        for (usize place = 0; place < fraction; ++place) digits[length++] = static_cast<char>('0' + (generator.step() >> 33) % 10);
        const int exponent = static_cast<int>((generator.step() >> 33) % 641) - 330;
        const int written = std::snprintf(digits + length, sizeof(digits) - length, "e%d", exponent);
        length = length + static_cast<usize>(written);
        const stxlib::TextView view = view_of(digits, length);
        const text::Parsed<double> value = text::parse_double(view);
        if (value.succeeded()) {
            parsed.real(*value.value());
        } else {
            parsed.word(static_cast<std::uint64_t>(*value.failure()));
        }
        const text::Parsed<float> narrow = text::parse_float(view);
        if (narrow.succeeded()) {
            parsed.real(*narrow.value());
        } else {
            parsed.word(static_cast<std::uint64_t>(*narrow.failure()));
        }
    }
    print("text.shortest.double", shortest);
    print("text.formatted.double", formatted);
    print("text.shortest.float", floats_digest);
    print("text.parse", parsed);
}

// ---- Random ---------------------------------------------------------------------

namespace random = stxlib::random;

void random_items() {
    {
        random::Xoshiro256PlusPlus generator(random::seed_from_number(2026));
        Digest digest;
        for (int index = 0; index < 20000; ++index) {
            digest.word(generator.next());
            digest.real(random::uniform_unit(generator));
            digest.real(random::uniform_unit_float(generator));
            digest.real(random::uniform_real(generator, -3.5, 1e10));
            digest.real(random::uniform_real(generator, 1e-300, 2e-300));
            digest.word(random::chance(generator, 0.3) ? 1U : 0U);
            digest.word(static_cast<std::uint64_t>(random::uniform_between(generator, -1000, 1000)));
        }
        print("random.xoshiro256pp", digest);
    }
    {
        random::ChaCha12Stream generator(random::seed_from_number(2026), 7);
        Digest digest;
        for (int index = 0; index < 20000; ++index) {
            digest.word(generator.next());
            digest.real(random::uniform_unit(generator));
            digest.real(random::uniform_real(generator, -1.0, 1.0));
        }
        print("random.chacha12", digest);
    }
}

const char* level_text(const stxlib::simd::Level level) {
    if (level == stxlib::simd::Level::avx2_fma) return "avx2_fma";
    if (level == stxlib::simd::Level::vector128) return "vector128";
    return "scalar";
}

} // namespace

int main() {
    std::printf("# stxlib bit identity\n");
    std::printf("# simd level: %s\n", level_text(stxlib::simd::selected_level()));
#if defined(STXLIB_FLOAT_EXACT_FUSED)
    std::printf("# float_exact product: fused\n");
#else
    std::printf("# float_exact product: dekker\n");
#endif
#if defined(STXLIB_MATH_DEKKER)
    std::printf("# math: dekker build\n");
#else
    std::printf("# math: library build\n");
#endif
    math_items();
    exact_items();
    const int reduced = reduce_items();
    text_items();
    random_items();
    return reduced;
}
