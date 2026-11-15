// Tests of src/core/float_exact.hpp, the exact floating-point primitives
// (planning/stages/A1-float-discipline.md, section 2), against an
// independent exact reference: two's-complement integers of 2,560 bits, in
// which every value of the tested ranges is an integer in units of a power
// of two.
//
//   float_exact_tests               sampled (the ctest run)
//   float_exact_tests --exhaustive  every float for the split; every float
//                                   significand of a binade against fixed
//                                   partners for the float sum and product
//
// What is exhaustive is said where it runs; everything else is sampled.

#include "core/float_exact.hpp"

#include "core/checked.hpp"
#include "test_support.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

namespace fe = stxlib::float_exact;
using stxlib::usize;

__extension__ typedef unsigned __int128 Wide;

// ---- The exact reference --------------------------------------------------------

constexpr usize big_limbs = 40;

struct Big final {
    std::uint64_t limb[big_limbs]{};
};

// Arithmetic modulo 2^2560 by design: two's complement.
STXLIB_MODULAR Big add(const Big& left, const Big& right) {
    Big result{};
    std::uint64_t carry = 0;
    for (usize index = 0; index < big_limbs; ++index) {
        const Wide sum = static_cast<Wide>(left.limb[index]) + right.limb[index] + carry;
        result.limb[index] = static_cast<std::uint64_t>(sum);
        carry = static_cast<std::uint64_t>(sum >> 64);
    }
    return result;
}

STXLIB_MODULAR Big negate(const Big& value) {
    Big result{};
    std::uint64_t carry = 1;
    for (usize index = 0; index < big_limbs; ++index) {
        const Wide sum = static_cast<Wide>(~value.limb[index]) + carry;
        result.limb[index] = static_cast<std::uint64_t>(sum);
        carry = static_cast<std::uint64_t>(sum >> 64);
    }
    return result;
}

Big subtract(const Big& left, const Big& right) {
    return add(left, negate(right));
}

// The low 2,560 bits of the product: the two's-complement product while it
// fits, which every use here does (at most about 2,100 bits).
STXLIB_MODULAR Big multiply(const Big& left, const Big& right) {
    Big result{};
    for (usize i = 0; i < big_limbs; ++i) {
        if (left.limb[i] == 0) continue;
        std::uint64_t carry = 0;
        for (usize j = 0; i + j < big_limbs; ++j) {
            const Wide part = static_cast<Wide>(left.limb[i]) * right.limb[j] + result.limb[i + j] + carry;
            result.limb[i + j] = static_cast<std::uint64_t>(part);
            carry = static_cast<std::uint64_t>(part >> 64);
        }
    }
    return result;
}

int sign_of(const Big& value) {
    if ((value.limb[big_limbs - 1] >> 63) != 0) return -1;
    for (usize index = 0; index < big_limbs; ++index) {
        if (value.limb[index] != 0) return 1;
    }
    return 0;
}

bool equal(const Big& left, const Big& right) {
    return std::memcmp(left.limb, right.limb, sizeof left.limb) == 0;
}

bool reference_ok = true;

// value * 2^shift as an integer; the value must be a multiple of 2^-shift
// (a failed check otherwise).
Big from_double(const double value, const int shift) {
    Big result{};
    if (value == 0.0) return result;
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const int field = static_cast<int>((bits >> 52) & 0x7ffU);
    std::uint64_t significand = bits & 0x000fffffffffffffULL;
    int exponent = field - 1075;
    if (field != 0) {
        significand |= 0x0010000000000000ULL;
    } else {
        exponent = -1074;
    }
    int total = exponent + shift;
    while (total < 0 && (significand & 1U) == 0) {
        significand >>= 1;
        total += 1;
    }
    if (total < 0) {
        reference_ok = false;
        return result;
    }
    const usize limb = static_cast<usize>(total) / 64;
    const unsigned offset = static_cast<unsigned>(total) % 64;
    if (limb + 1 >= big_limbs) {
        reference_ok = false;
        return result;
    }
    result.limb[limb] = significand << offset;
    if (offset != 0) result.limb[limb + 1] = significand >> (64 - offset);
    if ((bits >> 63) != 0) result = negate(result);
    return result;
}

