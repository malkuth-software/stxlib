// The public math functions (stxlib/math.hpp): STXlib's subnormal rule
// around the unflushed functions of dispatch.cpp. Owner decision 2026-11-13
// (notes decisions/2026-11-13-locals-float-mutability.md, "Subnormals"):
// STXlib's numeric code neither accepts nor produces subnormal values. Each
// function here reads every subnormal argument as zero of its sign, calls
// the unflushed function (the kernel the SIMD level selects, stx_auxlib
// b6e330a's bits), and returns a subnormal result as zero of its sign.
//
// The flush is done here, in software, on the bits: the floating-point
// environment's flush-to-zero and denormals-are-zero modes are never set or
// read, since changing the environment is a leak (the same decision,
// "Floating point is allowed and well bounded"). Zero, normal, infinite and
// NaN values pass through unchanged, NaN payloads included. New in STXlib
// batch 3c8; not from stx_auxlib.

#include "stxlib/math.hpp"

#define STXLIB_MATH_DECLARE_VARIANT unflushed
#include "variant_functions.hpp"
#undef STXLIB_MATH_DECLARE_VARIANT

#include <bit>
#include <cstdint>

namespace stxlib::math {

namespace {

// Zero of the value's sign when the value is subnormal (biased exponent 0,
// significand not 0), else the value itself. A zero maps to itself.
[[nodiscard]] double flush(const double value) noexcept {
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t exponent_bits = 0x7ff0000000000000ULL;
    const std::uint64_t sign_bit = 0x8000000000000000ULL;
    if ((bits & exponent_bits) == 0) {
        const double signed_zero = std::bit_cast<double>(bits & sign_bit);
        return signed_zero;
    }
    return value;
}

[[nodiscard]] float flush(const float value) noexcept {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t exponent_bits = 0x7f800000U;
    const std::uint32_t sign_bit = 0x80000000U;
    if ((bits & exponent_bits) == 0) {
        const float signed_zero = std::bit_cast<float>(bits & sign_bit);
        return signed_zero;
    }
    return value;
}

} // namespace

void sincos(const float angle, float& s, float& c) noexcept {
    float kernel_s = 0.0f;
    float kernel_c = 0.0f;
    unflushed::sincos(flush(angle), kernel_s, kernel_c);
    s = flush(kernel_s);
    c = flush(kernel_c);
}

void sincos(const double x, double& s, double& c) noexcept {
    double kernel_s = 0.0;
    double kernel_c = 0.0;
    unflushed::sincos(flush(x), kernel_s, kernel_c);
    s = flush(kernel_s);
    c = flush(kernel_c);
}

float exp(const float x) noexcept {
    const float result = flush(unflushed::exp(flush(x)));
    return result;
}

double exp(const double x) noexcept {
    const double result = flush(unflushed::exp(flush(x)));
    return result;
}

double log(const double x) noexcept {
    const double result = flush(unflushed::log(flush(x)));
    return result;
}

float log(const float x) noexcept {
    const float result = flush(unflushed::log(flush(x)));
    return result;
}

double log2(const double x) noexcept {
    const double result = flush(unflushed::log2(flush(x)));
    return result;
}

float log2(const float x) noexcept {
    const float result = flush(unflushed::log2(flush(x)));
    return result;
}

double log10(const double x) noexcept {
    const double result = flush(unflushed::log10(flush(x)));
    return result;
}

float log10(const float x) noexcept {
    const float result = flush(unflushed::log10(flush(x)));
    return result;
}

double pow(const double x, const double y) noexcept {
    const double result = flush(unflushed::pow(flush(x), flush(y)));
    return result;
}

float pow(const float x, const float y) noexcept {
    const float result = flush(unflushed::pow(flush(x), flush(y)));
    return result;
}

double hypot(const double x, const double y) noexcept {
    const double result = flush(unflushed::hypot(flush(x), flush(y)));
    return result;
}

float hypot(const float x, const float y) noexcept {
    const float result = flush(unflushed::hypot(flush(x), flush(y)));
    return result;
}

double atan(const double x) noexcept {
    const double result = flush(unflushed::atan(flush(x)));
    return result;
}

float atan(const float x) noexcept {
    const float result = flush(unflushed::atan(flush(x)));
    return result;
}

double atan2(const double y, const double x) noexcept {
    const double result = flush(unflushed::atan2(flush(y), flush(x)));
    return result;
}

float atan2(const float y, const float x) noexcept {
    const float result = flush(unflushed::atan2(flush(y), flush(x)));
    return result;
}

double asin(const double x) noexcept {
    const double result = flush(unflushed::asin(flush(x)));
    return result;
}

float asin(const float x) noexcept {
    const float result = flush(unflushed::asin(flush(x)));
    return result;
}

double acos(const double x) noexcept {
    const double result = flush(unflushed::acos(flush(x)));
    return result;
}

float acos(const float x) noexcept {
    const float result = flush(unflushed::acos(flush(x)));
    return result;
}

double tan(const double x) noexcept {
    const double result = flush(unflushed::tan(flush(x)));
    return result;
}

float tan(const float x) noexcept {
    const float result = flush(unflushed::tan(flush(x)));
    return result;
}

double cbrt(const double x) noexcept {
    const double result = flush(unflushed::cbrt(flush(x)));
    return result;
}

float cbrt(const float x) noexcept {
    const float result = flush(unflushed::cbrt(flush(x)));
    return result;
}

double sinh(const double x) noexcept {
    const double result = flush(unflushed::sinh(flush(x)));
    return result;
}

float sinh(const float x) noexcept {
    const float result = flush(unflushed::sinh(flush(x)));
    return result;
}

double cosh(const double x) noexcept {
    const double result = flush(unflushed::cosh(flush(x)));
    return result;
}

float cosh(const float x) noexcept {
    const float result = flush(unflushed::cosh(flush(x)));
    return result;
}

double tanh(const double x) noexcept {
    const double result = flush(unflushed::tanh(flush(x)));
    return result;
}

float tanh(const float x) noexcept {
    const float result = flush(unflushed::tanh(flush(x)));
    return result;
}

double asinh(const double x) noexcept {
    const double result = flush(unflushed::asinh(flush(x)));
    return result;
}

float asinh(const float x) noexcept {
    const float result = flush(unflushed::asinh(flush(x)));
    return result;
}

double acosh(const double x) noexcept {
    const double result = flush(unflushed::acosh(flush(x)));
    return result;
}

float acosh(const float x) noexcept {
    const float result = flush(unflushed::acosh(flush(x)));
    return result;
}

double atanh(const double x) noexcept {
    const double result = flush(unflushed::atanh(flush(x)));
    return result;
}

float atanh(const float x) noexcept {
    const float result = flush(unflushed::atanh(flush(x)));
    return result;
}

} // namespace stxlib::math
