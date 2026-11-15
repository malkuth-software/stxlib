// Row 13 for cpp-verify: P0 over the stated day range and the month half
// of P1 as a postcondition on the returned record.
#include <cstdint>

#if defined(STX_VACUITY)
#define STX_CV_PROBE cppverify::post(false)
#else
#define STX_CV_PROBE
#endif

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=92-107 namespace=stx_auxlib sha256=901f54e82d9e6a062efcfdb31da22b8d0eccf35e566eee622a35799104d2928f
//@ contract source=stx_auxlib path=src/calendar.cpp line=92 from=") noexcept {" to=") noexcept cppverify::pre(days >= -784353015833LL && days <= 784351576776LL) cppverify::post(cppverify::result.month >= 1 && cppverify::result.month <= 12 && cppverify::result.day >= 1 && cppverify::result.day <= 31) STX_CV_PROBE {"
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=101 from="shifted_month < 10" to="shifted_month < 9"