// ---- Inputs --------------------------------------------------------------------------

struct Lcg final {
    std::uint64_t state{0};
    // A 64-bit linear congruential step: arithmetic modulo 2^64 by design.
    STXLIB_MODULAR std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t high = state >> 32;
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (high << 32) | (state >> 32);
    }
    double unit() {
        return static_cast<double>(next() >> 11) * 0x1.0p-53;
    }
    // A random double with an exponent in [low, high] and either sign.
    double scaled(const int low, const int high) {
        const std::uint64_t bits = next();
        const int span = high - low + 1;
        const int exponent = low + static_cast<int>(bits % static_cast<std::uint64_t>(span));
        const double significand = 1.0 + static_cast<double>(next() >> 12) * 0x1.0p-52;
        const double magnitude = std::ldexp(significand, exponent);
        return (bits >> 63) != 0 ? -magnitude : magnitude;
    }
};

// ---- Float: the split (exhaustive) ----------------------------------------------

// Every float in split's range (--exhaustive: all 2^32 patterns; ctest: a
// stride): hi + lo == a exactly, hi and lo each with at most 12 significant
// bits, both zero or normal.
void float_split(const bool exhaustive) {
    const std::uint64_t stride = exhaustive ? 1 : 4099;
    bool ok = true;
    std::uint64_t checked = 0;
    for (std::uint64_t pattern = 0; pattern <= 0xffffffffULL; pattern += stride) {
        const float a = std::bit_cast<float>(static_cast<std::uint32_t>(pattern));
        if (!fe::in_split_range(a)) continue;
        const fe::Pair<float> parts = fe::split(a);
        const bool exact = static_cast<double>(parts.value) + static_cast<double>(parts.error) == static_cast<double>(a);
        const bool hi_narrow = (std::bit_cast<std::uint32_t>(parts.value) & 0xfffU) == 0;
        const bool lo_narrow = (std::bit_cast<std::uint32_t>(parts.error) & 0xfffU) == 0;
        const bool normal = fe::zero_or_normal(parts.value) && fe::zero_or_normal(parts.error);
        if (!(exact && hi_narrow && lo_narrow && normal)) {
            std::fprintf(stderr, "split(float) failed at %a\n", static_cast<double>(a));
            ok = false;
        }
        checked += 1;
    }
    CHECK(ok);
    std::printf("float split: %llu floats%s\n", static_cast<unsigned long long>(checked),
                exhaustive ? " (every float in range)" : "");
}

// ---- Float: sum and product (a binade against partners) ---------------------------

// Every float significand of [1, 2) (--exhaustive; ctest: a stride), with
// either sign, against 512 partners of exponent -40 to 40: two_product's
// error is exact (checked in double, where a float product is exact) and the
// two forms agree bit for bit where the build has both; two_sum's and
// fast_two_sum's errors are exact (checked in double where the exponents
// differ by at most 28, and as (a, b) beyond that).
void float_sum_product(const bool exhaustive) {
    Lcg lcg{41};
    float partners[512]{};
    for (float& partner : partners) partner = static_cast<float>(lcg.scaled(-40, 40));
    const std::uint32_t stride = exhaustive ? 1U : 8191U;
    bool products_ok = true;
    bool forms_agree = true;
    bool sums_ok = true;
    std::uint64_t checked = 0;
    for (std::uint32_t fraction = 0; fraction < 0x800000U; fraction += stride) {
        const float positive = std::bit_cast<float>(0x3f800000U | fraction);
        const float signs[2] = {positive, -positive};
        for (const float a : signs) {
            for (const float b : partners) {
                const fe::Pair<float> product = fe::two_product(a, b);
                const double exact_product = static_cast<double>(a) * static_cast<double>(b);
                if (static_cast<double>(product.value) + static_cast<double>(product.error) != exact_product ||
                    !fe::zero_or_normal(product.error)) {
                    products_ok = false;
                }
#if defined(STXLIB_FLOAT_EXACT_FUSED)
                const fe::Pair<float> dekker = fe::raw::two_product_dekker(a, b);
                const fe::Pair<float> fused = fe::raw::two_product_fused(a, b);
                if (std::bit_cast<std::uint32_t>(dekker.error) != std::bit_cast<std::uint32_t>(fused.error)) {
                    forms_agree = false;
                }
#endif
                const fe::Pair<float> sum = fe::two_sum(a, b);
                const int gap = fe::exponent_of(a) - fe::exponent_of(b);
                if (gap <= 28 && gap >= -28) {
                    if (static_cast<double>(sum.value) + static_cast<double>(sum.error) !=
                        static_cast<double>(a) + static_cast<double>(b)) {
                        sums_ok = false;
                    }
                } else if (gap > 28) {
                    if (sum.value != a || sum.error != b) sums_ok = false;
                } else if (sum.value != b || sum.error != a) {
                    sums_ok = false;
                }
                if (gap >= 0) {
                    const fe::Pair<float> fast = fe::fast_two_sum(a, b);
                    if (std::bit_cast<std::uint32_t>(fast.value) != std::bit_cast<std::uint32_t>(sum.value) ||
                        std::bit_cast<std::uint32_t>(fast.error) != std::bit_cast<std::uint32_t>(sum.error)) {
                        sums_ok = false;
                    }
                }
                checked += 1;
            }
        }
    }
    CHECK(products_ok);
    CHECK(forms_agree);
    CHECK(sums_ok);
    std::printf("float sum and product: %llu pairs%s\n", static_cast<unsigned long long>(checked),
                exhaustive ? " (every significand of a binade, both signs, 512 partners)" : "");
}

