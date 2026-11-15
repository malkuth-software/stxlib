/* Reference day count for the calendar rows (12, 13), in C so any
 * harness can share it.
 *
 * Independent of Hinnant's algorithm (which the treatment uses): days from 1970-01-01 are 365 per year plus the leap years counted
 * by inclusion and exclusion, L(x) = floor(x/4) - floor(x/100) +
 * floor(x/400), which counts the multiples in (0, x] and, by floor, extends
 * to every integer x, so the leap years in [a, b] are L(b) - L(a - 1). The
 * proleptic Gregorian rule: a year is leap when divisible by 4, except
 * centuries, except multiples of 400.
 *
 * Domain: years in int32 (every intermediate then fits int64).
 */
#ifndef STX_B1_CALENDAR_SPEC_H
#define STX_B1_CALENDAR_SPEC_H

#if defined(__cplusplus)
#include <cstdint>
#else
#include <stdbool.h>
#include <stdint.h>
#endif

static inline int64_t stx_spec_floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if (value % divisor < 0) quotient -= 1;
    return quotient;
}

static inline int64_t stx_spec_leaps_through(int64_t year)
{
    return stx_spec_floor_div(year, 4) - stx_spec_floor_div(year, 100) + stx_spec_floor_div(year, 400);
}

static inline bool stx_spec_is_leap(int64_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static inline uint32_t stx_spec_days_in_month(int64_t year, uint32_t month)
{
    static const uint32_t lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && stx_spec_is_leap(year)) return 29;
    return lengths[month - 1];
}

/* Days from 1970-01-01 to year-month-day; month 1-12, day 1-31 (a day past
 * the month's end counts on into the next month, as the formulas under test
 * do). */
static inline int64_t stx_spec_days(int64_t year, uint32_t month, uint32_t day)
{
    static const int64_t before_month[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    const int64_t years = 365 * (year - 1970) + (stx_spec_leaps_through(year - 1) - stx_spec_leaps_through(1969));
    int64_t days = years + before_month[month - 1] + (int64_t)day - 1;
    if (month > 2 && stx_spec_is_leap(year)) days += 1;
    return days;
}

#endif /* STX_B1_CALENDAR_SPEC_H */
