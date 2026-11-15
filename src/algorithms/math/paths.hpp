// The two paths of the switched math kernels: today's kernels (pow.cpp,
// log.cpp, hypot.cpp, sincos.cpp, tan.cpp) and the region kernels
// (region.cpp, trig_region.cpp). Every build compiles both, in every
// variant, so one build can test and bench both (tests/math_region_tests.cpp
// calls each path by name); the switches below choose the path the public
// functions take. CMake sets them (options STXLIB_MATH_REGION_*); a build
// without CMake gets the defaults here, which must match CMakeLists.txt's.
//
// Default: the region path where it is proved and not slower by design,
// today's kernel otherwise (seat sm1, R2 note section 6.1):
//   - float hypot: region (correctly rounded by construction; a sqrt plus
//     about 12 operations, no certify-and-fall-back);
//   - sin, cos and tan of a double: region (the same proved 0.51 ulp; a
//     shorter reduction and polynomial chains, no cold redo);
//   - pow(double): today's (region proved 0.500097 ulp against 0.576, but
//     about 18 more operations per call);
//   - float log, log2, log10: today's (both correctly rounded for every
//     float; region is one double-double pass, more operations than
//     today's cheap path on the common input);
//   - float pow: today's (region proved correctly rounded except within
//     2^-94 of a midpoint, against 2^-52.7; its stage 1 is the double-double
//     kernel, more than today's cheap path).
// The bench (R2 run list) can overturn each default.

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
#define STXLIB_MATH_REGION_HYPOT_FLOAT 1
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
