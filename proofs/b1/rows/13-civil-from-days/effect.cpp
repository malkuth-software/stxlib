// Row 13, bound-effect configuration (owner direction of 2026-11-13,
// common/effect.h): calendar_detail::civil_from_days with each arithmetic
// result abstracted to any value within its stated bound.
// Checked: P0 (no undefined behaviour in what is left exact) and P1's
//     control-flow part: the month is 1-12, which follows from
//     shifted_month in [0, 11] through the branch on line 101.
// Not checked here (numeric, in the row's record as obligations): the year
//     fits int32, the day is in its month, P2.
// Bounds (each an obligation, established outside this run):
//     era in [-5368710, 5368709]: floor division of a monotone shifted
//         day count, at the domain's ends (computed);
//     day_of_era in [0, 146096]: the floor-division identity;
//     year_of_era, day_of_year, shifted_month, day: exhaustive over the
//         146097 values of day_of_era (sb3, Python, 2026-11-13).
#include "effect.h"
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=92-107 namespace=stx_auxlib sha256=901f54e82d9e6a062efcfdb31da22b8d0eccf35e566eee622a35799104d2928f
//@ abstract source=stx_auxlib path=src/calendar.cpp line=94 from="(shifted >= 0 ? shifted : shifted - 146096) / 146097" to="stx_effect_i64_given((shifted >= 0 ? shifted : shifted - 146096) >= -784353015833LL + 719468 - 146096, -5368710, 5368709)" bound="era in [-5368710, 5368709] for days in the stated domain"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=95 from="shifted - era * 146097" to="stx_effect_i64(0, 146096)" bound="day_of_era in [0, 146096]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=97 from="(day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365" to="stx_effect_i64_given(day_of_era >= 0 && day_of_era <= 146096, 0, 399)" bound="year_of_era in [0, 399]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=98 from="day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100)" to="stx_effect_i64_given(year_of_era >= 0 && year_of_era <= 399, 0, 365)" bound="day_of_year in [0, 365]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=99 from="(5 * day_of_year + 2) / 153" to="stx_effect_i64_given(day_of_year >= 0 && day_of_year <= 365, 0, 11)" bound="shifted_month in [0, 11]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=100 from="day_of_year - (153 * shifted_month + 2) / 5 + 1" to="stx_effect_i64_given(shifted_month >= 0 && shifted_month <= 11, 1, 31)" bound="day in [1, 31]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=103 from="era * 400" to="stx_effect_i64_given(era >= -5368710 && era <= 5368709, -2147484000LL, 2147483600LL)" bound="era * 400 in [-2147484000, 2147483600]"
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=101 from="shifted_month < 10" to="shifted_month < 9"

namespace calendar = stx_auxlib::calendar_detail;

int main() {
    const std::int64_t days = static_cast<std::int64_t>(nondet_longlong());
    STX_ASSUME(days >= -784353015833LL && days <= 784351576776LL);
    const calendar::CivilDay date = calendar::civil_from_days(days);
    STX_ASSERT(date.month >= 1 && date.month <= 12, "P1: the month is 1-12");
    STX_VACUITY_PROBE();
    return 0;
}
