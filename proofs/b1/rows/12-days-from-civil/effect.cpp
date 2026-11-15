// Row 12, bound-effect configuration (owner direction of 2026-11-13,
// common/effect.h): calendar_detail::days_from_civil with each arithmetic
// result abstracted to any value within its stated bound.
// Checked: P0 (no undefined behaviour in what is left exact: the branches
//     and the linear steps around them). The function has no control-flow
//     property beyond P0, so this configuration has no mutant.
// Not checked here (numeric, in the row's record as obligations): P2.
// Bounds (each an obligation, established outside this run):
//     era in [-5368710, 5368709] and year_of_era in [0, 399]: floor
//         division of shifted_year in [INT32_MIN - 1, INT32_MAX];
//     day_of_year in [0, 367], day_of_era in [0, 146098], the result in
//         [-784353144338, 784351705403]: exhaustive over their operands'
//         stated ranges (sb3, Python, 2026-11-13).
#include "effect.h"
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=14-23 namespace=stx_auxlib::calendar_detail sha256=435286ad26bcb01ec23b91f136104f0e434e17ab23f9d2b4c94912f9375ebaa2
//@ lift source=stx_auxlib path=src/calendar.cpp lines=80-90 namespace=stx_auxlib sha256=9b2c107d6c48f7b9e2c23fa56b7762d2ec71569a84d6fd9a3b96053c9a7a4338
//@ abstract source=stx_auxlib path=src/calendar.cpp line=83 from="(shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400" to="stx_effect_i64_given((shifted_year >= 0 ? shifted_year : shifted_year - 399) >= -2147484048LL, -5368710, 5368709)" bound="era in [-5368710, 5368709]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=84 from="shifted_year - era * 400" to="stx_effect_i64_given(era >= -5368710 && era <= 5368709, 0, 399)" bound="year_of_era in [0, 399]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=86 from="(153 * shifted_month + 2) / 5 + day - 1" to="stx_effect_i64_given(shifted_month >= 0 && shifted_month <= 11 && day >= 1 && day <= 31, 0, 367)" bound="day_of_year in [0, 367]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=87 from="year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year" to="stx_effect_i64_given(year_of_era >= 0 && year_of_era <= 399 && day_of_year >= 0 && day_of_year <= 367, 0, 146098)" bound="day_of_era in [0, 146098]"
//@ abstract source=stx_auxlib path=src/calendar.cpp line=88 from="era * 146097 + day_of_era - 719468" to="stx_effect_i64_given(era >= -5368710 && era <= 5368709 && day_of_era >= 0 && day_of_era <= 146098, -784353144338LL, 784351705403LL)" bound="the day count in [-784353144338, 784351705403]"

namespace calendar = stx_auxlib::calendar_detail;

int main() {
    const std::int64_t year = static_cast<std::int64_t>(nondet_longlong());
    const std::uint32_t month = nondet_uint();
    const std::uint32_t day = nondet_uint();
    STX_ASSUME(year >= INT32_MIN && year <= INT32_MAX);
    STX_ASSUME(month >= 1 && month <= 12);
    STX_ASSUME(day >= 1 && day <= 31);
    const std::int64_t days = calendar::days_from_civil(year, month, day);
    (void)days;
    STX_VACUITY_PROBE();
    return 0;
}
