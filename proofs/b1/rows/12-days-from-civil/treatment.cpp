// Row 12: calendar_detail::days_from_civil (B1.md section 3.1).
// P0: no undefined behaviour (no int64 overflow) for year in int32, month
//     1-12, day 1-31 (the year range is the stated precondition,
//     src/calendar_detail.hpp:25-28).
// P2: equals the day count of common/calendar_spec.h (365 days a year plus
//     the leap days counted by inclusion and exclusion), an algorithm
//     independent of Hinnant's.
// Bound: the whole stated domain, symbolic; no loop.
#include "calendar_spec.h"
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=80-90 namespace=stx_auxlib sha256=9b2c107d6c48f7b9e2c23fa56b7762d2ec71569a84d6fd9a3b96053c9a7a4338
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=86 from="+ 2) / 5" to="+ 3) / 5"

int main() {
    const std::int64_t year = static_cast<std::int64_t>(nondet_longlong());
    const std::uint32_t month = nondet_uint();
    const std::uint32_t day = nondet_uint();
    STX_ASSUME(year >= INT32_MIN && year <= INT32_MAX);
    STX_ASSUME(month >= 1 && month <= 12);
    STX_ASSUME(day >= 1 && day <= 31);
    const std::int64_t days = stx_auxlib::calendar_detail::days_from_civil(year, month, day);
    STX_ASSERT(days == stx_spec_days(year, month, day), "P2: days_from_civil is the reference day count");
    STX_VACUITY_PROBE();
    return 0;
}
