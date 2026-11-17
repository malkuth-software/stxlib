/* math.h for x86_64-pc-windows-msvc over ucrtbase.dll (C17 7.12).
 *
 * Every double function is ucrtbase.dll's. A float function is imported
 * where ucrtbase.dll exports it and is otherwise an inline form over the
 * double one: frexpf, ldexpf and modff are exact that way (the float value,
 * its exponent and its parts are exact in double), fabsf is the compiler's
 * builtin, and hypotf is the exported _hypotf. Every long double function is
 * an inline form over the double one: long double is double on this target,
 * so the conversions change no value.
 *
 * Classification uses the compiler's builtins (macros in C, overloads in
 * C++); the FP_* category values are
 * ours (no UCRT function receives or returns them through this header).
 * FP_ILOGB0 and FP_ILOGBNAN are what ucrtbase.dll's ilogb returns (probe).
 * math_errhandling: the UCRT sets errno and raises floating-point
 * exceptions (probe: log(-1) set EDOM and FE_INVALID).
 *
 * This file's declarations are generated from the export snapshot by
 * tools/windows_libcxx/gen_math_header.py (the block between BEGIN GENERATED
 * and END GENERATED); edit the generator, not the block.
 */
#ifndef STXLIB_CRT_MATH_H
#define STXLIB_CRT_MATH_H

#include "stxlib_crt_common.h"

typedef float float_t;
typedef double double_t;

#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))

#define FP_INFINITE 1
#define FP_NAN 2
#define FP_NORMAL (-1)
#define FP_SUBNORMAL (-2)
#define FP_ZERO 0

#define FP_ILOGB0 (-0x7fffffff - 1)
#define FP_ILOGBNAN 0x7fffffff

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling (MATH_ERRNO | MATH_ERREXCEPT)

#ifndef __cplusplus
#define fpclassify(x) (__builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, (x)))
#define isfinite(x) (__builtin_isfinite(x))
#define isinf(x) (__builtin_isinf(x))
#define isnan(x) (__builtin_isnan(x))
#define isnormal(x) (__builtin_isnormal(x))
#define signbit(x) (__builtin_signbit(x))
#define isgreater(x, y) (__builtin_isgreater((x), (y)))
#define isgreaterequal(x, y) (__builtin_isgreaterequal((x), (y)))
#define isless(x, y) (__builtin_isless((x), (y)))
#define islessequal(x, y) (__builtin_islessequal((x), (y)))
#define islessgreater(x, y) (__builtin_islessgreater((x), (y)))
#define isunordered(x, y) (__builtin_isunordered((x), (y)))
#else
/* C++: libc++ declares fpclassify and signbit itself and, on this C
 * library, expects the classification and comparison functions as C++
 * overloads at global scope (include/math.h, "The MSVC runtime already
 * provides these functions as templates"); it adds the integral forms. */
extern "C++" {
inline bool isfinite(float x) noexcept { return __builtin_isfinite(x); }
inline bool isfinite(double x) noexcept { return __builtin_isfinite(x); }
inline bool isfinite(long double x) noexcept { return __builtin_isfinite(x); }
inline bool isinf(float x) noexcept { return __builtin_isinf(x); }
inline bool isinf(double x) noexcept { return __builtin_isinf(x); }
inline bool isinf(long double x) noexcept { return __builtin_isinf(x); }
inline bool isnan(float x) noexcept { return __builtin_isnan(x); }
inline bool isnan(double x) noexcept { return __builtin_isnan(x); }
inline bool isnan(long double x) noexcept { return __builtin_isnan(x); }
inline bool isnormal(float x) noexcept { return __builtin_isnormal(x); }
inline bool isnormal(double x) noexcept { return __builtin_isnormal(x); }
inline bool isnormal(long double x) noexcept { return __builtin_isnormal(x); }
/* Two arguments of any floating types; the builtins compare after the usual
 * arithmetic conversions and raise no exception for a NaN. */
template <class _Left, class _Right>
inline bool isgreater(_Left x, _Right y) noexcept { return __builtin_isgreater(x, y); }
template <class _Left, class _Right>
inline bool isgreaterequal(_Left x, _Right y) noexcept { return __builtin_isgreaterequal(x, y); }
template <class _Left, class _Right>
inline bool isless(_Left x, _Right y) noexcept { return __builtin_isless(x, y); }
template <class _Left, class _Right>
inline bool islessequal(_Left x, _Right y) noexcept { return __builtin_islessequal(x, y); }
template <class _Left, class _Right>
inline bool islessgreater(_Left x, _Right y) noexcept { return __builtin_islessgreater(x, y); }
template <class _Left, class _Right>
inline bool isunordered(_Left x, _Right y) noexcept { return __builtin_isunordered(x, y); }
}
#endif

