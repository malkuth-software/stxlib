// The two paths of the switched kernels (paths.hpp), declared here without
// a guard for one kernel variant (the including source's own, or the
// namespace that STXLIB_MATH_DECLARE_VARIANT names):
//   - region: the region kernels (region.cpp): flat single-pass forms of
//     pow, the float logarithms and float hypot, and float pow's round,
//     test, decide; and (trig_region.cpp) sincos and tan of a double,
//     faster at the library's bound. Each with the public contract of
//     stxlib/math.hpp, the subnormal rule included.
//   - today: today's kernels for the same functions (pow.cpp, log.cpp,
//     hypot.cpp, sincos.cpp, tan.cpp), with the variant functions'
//     unflushed contract (flush.cpp applies the rule around them).
// The variant functions (variant_functions.hpp) take one path or the other
// by the switches in paths.hpp; tests/math_region_tests.cpp calls both.

#if defined(STXLIB_MATH_DECLARE_VARIANT)
namespace stxlib::math::STXLIB_MATH_DECLARE_VARIANT::region {
#else
namespace stxlib::math::STXLIB_MATH_VARIANT::region {
#endif

double pow(double x, double y) noexcept;
// Float pow: round, test, decide (region.cpp, "float pow"). Proved
// (tools/math/region_certificate.py `pow` for stage 1,
// tools/math/pow_float_accurate_certificate.py for stage 3, exact rational
// arithmetic): correctly rounded at every pair whose x^y is not within 2^-94
// (relative) of a float midpoint, at every exact result and exact midpoint
// (detected exactly), and at every pair with integer y in [1, 40]
// (tools/math/pow_float_midpoints.py). Not proved: a pair within 2^-94 of a
// midpoint without being one. None is known; the published literature gives
// no complete list of binary32 pow's hard cases (CORE-MATH checks powf
// against known hard cases only), and a density estimate puts the expected
// number of such pairs near 2^-9.4 (heuristic, not a proof).
float pow(float x, float y) noexcept;
float log(float x) noexcept;
float log2(float x) noexcept;
float log10(float x) noexcept;
float hypot(float x, float y) noexcept;
void sincos(double x, double& s, double& c) noexcept;
double tan(double x) noexcept;

// Test hooks for float pow's stages 2 and 3 (region.cpp).
float pow_float_decided(float x, float y) noexcept;
void pow_float_accurate_value(float x, float y, double& hi, double& lo) noexcept;

} // namespace stxlib::math::<variant>::region

#if defined(STXLIB_MATH_DECLARE_VARIANT)
namespace stxlib::math::STXLIB_MATH_DECLARE_VARIANT::today {
#else
namespace stxlib::math::STXLIB_MATH_VARIANT::today {
#endif

double pow(double x, double y) noexcept;
float pow(float x, float y) noexcept;
float log(float x) noexcept;
float log2(float x) noexcept;
float log10(float x) noexcept;
float hypot(float x, float y) noexcept;
void sincos(double x, double& s, double& c) noexcept;
double tan(double x) noexcept;

} // namespace stxlib::math::<variant>::today
