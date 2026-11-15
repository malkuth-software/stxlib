// Row 16 for cpp-verify: the stated preconditions and P2 as contracts.
#include <cstdint>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=include/stx_auxlib/random.hpp lines=140-148 namespace=stx_auxlib::random::detail sha256=1c397ea0a4db64c3e4a90300a2b19b157ccb4591081412a19d6e6eefeae94404
//@ lift source=stx_auxlib path=src/random.cpp lines=212-228 namespace=stx_auxlib::random sha256=f80e7446f46e8f64aa25c641cc320cae30e0167139f32dcca44846efc0dd4b4e
//@ contract source=stx_auxlib path=src/random.cpp line=212 from=") noexcept {" to=") noexcept cppverify::pre(low <= high) cppverify::post((__int128)cppverify::result == (__int128)high - (__int128)low) STX_CV_PROBE {"
//@ contract source=stx_auxlib path=src/random.cpp line=221 from=") noexcept {" to=") noexcept cppverify::pre((__int128)low + (__int128)offset <= (__int128)INT64_MAX) cppverify::post((__int128)cppverify::result == (__int128)low + (__int128)offset) {"
//@ mutant id=m1 source=stx_auxlib path=src/random.cpp line=226 from="offset - to_minus_one - 1" to="offset - to_minus_one"