/* The Microsoft names our sources use. */
_STXLIB_CRT_BEGIN
_STXLIB_UCRT float __cdecl _hypotf(float x, float y);
_STXLIB_UCRT double __cdecl _hypot(double x, double y);
_STXLIB_UCRT int __cdecl _finite(double x);
_STXLIB_UCRT int __cdecl _isnan(double x);
_STXLIB_UCRT double __cdecl _copysign(double x, double y);
_STXLIB_UCRT double __cdecl _chgsign(double x);
_STXLIB_CRT_END

_STXLIB_CRT_BEGIN

/* BEGIN GENERATED */
_STXLIB_UCRT double __cdecl acos(double x);
_STXLIB_UCRT double __cdecl asin(double x);
_STXLIB_UCRT double __cdecl atan(double x);
_STXLIB_UCRT double __cdecl atan2(double y, double x);
_STXLIB_UCRT double __cdecl cos(double x);
_STXLIB_UCRT double __cdecl sin(double x);
_STXLIB_UCRT double __cdecl tan(double x);
_STXLIB_UCRT double __cdecl acosh(double x);
_STXLIB_UCRT double __cdecl asinh(double x);
_STXLIB_UCRT double __cdecl atanh(double x);
_STXLIB_UCRT double __cdecl cosh(double x);
_STXLIB_UCRT double __cdecl sinh(double x);
_STXLIB_UCRT double __cdecl tanh(double x);
_STXLIB_UCRT double __cdecl exp(double x);
_STXLIB_UCRT double __cdecl exp2(double x);
_STXLIB_UCRT double __cdecl expm1(double x);
_STXLIB_UCRT double __cdecl frexp(double x, int* exponent);
_STXLIB_UCRT int __cdecl ilogb(double x);
_STXLIB_UCRT double __cdecl ldexp(double x, int exponent);
_STXLIB_UCRT double __cdecl log(double x);
_STXLIB_UCRT double __cdecl log10(double x);
_STXLIB_UCRT double __cdecl log1p(double x);
_STXLIB_UCRT double __cdecl log2(double x);
_STXLIB_UCRT double __cdecl logb(double x);
_STXLIB_UCRT double __cdecl modf(double x, double* integral);
_STXLIB_UCRT double __cdecl scalbn(double x, int exponent);
_STXLIB_UCRT double __cdecl scalbln(double x, long exponent);
_STXLIB_UCRT double __cdecl cbrt(double x);
_STXLIB_UCRT double __cdecl fabs(double x);
_STXLIB_UCRT double __cdecl hypot(double x, double y);
_STXLIB_UCRT double __cdecl pow(double x, double y);
_STXLIB_UCRT double __cdecl sqrt(double x);
_STXLIB_UCRT double __cdecl erf(double x);
_STXLIB_UCRT double __cdecl erfc(double x);
_STXLIB_UCRT double __cdecl lgamma(double x);
_STXLIB_UCRT double __cdecl tgamma(double x);
_STXLIB_UCRT double __cdecl ceil(double x);
_STXLIB_UCRT double __cdecl floor(double x);
_STXLIB_UCRT double __cdecl nearbyint(double x);
_STXLIB_UCRT double __cdecl rint(double x);
_STXLIB_UCRT long __cdecl lrint(double x);
_STXLIB_UCRT long long __cdecl llrint(double x);
_STXLIB_UCRT double __cdecl round(double x);
_STXLIB_UCRT long __cdecl lround(double x);
_STXLIB_UCRT long long __cdecl llround(double x);
_STXLIB_UCRT double __cdecl trunc(double x);
_STXLIB_UCRT double __cdecl fmod(double x, double y);
_STXLIB_UCRT double __cdecl remainder(double x, double y);
_STXLIB_UCRT double __cdecl remquo(double x, double y, int* quotient);
_STXLIB_UCRT double __cdecl copysign(double x, double y);
_STXLIB_UCRT double __cdecl nan(const char* tag);
_STXLIB_UCRT double __cdecl nextafter(double x, double y);
_STXLIB_UCRT double __cdecl nexttoward(double x, long double y);
_STXLIB_UCRT double __cdecl fdim(double x, double y);
_STXLIB_UCRT double __cdecl fmax(double x, double y);
_STXLIB_UCRT double __cdecl fmin(double x, double y);
_STXLIB_UCRT double __cdecl fma(double x, double y, double z);