// ---- Double: sum, product and split (sampled) --------------------------------------

void double_primitives(const int count) {
    Lcg lcg{42};
    bool sums_ok = true;
    bool products_ok = true;
    bool forms_agree = true;
    bool splits_ok = true;
    for (int i = 0; i < count; ++i) {
        const double a = lcg.scaled(-450, 450);
        const double b = lcg.scaled(-450, 450);
        const fe::Pair<double> sum = fe::two_sum(a, b);
        if (!equal(add(from_double(a, 1074), from_double(b, 1074)),
                   add(from_double(sum.value, 1074), from_double(sum.error, 1074)))) {
            sums_ok = false;
        }
        if (fe::exponent_of(a) >= fe::exponent_of(b)) {
            const fe::Pair<double> fast = fe::fast_two_sum(a, b);
            if (fast.value != sum.value || fast.error != sum.error) sums_ok = false;
        }
        if (fe::in_product_range(a, b)) {
            const fe::Pair<double> product = fe::two_product(a, b);
            const Big exact = multiply(from_double(a, 510), from_double(b, 510));
            if (!equal(exact, add(from_double(product.value, 1020), from_double(product.error, 1020)))) {
                products_ok = false;
            }
#if defined(STXLIB_FLOAT_EXACT_FUSED)
            const fe::Pair<double> dekker = fe::raw::two_product_dekker(a, b);
            const fe::Pair<double> fused = fe::raw::two_product_fused(a, b);
            if (std::bit_cast<std::uint64_t>(dekker.error) != std::bit_cast<std::uint64_t>(fused.error)) {
                forms_agree = false;
            }
#endif
        }
        const fe::Pair<double> parts = fe::split(a);
        const bool narrow = (std::bit_cast<std::uint64_t>(parts.value) & ((std::uint64_t{1} << 27) - 1)) == 0;
        if (parts.value + parts.error != a || !narrow) splits_ok = false;
    }
    CHECK(reference_ok);
    CHECK(sums_ok);
    CHECK(products_ok);
    CHECK(forms_agree);
    CHECK(splits_ok);
    std::printf("double primitives: %d samples\n", count);
}

// ---- Expansions (sampled) ----------------------------------------------------------

// A random expansion of up to `terms` values with exponents in [-200, 200]
// (multiples of 2^-252), grown one value at a time; `exact` gets the sum.
usize random_expansion(Lcg& lcg, const usize terms, double* out, Big& exact) {
    double scratch[64]{};
    usize count = 0;
    exact = Big{};
    for (usize term = 0; term < terms; ++term) {
        double value = lcg.scaled(-200, 200);
        if (term > 0 && lcg.unit() < 0.3) value = -out[count - 1];  // cancellation
        exact = add(exact, from_double(value, 252));
        if (count == 0) {
            out[0] = value;
            count = 1;
            continue;
        }
        const usize grown = fe::grow_expansion(out, count, value, scratch);
        // Drop zero components (grow_expansion keeps them).
        count = 0;
        for (usize index = 0; index < grown; ++index) {
            if (scratch[index] != 0.0) out[count++] = scratch[index];
        }
        if (count == 0) out[count++] = 0.0;
    }
    return count;
}

