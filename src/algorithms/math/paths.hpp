// The two paths of the switched math kernels: today's kernels (pow.cpp,
// log.cpp, hypot.cpp, sincos.cpp, tan.cpp) and the region kernels
// (region.cpp, trig_region.cpp). Every build compiles both, in every
// variant, so one build can test and bench both (tests/math_region_tests.cpp
// calls each path by name); the switches below choose the path the public
// functions take. CMake sets them (options STXLIB_MATH_REGION_*); a build
// without CMake gets the defaults here, which must match CMakeLists.txt's.
//
// Default, by the owner's rule (the faster path at today's accuracy), from
// the R2 bench on the M4 (main 45b275b; ns per call, today's against
// region; seat sm1, R2 note sections 6.1 and 11):
//   - sin, cos, sincos and tan of a double: region (sincos 5.56 against
//     7.07, tan 7.27 against 8.03; the same proved 0.51 ulp);
//   - float hypot: today's (2.36 against 5.00; both give identical results
//     on every tested pair);
//   - pow(double): today's (10.88 against 16.48);
//   - float pow: today's (5.11 against 35.8);
//   - float log, log2, log10: today's (region 1.25 to 1.5 times slower).
//     Today's cold fallback calls the region kernel for a positive normal
//     float, so both paths round the same way there.

#ifndef STXLIB_ALGORITHMS_MATH_PATHS_HPP
#define STXLIB_ALGORITHMS_MATH_PATHS_HPP

#include <bit>
#include <cstdint>

#if !defined(STXLIB_MATH_REGION_POW_DOUBLE)
#define STXLIB_MATH_REGION_POW_DOUBLE 0
#endif
#if !defined(STXLIB_MATH_REGION_POW_FLOAT)
#define STXLIB_MATH_REGION_POW_FLOAT 0
#endif
#if !defined(STXLIB_MATH_REGION_LOG_FLOAT)
#define STXLIB_MATH_REGION_LOG_FLOAT 0
#endif
#if !defined(STXLIB_MATH_REGION_HYPOT_FLOAT)
#define STXLIB_MATH_REGION_HYPOT_FLOAT 0
#endif
#if !defined(STXLIB_MATH_REGION_TRIG_DOUBLE)
#define STXLIB_MATH_REGION_TRIG_DOUBLE 1
#endif

namespace stxlib::math::paths {

// The region kernels apply STXlib's subnormal rule themselves; the variant
// functions (namespace unflushed's kernels) do not, and flush.cpp applies it
// around them. So a variant function takes the region path only for
// arguments that are not subnormal, and takes today's path for the rest
// and for a result the region kernel flushed: the variant function's
// unflushed contract stays whole on both paths, and the public function
// (flush(f(flush(arguments)))) is the region kernel's.
[[nodiscard]] inline bool not_subnormal(const double value) noexcept {
    const std::uint64_t magnitude = std::bit_cast<std::uint64_t>(value) & 0x7fffffffffffffffULL;
    const bool result = magnitude == 0 || magnitude >= 0x0010000000000000ULL;
    return result;
}

[[nodiscard]] inline bool not_subnormal(const float value) noexcept {
    const std::uint32_t magnitude = std::bit_cast<std::uint32_t>(value) & 0x7fffffffU;
    const bool result = magnitude == 0 || magnitude >= 0x00800000U;
    return result;
}

} // namespace stxlib::math::paths

#endif // STXLIB_ALGORITHMS_MATH_PATHS_HPP
