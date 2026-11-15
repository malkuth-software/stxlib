// Row 12 for cpp-verify: P0 under the stated precondition, as a
// precondition contract; the tool's definedness obligations are the
// property.
#include <cstdint>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=80-90 namespace=stx_auxlib sha256=9b2c107d6c48f7b9e2c23fa56b7762d2ec71569a84d6fd9a3b96053c9a7a4338
//@ contract source=stx_auxlib path=src/calendar.cpp line=81 from=") noexcept {" to=") noexcept cppverify::pre(year >= -2147483648LL && year <= 2147483647LL && month >= 1 && month <= 12 && day >= 1 && day <= 31) STX_CV_PROBE {"
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=86 from="+ 2) / 5" to="+ 3) / 5"