Big expansion_value(const double* e, const usize count, const int shift) {
    Big total{};
    for (usize index = 0; index < count; ++index) total = add(total, from_double(e[index], shift));
    return total;
}

bool nonoverlapping_increasing(const double* e, const usize count) {
    for (usize index = 1; index < count; ++index) {
        if (e[index - 1] == 0.0) continue;
        if (!(std::fabs(e[index - 1]) < std::fabs(e[index]))) return false;
    }
    return true;
}

void expansions(const int count) {
    Lcg lcg{43};
    bool sums_ok = true;
    bool scales_ok = true;
    bool products_ok = true;
    bool signs_ok = true;
    for (int i = 0; i < count; ++i) {
        double e[16]{};
        double f[16]{};
        Big e_exact{};
        Big f_exact{};
        const usize e_count = random_expansion(lcg, 1 + static_cast<usize>(lcg.unit() * 8.0), e, e_exact);
        const usize f_count = random_expansion(lcg, 1 + static_cast<usize>(lcg.unit() * 8.0), f, f_exact);
        if (!equal(expansion_value(e, e_count, 252), e_exact)) sums_ok = false;
        double h[32]{};
        const usize h_count = fe::expansion_sum(e, e_count, f, f_count, h);
        if (!equal(expansion_value(h, h_count, 252), add(e_exact, f_exact)) || !nonoverlapping_increasing(h, h_count)) {
            sums_ok = false;
        }
        if (fe::expansion_sign(h, h_count) != sign_of(add(e_exact, f_exact))) signs_ok = false;
        const double b = lcg.scaled(-200, 200);
        double scaled[32]{};
        const usize scaled_count = fe::scale_expansion(e, e_count, b, scaled);
        if (!equal(expansion_value(scaled, scaled_count, 504), multiply(e_exact, from_double(b, 252)))) {
            scales_ok = false;
        }
        double product[512]{};
        double scratch[1024]{};
        const usize product_count = fe::expansion_product(e, e_count, f, f_count, product, scratch);
        if (!equal(expansion_value(product, product_count, 504), multiply(e_exact, f_exact))) products_ok = false;
    }
    CHECK(reference_ok);
    CHECK(sums_ok);
    CHECK(scales_ok);
    CHECK(products_ok);
    CHECK(signs_ok);
    std::printf("expansions: %d samples\n", count);
}

// ---- Compensated sums (sampled) ----------------------------------------------------

// The error of each compensated sum against the exact sum (an expansion,
// checked above), within its bound: Neumaier u|S| + 2n u^2 sum|x|, Kahan
// 2u sum|x| + 2n u^2 sum|x| (Higham, section 4.3), with 1% slack for the
// bound's own rounding.
void compensated(const int rounds) {
    Lcg lcg{44};
    bool kahan_ok = true;
    bool neumaier_ok = true;
    const double u = 0x1p-53;
    for (int round = 0; round < rounds; ++round) {
        constexpr int terms = 1000;
        fe::KahanSum<double> kahan{};
        fe::NeumaierSum<double> neumaier{};
        // The exact sum's components lie within 2^-82 to 2^41 and do not
        // overlap, so there are at most 123 nonzero ones.
        double exact[161]{};
        double scratch[162]{};
        usize exact_count = 1;
        double absolute = 0.0;
        for (int term = 0; term < terms; ++term) {
            const double value = lcg.scaled(-30, 30);
            kahan.add(value);
            neumaier.add(value);
            absolute += std::fabs(value);
            const usize grown = fe::grow_expansion(exact, exact_count, value, scratch);
            exact_count = 0;
            for (usize index = 0; index < grown; ++index) {
                if (scratch[index] != 0.0) exact[exact_count++] = scratch[index];
            }
            if (exact_count == 0) exact[exact_count++] = 0.0;
        }
        const double total = exact[exact_count - 1];
        double difference[162]{};
        const usize kahan_count = fe::grow_expansion(exact, exact_count, -kahan.value(), difference);
        double kahan_error = 0.0;
        for (usize index = 0; index < kahan_count; ++index) kahan_error += difference[index];
        const usize neumaier_count = fe::grow_expansion(exact, exact_count, -neumaier.value(), difference);
        double neumaier_error = 0.0;
        for (usize index = 0; index < neumaier_count; ++index) neumaier_error += difference[index];
        const double second_order = 2.0 * terms * u * u * absolute;
        if (std::fabs(kahan_error) > 1.01 * (2.0 * u * absolute + second_order)) kahan_ok = false;
        if (std::fabs(neumaier_error) > 1.01 * (u * std::fabs(total) + second_order)) neumaier_ok = false;
    }
    CHECK(kahan_ok);
    CHECK(neumaier_ok);
    std::printf("compensated sums: %d rounds of 1000 terms\n", rounds);
}

