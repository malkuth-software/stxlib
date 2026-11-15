// Row 15 for cpp-verify: P2 as a postcondition over unsigned __int128.
#include <cstdint>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=include/stx_auxlib/random.hpp lines=140-148 namespace=stx_auxlib::random::detail sha256=1c397ea0a4db64c3e4a90300a2b19b157ccb4591081412a19d6e6eefeae94404
//@ lift source=stx_auxlib path=src/random.cpp lines=195-210 namespace=stx_auxlib::random sha256=9a3bc204df677d1e4b979446efe69ef3674c5864db9000d951d9f899805f62d5
//@ contract source=stx_auxlib path=src/random.cpp line=195 from=") noexcept {" to=") noexcept cppverify::post((((unsigned __int128)cppverify::result.high) << 64 | cppverify::result.low) == (unsigned __int128)left * right) STX_CV_PROBE {"
//@ mutant id=m1 source=stx_auxlib path=src/random.cpp line=205 from=" + low_high" to=""
