// The region kernels (region.cpp): flat single-pass forms of pow, the float
// logarithms and float hypot; and (trig_region.cpp) sincos and tan of a
// double, faster at the library's bound. Each with the public contract of
// stxlib/math.hpp (subnormal rule included). Declared here, without a
// guard, for one kernel variant (region.cpp's own, or the namespace that
// STXLIB_MATH_DECLARE_VARIANT names for a caller that compiles region.cpp
// as that variant). An experiment: nothing in the library calls these; the
// owner decides whether they replace the current kernels.

#if defined(STXLIB_MATH_DECLARE_VARIANT)
namespace stxlib::math::STXLIB_MATH_DECLARE_VARIANT::region {
#else
namespace stxlib::math::STXLIB_MATH_VARIANT::region {
#endif

double pow(double x, double y) noexcept;
float pow(float x, float y) noexcept;
float log(float x) noexcept;
float log2(float x) noexcept;
float log10(float x) noexcept;
float hypot(float x, float y) noexcept;
void sincos(double x, double& s, double& c) noexcept;
double tan(double x) noexcept;

} // namespace stxlib::math::<variant>::region