_STXLIB_UCRT float __cdecl acosf(float x);
_STXLIB_UCRT float __cdecl asinf(float x);
_STXLIB_UCRT float __cdecl atanf(float x);
_STXLIB_UCRT float __cdecl atan2f(float y, float x);
_STXLIB_UCRT float __cdecl cosf(float x);
_STXLIB_UCRT float __cdecl sinf(float x);
_STXLIB_UCRT float __cdecl tanf(float x);
_STXLIB_UCRT float __cdecl acoshf(float x);
_STXLIB_UCRT float __cdecl asinhf(float x);
_STXLIB_UCRT float __cdecl atanhf(float x);
_STXLIB_UCRT float __cdecl coshf(float x);
_STXLIB_UCRT float __cdecl sinhf(float x);
_STXLIB_UCRT float __cdecl tanhf(float x);
_STXLIB_UCRT float __cdecl expf(float x);
_STXLIB_UCRT float __cdecl exp2f(float x);
_STXLIB_UCRT float __cdecl expm1f(float x);
_STXLIB_CRT_INLINE float __cdecl frexpf(float x, int* exponent) {
    const float result = (float)frexp((double)x, exponent);
    return result;
}
_STXLIB_UCRT int __cdecl ilogbf(float x);
_STXLIB_CRT_INLINE float __cdecl ldexpf(float x, int exponent) {
    const float result = (float)ldexp((double)x, exponent);
    return result;
}
_STXLIB_UCRT float __cdecl logf(float x);
_STXLIB_UCRT float __cdecl log10f(float x);
_STXLIB_UCRT float __cdecl log1pf(float x);
_STXLIB_UCRT float __cdecl log2f(float x);
_STXLIB_UCRT float __cdecl logbf(float x);
_STXLIB_UCRT float __cdecl modff(float x, float* integral);
_STXLIB_UCRT float __cdecl scalbnf(float x, int exponent);
_STXLIB_UCRT float __cdecl scalblnf(float x, long exponent);
_STXLIB_UCRT float __cdecl cbrtf(float x);
_STXLIB_CRT_INLINE float __cdecl fabsf(float x) {
    const float result = (float)__builtin_fabsf(x);
    return result;
}
_STXLIB_CRT_INLINE float __cdecl hypotf(float x, float y) {
    const float result = (float)_hypotf(x, y);
    return result;
}
_STXLIB_UCRT float __cdecl powf(float x, float y);
_STXLIB_UCRT float __cdecl sqrtf(float x);
_STXLIB_UCRT float __cdecl erff(float x);
_STXLIB_UCRT float __cdecl erfcf(float x);
_STXLIB_UCRT float __cdecl lgammaf(float x);
_STXLIB_UCRT float __cdecl tgammaf(float x);
_STXLIB_UCRT float __cdecl ceilf(float x);
_STXLIB_UCRT float __cdecl floorf(float x);
_STXLIB_UCRT float __cdecl nearbyintf(float x);
_STXLIB_UCRT float __cdecl rintf(float x);
_STXLIB_UCRT long __cdecl lrintf(float x);
_STXLIB_UCRT long long __cdecl llrintf(float x);
_STXLIB_UCRT float __cdecl roundf(float x);
_STXLIB_UCRT long __cdecl lroundf(float x);
_STXLIB_UCRT long long __cdecl llroundf(float x);
_STXLIB_UCRT float __cdecl truncf(float x);
_STXLIB_UCRT float __cdecl fmodf(float x, float y);
_STXLIB_UCRT float __cdecl remainderf(float x, float y);
_STXLIB_UCRT float __cdecl remquof(float x, float y, int* quotient);
_STXLIB_UCRT float __cdecl copysignf(float x, float y);
_STXLIB_UCRT float __cdecl nanf(const char* tag);
_STXLIB_UCRT float __cdecl nextafterf(float x, float y);
_STXLIB_UCRT float __cdecl nexttowardf(float x, long double y);
_STXLIB_UCRT float __cdecl fdimf(float x, float y);
_STXLIB_UCRT float __cdecl fmaxf(float x, float y);
_STXLIB_UCRT float __cdecl fminf(float x, float y);
_STXLIB_UCRT float __cdecl fmaf(float x, float y, float z);

