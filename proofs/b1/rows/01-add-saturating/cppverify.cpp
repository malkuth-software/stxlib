// Row 01 for cpp-verify: the row's P2 as a postcondition; P0 is the tool's
// own definedness obligations. The function calls std::numeric_limits (a
// class template), which cpp-verify 0.1.1 lists as unsupported, so a
// fail-closed stop is the expected outcome and is recorded as the distance.
#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=src/checked.hpp lines=27-31 namespace=stx_auxlib::checked sha256=9876341cc7ada133234c8923111088d3c725908dd9053937ea3f0b96aabcc56c
//@ contract source=stx_auxlib path=src/checked.hpp line=27 from=") noexcept {" to=") noexcept cppverify::post(cppverify::result == (left > SIZE_MAX - right ? SIZE_MAX : left + right)) STX_CV_PROBE {"
//@ mutant id=m1 source=stx_auxlib path=src/checked.hpp line=28 from="return std::numeric_limits<std::size_t>::max();" to="return std::numeric_limits<std::size_t>::max() - 1;"