// ---- Predicates (sampled, with exactly degenerate cases) -----------------------------

int orient2d_reference(const double ax, const double ay, const double bx, const double by, const double cx,
                       const double cy) {
    const int shift = 510;
    const Big acx = subtract(from_double(ax, shift), from_double(cx, shift));
    const Big bcy = subtract(from_double(by, shift), from_double(cy, shift));
    const Big acy = subtract(from_double(ay, shift), from_double(cy, shift));
    const Big bcx = subtract(from_double(bx, shift), from_double(cx, shift));
    return sign_of(subtract(multiply(acx, bcy), multiply(acy, bcx)));
}

int incircle_reference(const double ax, const double ay, const double bx, const double by, const double cx,
                       const double cy, const double dx, const double dy) {
    const int shift = 252;
    const Big adx = subtract(from_double(ax, shift), from_double(dx, shift));
    const Big ady = subtract(from_double(ay, shift), from_double(dy, shift));
    const Big bdx = subtract(from_double(bx, shift), from_double(dx, shift));
    const Big bdy = subtract(from_double(by, shift), from_double(dy, shift));
    const Big cdx = subtract(from_double(cx, shift), from_double(dx, shift));
    const Big cdy = subtract(from_double(cy, shift), from_double(dy, shift));
    const Big alift = add(multiply(adx, adx), multiply(ady, ady));
    const Big blift = add(multiply(bdx, bdx), multiply(bdy, bdy));
    const Big clift = add(multiply(cdx, cdx), multiply(cdy, cdy));
    const Big a_term = multiply(alift, subtract(multiply(bdx, cdy), multiply(cdx, bdy)));
    const Big b_term = multiply(blift, subtract(multiply(cdx, ady), multiply(adx, cdy)));
    const Big c_term = multiply(clift, subtract(multiply(adx, bdy), multiply(bdx, ady)));
    return sign_of(add(add(a_term, b_term), c_term));
}

double nudge(const double value, const int direction) {
    const double toward = direction > 0 ? std::numeric_limits<double>::infinity()
                                        : -std::numeric_limits<double>::infinity();
    return std::nextafter(value, toward);
}

