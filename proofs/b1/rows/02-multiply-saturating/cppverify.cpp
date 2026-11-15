// Row 02 for cpp-verify: P2 as a postcondition over 128-bit arithmetic. As
// row 01, std::numeric_limits makes a fail-closed stop the expected outcome.
#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=src/checked.hpp lines=34-40 namespace=stx_auxlib::checked sha256=6b7ff4bed5f86774c7ea6d33bac14b9d15d4bf27543b6ed0926b8209683d1da3
//@ contract source=stx_auxlib path=src/checked.hpp line=35 from=") noexcept {" to=") noexcept cppverify::post((unsigned __int128)cppverify::result == ((unsigned __int128)left * right < (unsigned __int128)SIZE_MAX ? (unsigned __int128)left * right : (unsigned __int128)SIZE_MAX)) STX_CV_PROBE {"
//@ mutant id=m1 source=stx_auxlib path=src/checked.hpp line=36 from="right > std" to="right >= std"