_STXLIB_CRT_INLINE long double __cdecl acosl(long double x) {
    const long double result = (long double)acos((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl asinl(long double x) {
    const long double result = (long double)asin((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl atanl(long double x) {
    const long double result = (long double)atan((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl atan2l(long double y, long double x) {
    const long double result = (long double)atan2((double)y, (double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl cosl(long double x) {
    const long double result = (long double)cos((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl sinl(long double x) {
    const long double result = (long double)sin((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl tanl(long double x) {
    const long double result = (long double)tan((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl acoshl(long double x) {
    const long double result = (long double)acosh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl asinhl(long double x) {
    const long double result = (long double)asinh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl atanhl(long double x) {
    const long double result = (long double)atanh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl coshl(long double x) {
    const long double result = (long double)cosh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl sinhl(long double x) {
    const long double result = (long double)sinh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl tanhl(long double x) {
    const long double result = (long double)tanh((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl expl(long double x) {
    const long double result = (long double)exp((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl exp2l(long double x) {
    const long double result = (long double)exp2((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl expm1l(long double x) {
    const long double result = (long double)expm1((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl frexpl(long double x, int* exponent) {
    const long double result = (long double)frexp((double)x, exponent);
    return result;
}
_STXLIB_CRT_INLINE int __cdecl ilogbl(long double x) {
    const int result = ilogb((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl ldexpl(long double x, int exponent) {
    const long double result = (long double)ldexp((double)x, exponent);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl logl(long double x) {
    const long double result = (long double)log((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl log10l(long double x) {
    const long double result = (long double)log10((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl log1pl(long double x) {
    const long double result = (long double)log1p((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl log2l(long double x) {
    const long double result = (long double)log2((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl logbl(long double x) {
    const long double result = (long double)logb((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl modfl(long double x, long double* integral) {
    double value_integral = 0.0;
    const long double result = (long double)modf((double)x, &value_integral);
    *integral = (long double)value_integral;
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl scalbnl(long double x, int exponent) {
    const long double result = (long double)scalbn((double)x, exponent);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl scalblnl(long double x, long exponent) {
    const long double result = (long double)scalbln((double)x, exponent);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl cbrtl(long double x) {
    const long double result = (long double)cbrt((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fabsl(long double x) {
    const long double result = (long double)fabs((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl hypotl(long double x, long double y) {
    const long double result = (long double)hypot((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl powl(long double x, long double y) {
    const long double result = (long double)pow((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl sqrtl(long double x) {
    const long double result = (long double)sqrt((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl erfl(long double x) {
    const long double result = (long double)erf((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl erfcl(long double x) {
    const long double result = (long double)erfc((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl lgammal(long double x) {
    const long double result = (long double)lgamma((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl tgammal(long double x) {
    const long double result = (long double)tgamma((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl ceill(long double x) {
    const long double result = (long double)ceil((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl floorl(long double x) {
    const long double result = (long double)floor((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl nearbyintl(long double x) {
    const long double result = (long double)nearbyint((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl rintl(long double x) {
    const long double result = (long double)rint((double)x);
    return result;
}
_STXLIB_CRT_INLINE long __cdecl lrintl(long double x) {
    const long result = lrint((double)x);
    return result;
}
_STXLIB_CRT_INLINE long long __cdecl llrintl(long double x) {
    const long long result = llrint((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl roundl(long double x) {
    const long double result = (long double)round((double)x);
    return result;
}
_STXLIB_CRT_INLINE long __cdecl lroundl(long double x) {
    const long result = lround((double)x);
    return result;
}
_STXLIB_CRT_INLINE long long __cdecl llroundl(long double x) {
    const long long result = llround((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl truncl(long double x) {
    const long double result = (long double)trunc((double)x);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fmodl(long double x, long double y) {
    const long double result = (long double)fmod((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl remainderl(long double x, long double y) {
    const long double result = (long double)remainder((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl remquol(long double x, long double y, int* quotient) {
    const long double result = (long double)remquo((double)x, (double)y, quotient);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl copysignl(long double x, long double y) {
    const long double result = (long double)copysign((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl nanl(const char* tag) {
    const long double result = (long double)nan(tag);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl nextafterl(long double x, long double y) {
    const long double result = (long double)nextafter((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl nexttowardl(long double x, long double y) {
    const long double result = (long double)nexttoward((double)x, (long double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fdiml(long double x, long double y) {
    const long double result = (long double)fdim((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fmaxl(long double x, long double y) {
    const long double result = (long double)fmax((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fminl(long double x, long double y) {
    const long double result = (long double)fmin((double)x, (double)y);
    return result;
}
_STXLIB_CRT_INLINE long double __cdecl fmal(long double x, long double y, long double z) {
    const long double result = (long double)fma((double)x, (double)y, (double)z);
    return result;
}
/* END GENERATED */

_STXLIB_CRT_END

#endif /* STXLIB_CRT_MATH_H */