void predicates(const int count) {
    Lcg lcg{45};
    bool orient_ok = true;
    bool incircle_ok = true;
    bool float_ok = true;
    int degenerate = 0;
    // Pythagorean points of the circle of radius 5.
    const double circle[8][2] = {{5, 0}, {4, 3}, {3, 4}, {0, 5}, {-3, 4}, {-4, -3}, {0, -5}, {3, -4}};
    const int scales[] = {0, -20, 20, -180, 200, -400, 450};
    for (int i = 0; i < count; ++i) {
        // Random points.
        double p[8]{};
        for (double& coordinate : p) coordinate = lcg.scaled(-20, 20);
        if (fe::orient2d(p[0], p[1], p[2], p[3], p[4], p[5]) != orient2d_reference(p[0], p[1], p[2], p[3], p[4], p[5])) {
            orient_ok = false;
        }
        if (fe::incircle(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]) !=
            incircle_reference(p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7])) {
            incircle_ok = false;
        }
        // Exactly collinear: small integers along a line, scaled by a power of
        // two within orient2d's range, then perhaps one coordinate nudged.
        const int scale = scales[static_cast<usize>(lcg.unit() * 7.0)];
        const double x0 = std::floor(lcg.unit() * 2000.0) - 1000.0;
        const double y0 = std::floor(lcg.unit() * 2000.0) - 1000.0;
        const double dx = std::floor(lcg.unit() * 200.0) - 100.0;
        const double dy = std::floor(lcg.unit() * 200.0) - 100.0;
        const double t = std::floor(lcg.unit() * 50.0) - 25.0;
        double ax = std::ldexp(x0, scale);
        double ay = std::ldexp(y0, scale);
        double bx = std::ldexp(x0 + dx, scale);
        double by = std::ldexp(y0 + dy, scale);
        double cx = std::ldexp(x0 + t * dx, scale);
        double cy = std::ldexp(y0 + t * dy, scale);
        const int nudged = static_cast<int>(lcg.unit() * 3.0) - 1;
        if (nudged != 0 && cx != 0.0) cx = nudge(cx, nudged);
        if (nudged == 0) degenerate += 1;
        if (fe::orient2d(ax, ay, bx, by, cx, cy) != orient2d_reference(ax, ay, bx, by, cx, cy)) orient_ok = false;
        // Exactly cocircular: three Pythagorean points and a fourth, scaled
        // within incircle's range and translated by an integer, perhaps nudged.
        const int circle_scale = static_cast<int>(lcg.unit() * 400.0) - 180;
        const double offset_x = std::floor(lcg.unit() * 64.0) - 32.0;
        const double offset_y = std::floor(lcg.unit() * 64.0) - 32.0;
        double q[8]{};
        const usize first = static_cast<usize>(lcg.unit() * 8.0);
        for (usize point = 0; point < 4; ++point) {
            const usize index = (first + point * 2) % 8;
            q[2 * point] = std::ldexp(circle[index][0] + offset_x, circle_scale);
            q[2 * point + 1] = std::ldexp(circle[index][1] + offset_y, circle_scale);
        }
        const int circle_nudged = static_cast<int>(lcg.unit() * 3.0) - 1;
        if (circle_nudged != 0 && q[6] != 0.0) q[6] = nudge(q[6], circle_nudged);
        if (fe::incircle(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]) !=
            incircle_reference(q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7])) {
            incircle_ok = false;
        }
        // The float forms: the same points, where they are floats exactly.
        const float fx[6] = {static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(bx),
                             static_cast<float>(by), static_cast<float>(cx), static_cast<float>(cy)};
        bool exact_floats = true;
        const double original[6] = {ax, ay, bx, by, cx, cy};
        for (usize index = 0; index < 6; ++index) {
            if (static_cast<double>(fx[index]) != original[index] || !fe::zero_or_normal(fx[index])) exact_floats = false;
        }
        if (exact_floats && fe::orient2d(fx[0], fx[1], fx[2], fx[3], fx[4], fx[5]) !=
                                orient2d_reference(ax, ay, bx, by, cx, cy)) {
            float_ok = false;
        }
    }
    CHECK(reference_ok);
    CHECK(orient_ok);
    CHECK(incircle_ok);
    CHECK(float_ok);
    std::printf("predicates: %d rounds (random, collinear and cocircular; %d exactly collinear)\n", count, degenerate);
}

} // namespace

// ---- The ranges' overflow edges (M5 counterexamples) -----------------------------

