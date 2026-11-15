// Row 13: calendar_detail::civil_from_days (B1.md section 3.1).
// P0: no undefined behaviour over the days whose year fits int32, that is
//     days from days(-2^31-01-01) = -784353015833 to days(2^31-1-12-31) =
//     784351576776 (computed with common/calendar_spec.h's formula).
// P1: the result is a valid date: month 1-12, day 1 to the month's length,
//     year in int32.
// P2: inverse of row 12 both ways: days_from_civil(civil_from_days(d)) == d
//     for every d in range, and civil_from_days(days_from_civil(date)) ==
//     date for every valid date with year in int32.
// Bound: the whole stated domain, symbolic; no loop.
#include "calendar_spec.h"
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=80-107 namespace=stx_auxlib sha256=dd35c9e5c9471d9b9de4c8ea818032917441cf1417a92fce29aa3aa0d687a53a
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=101 from="shifted_month < 10" to="shifted_month < 9"

namespace calendar = stx_auxlib::calendar_detail;

int main() {
    const std::int64_t days = static_cast<std::int64_t>(nondet_longlong());
    STX_ASSUME(days >= -784353015833LL && days <= 784351576776LL);
    const calendar::CivilDay date = calendar::civil_from_days(days);
    STX_ASSERT(date.year >= INT32_MIN && date.year <= INT32_MAX, "P1: the year fits int32");
    STX_ASSERT(date.month >= 1 && date.month <= 12, "P1: the month is 1-12");
    STX_ASSERT(date.day >= 1 && date.day <= stx_spec_days_in_month(date.year, date.month), "P1: the day is in its month");
    STX_ASSERT(calendar::days_from_civil(date.year, date.month, date.day) == days, "P2: days_from_civil inverts civil_from_days");

    const std::int64_t year = static_cast<std::int64_t>(nondet_longlong());
    const std::uint32_t month = nondet_uint();
    const std::uint32_t day = nondet_uint();
    STX_ASSUME(year >= INT32_MIN && year <= INT32_MAX);
    STX_ASSUME(month >= 1 && month <= 12);
    STX_ASSUME(day >= 1 && day <= stx_spec_days_in_month(year, month));
    const calendar::CivilDay back = calendar::civil_from_days(calendar::days_from_civil(year, month, day));
    STX_ASSERT(back.year == year && back.month == month && back.day == day, "P2: civil_from_days inverts days_from_civil");
    STX_VACUITY_PROBE();
    return 0;
}
