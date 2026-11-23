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

#include "core/subnormal.hpp"

namespace stxlib::math {

namespace {

double flush(const double value) noexcept {
    return ::stxlib::detail::flush_subnormal(value);
}

float flush(const float value) noexcept {
    return ::stxlib::detail::flush_subnormal(value);
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

// sin and cos as single functions (roadmap R1): fronts over sincos above,
// so their bits and contracts are sincos'.
float sin(const float angle) noexcept {
    float s = 0.0f;
    float c = 0.0f;
    sincos(angle, s, c);
    return s;
}

float cos(const float angle) noexcept {
    float s = 0.0f;
    float c = 0.0f;
    sincos(angle, s, c);
    return c;
}

double sin(const double x) noexcept {
    double s = 0.0;
    double c = 0.0;
    sincos(x, s, c);
    return s;
}

double cos(const double x) noexcept {
    double s = 0.0;
    double c = 0.0;
    sincos(x, s, c);
    return c;
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

double exp2(const double x) noexcept {
    const double result = flush(unflushed::exp2(flush(x)));
    return result;
}

float exp2(const float x) noexcept {
    const float result = flush(unflushed::exp2(flush(x)));
    return result;
}

double expm1(const double x) noexcept {
    const double result = flush(unflushed::expm1(flush(x)));
    return result;
}

float expm1(const float x) noexcept {
    const float result = flush(unflushed::expm1(flush(x)));
    return result;
}

double log1p(const double x) noexcept {
    const double result = flush(unflushed::log1p(flush(x)));
    return result;
}

float log1p(const float x) noexcept {
    const float result = flush(unflushed::log1p(flush(x)));
    return result;
}

void sincospi(const float x, float& s, float& c) noexcept {
    float kernel_s = 0.0f;
    float kernel_c = 0.0f;
    unflushed::sincospi(flush(x), kernel_s, kernel_c);
    s = flush(kernel_s);
    c = flush(kernel_c);
}

void sincospi(const double x, double& s, double& c) noexcept {
    double kernel_s = 0.0;
    double kernel_c = 0.0;
    unflushed::sincospi(flush(x), kernel_s, kernel_c);
    s = flush(kernel_s);
    c = flush(kernel_c);
}

double sinpi(const double x) noexcept {
    const double result = flush(unflushed::sinpi(flush(x)));
    return result;
}

float sinpi(const float x) noexcept {
    const float result = flush(unflushed::sinpi(flush(x)));
    return result;
}

double cospi(const double x) noexcept {
    const double result = flush(unflushed::cospi(flush(x)));
    return result;
}

float cospi(const float x) noexcept {
    const float result = flush(unflushed::cospi(flush(x)));
    return result;
}

double sincpi(const double x) noexcept {
    const double result = flush(unflushed::sincpi(flush(x)));
    return result;
}

float sincpi(const float x) noexcept {
    const float result = flush(unflushed::sincpi(flush(x)));
    return result;
}

double cosm1(const double x) noexcept {
    const double result = flush(unflushed::cosm1(flush(x)));
    return result;
}

float cosm1(const float x) noexcept {
    const float result = flush(unflushed::cosm1(flush(x)));
    return result;
}

double cosm1pi(const double x) noexcept {
    const double result = flush(unflushed::cosm1pi(flush(x)));
    return result;
}

float cosm1pi(const float x) noexcept {
    const float result = flush(unflushed::cosm1pi(flush(x)));
    return result;
}

// sincosd writes every zero as +0 (math.hpp): the flush can make a tiny
// sine -0, and adding +0 changes -0 alone.
void sincosd(const double x, double& s, double& c) noexcept {
    double kernel_s = 0.0;
    double kernel_c = 0.0;
    unflushed::sincosd(flush(x), kernel_s, kernel_c);
    s = flush(kernel_s) + 0.0;
    c = flush(kernel_c) + 0.0;
}

void sincosd(const float x, float& s, float& c) noexcept {
    float kernel_s = 0.0f;
    float kernel_c = 0.0f;
    unflushed::sincosd(flush(x), kernel_s, kernel_c);
    s = flush(kernel_s) + 0.0f;
    c = flush(kernel_c) + 0.0f;
}

} // namespace stxlib::math