// The ranges exclude the inputs the M5 proofs refuted (ESBMC and z3: a NaN or
// infinite error term with a finite sum or product), and admit the largest
// operands they state, where the primitives stay exact (checked in double,
// where these float sums and products are exact). Cases chosen by hand;
// D01 and the ESBMC harness prove the float forms over the whole range.
void range_edges() {
    const float flt_max = std::numeric_limits<float>::max();
    const double dbl_max = std::numeric_limits<double>::max();
    // two_sum: a = 0x767e0000, b = -FLT_MAX gave a NaN error.
    CHECK(!fe::in_sum_range(-flt_max));
    CHECK(!fe::in_sum_range(std::bit_cast<float>(0x7f000001U)));
    CHECK(fe::in_sum_range(0x1p127f) && fe::in_sum_range(-0x1p127f));
    CHECK(!fe::in_sum_range(dbl_max) && fe::in_sum_range(0x1p1023));
    const float sum_edges[][2] = {{0x1p127f, -0x1.fffffep126f},
                                  {std::bit_cast<float>(0x767e0000U), -0x1p127f},
                                  {0x1p127f, 0x1.fffffep125f},
                                  {-0x1p127f, -0x1p-102f}};
    bool sums_ok = true;
    for (const float (&pair)[2] : sum_edges) {
        const fe::Pair<float> sum = fe::two_sum(pair[0], pair[1]);
        const double exact = static_cast<double>(pair[0]) + static_cast<double>(pair[1]);
        if (!fe::finite(sum.value) || !fe::finite(sum.error) ||
            static_cast<double>(sum.error) != exact - static_cast<double>(sum.value)) {
            sums_ok = false;
        }
    }
    CHECK(sums_ok);
    // two_product: a = -0, b = -FLT_MAX gave NaN (Dekker's split of b); a =
    // 0x5edbbe36, b = 0x60151e43 (exponents 62 and 65) gave an infinite error.
    CHECK(!fe::in_product_range(-0.0f, -flt_max));
    CHECK(!fe::in_product_range(std::bit_cast<float>(0x5edbbe36U), std::bit_cast<float>(0x60151e43U)));
    CHECK(fe::in_product_range(0.0f, 0x1p115f));
    CHECK(!fe::in_product_range(-0.0, -dbl_max));
    const float product_edges[][2] = {{0x1.fffffep62f, 0x1.fffffep62f},
                                      {-0x1.fffffep62f, 0x1.fffffep63f},
                                      {std::bit_cast<float>(0x5edbbe36U), 0x1.0a3c86p62f},
                                      {0.0f, -0x1p115f}};
    bool products_ok = true;
    bool forms_agree = true;
    for (const float (&pair)[2] : product_edges) {
        if (!fe::in_product_range(pair[0], pair[1])) {
            products_ok = false;
            continue;
        }
        const fe::Pair<float> product = fe::two_product(pair[0], pair[1]);
        const double exact = static_cast<double>(pair[0]) * static_cast<double>(pair[1]);
        if (!fe::finite(product.value) || !fe::finite(product.error) ||
            static_cast<double>(product.error) != exact - static_cast<double>(product.value)) {
            products_ok = false;
        }
#if defined(STXLIB_FLOAT_EXACT_FUSED)
        const fe::Pair<float> dekker = fe::raw::two_product_dekker(pair[0], pair[1]);
        const fe::Pair<float> fused = fe::raw::two_product_fused(pair[0], pair[1]);
        if (std::bit_cast<std::uint32_t>(dekker.error) != std::bit_cast<std::uint32_t>(fused.error)) {
            forms_agree = false;
        }
#endif
    }
    // The double edge: exponents 510 and 511 (sum 1021 = emax - 2).
    const double a = 0x1.fffffffffffffp510;
    const double b = -0x1.fffffffffffffp511;
    CHECK(fe::in_product_range(a, b));
    const fe::Pair<double> wide = fe::two_product(a, b);
    if (!fe::finite(wide.value) || !fe::finite(wide.error)) products_ok = false;
#if defined(STXLIB_FLOAT_EXACT_FUSED)
    const fe::Pair<double> wide_dekker = fe::raw::two_product_dekker(a, b);
    const fe::Pair<double> wide_fused = fe::raw::two_product_fused(a, b);
    if (std::bit_cast<std::uint64_t>(wide_dekker.error) != std::bit_cast<std::uint64_t>(wide_fused.error)) {
        forms_agree = false;
    }
#endif
    CHECK(products_ok);
    CHECK(forms_agree);
    std::printf("float_exact range edges: checked\n");
}

int main(const int argument_count, char** const arguments) {
    const bool exhaustive = argument_count == 2 && std::strcmp(arguments[1], "--exhaustive") == 0;
    float_split(exhaustive);
    float_sum_product(exhaustive);
    double_primitives(exhaustive ? 20000000 : 200000);
    expansions(exhaustive ? 2000000 : 20000);
    compensated(exhaustive ? 20000 : 200);
    predicates(exhaustive ? 2000000 : 20000);
    range_edges();
    return stxlib_tests::finish("stxlib float_exact");
}
