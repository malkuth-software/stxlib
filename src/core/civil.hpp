#pragma once

// Civil-day arithmetic for the local time backends: Howard Hinnant's
// days_from_civil ("chrono-Compatible Low-Level Date Algorithms"), ported
// from stx_auxlib's src/calendar.cpp and src/calendar_detail.hpp at b6e330a
// (days_from_civil, seconds_from_civil, split_wall), unchanged but for
// STXlib's types; civil_from_days (the same paper) and the helpers after it
// are added for the zone reader (src/core/zones.cpp). stxlib::algorithms (batch 3) carries the full calendar.

#include "stxlib/clocks.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/types.hpp"

namespace stxlib::civil {

inline constexpr i64 nanoseconds_per_second = 1000000000;
inline constexpr i64 seconds_per_day = 86400;

// Days from 1970-01-01 to a proleptic Gregorian date.
[[nodiscard]] constexpr i64 days_from_civil(const i64 year, const u32 month, const u32 day) noexcept {
    const i64 shifted_year = month <= 2 ? year - 1 : year;
    const i64 era = (shifted_year >= 0 ? shifted_year : shifted_year - 399) / 400;
    const i64 year_of_era = shifted_year - era * 400;
    const i64 shifted_month = month > 2 ? static_cast<i64>(month) - 3 : static_cast<i64>(month) + 9;
    const i64 day_of_year = (153 * shifted_month + 2) / 5 + static_cast<i64>(day) - 1;
    const i64 day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const i64 result = era * 146097 + day_of_era - 719468;
    return result;
}

// Whole seconds from 1970-01-01T00:00:00 of a valid civil time, ignoring its
// nanosecond.
[[nodiscard]] constexpr i64 seconds_from_civil(const CivilTime& time) noexcept {
    const i64 days = days_from_civil(time.year, time.month, time.day);
    const i64 result = days * seconds_per_day + static_cast<i64>(time.hour) * 3600 +
                       static_cast<i64>(time.minute) * 60 + static_cast<i64>(time.second);
    return result;
}

// A wall-clock instant as floor seconds and the nanoseconds past them.
struct WallSplit final {
    i64 seconds{0};
    u32 nanosecond{0};
};

[[nodiscard]] constexpr WallSplit split_wall(const WallTime instant) noexcept {
    const i64 value = instant.unix_nanoseconds();
    i64 quotient = value / nanoseconds_per_second;
    i64 remainder = value % nanoseconds_per_second;
    if (remainder < 0) {
        quotient = quotient - 1;
        remainder = remainder + nanoseconds_per_second;
    }
    const WallSplit result{quotient, static_cast<u32>(remainder)};
    return result;
}

// The quotient rounded toward negative infinity. Requirement: divisor > 0.
[[nodiscard]] constexpr i64 floor_divide(const i64 dividend, const i64 divisor) noexcept {
    const i64 quotient = dividend / divisor;
    const i64 result = dividend % divisor < 0 ? quotient - 1 : quotient;
    return result;
}

// The remainder of floor_divide, from 0 to divisor - 1.
[[nodiscard]] constexpr i64 floor_remainder(const i64 dividend, const i64 divisor) noexcept {
    const i64 remainder = dividend % divisor;
    const i64 result = remainder < 0 ? remainder + divisor : remainder;
    return result;
}

struct CivilDate final {
    i64 year{1970};
    u32 month{1};
    u32 day{1};
};

// The proleptic Gregorian date `days` after 1970-01-01: Hinnant's
// civil_from_days (the same paper), the inverse of days_from_civil.
[[nodiscard]] constexpr CivilDate civil_from_days(const i64 days) noexcept {
    const i64 shifted = days + 719468;
    const i64 era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const i64 day_of_era = shifted - era * 146097;
    const i64 year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const i64 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const i64 shifted_month = (5 * day_of_year + 2) / 153;
    const i64 day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    const i64 month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
    const i64 year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
    const CivilDate result{year, static_cast<u32>(month), static_cast<u32>(day)};
    return result;
}

// 0 for Sunday to 6 for Saturday (1970-01-01 was a Thursday).
[[nodiscard]] constexpr u32 weekday_of(const i64 days) noexcept {
    const u32 result = static_cast<u32>(floor_remainder(days + 4, 7));
    return result;
}

// The civil time `seconds` after 1970-01-01T00:00:00, nanosecond 0.
// Requirement: the year fits i32 (every WallTime plus a day does).
[[nodiscard]] constexpr CivilTime civil_from_seconds(const i64 seconds) noexcept {
    const i64 days = floor_divide(seconds, seconds_per_day);
    const i64 second_of_day = seconds - days * seconds_per_day;
    const CivilDate date = civil_from_days(days);
    CivilTime result{};
    result.year = static_cast<i32>(date.year);
    result.month = static_cast<u8>(date.month);
    result.day = static_cast<u8>(date.day);
    result.hour = static_cast<u8>(second_of_day / 3600);
    result.minute = static_cast<u8>(second_of_day / 60 % 60);
    result.second = static_cast<u8>(second_of_day % 60);
    return result;
}

// Checked at compile time against dates whose day numbers are known.
static_assert(days_from_civil(1970, 1, 1) == 0);
static_assert(days_from_civil(2000, 3, 1) == 11017);
static_assert(days_from_civil(1969, 12, 31) == -1);
static_assert(days_from_civil(1601, 1, 1) == -134774);
static_assert(civil_from_days(-1).year == 1969 && civil_from_days(-1).month == 12 && civil_from_days(-1).day == 31);
static_assert(civil_from_days(11017).year == 2000 && civil_from_days(11017).month == 3);
static_assert(civil_from_days(-134774).year == 1601 && civil_from_days(-134774).day == 1);
static_assert(weekday_of(0) == 4 && weekday_of(-1) == 3);
static_assert(floor_divide(-1, 86400) == -1 && floor_remainder(-1, 86400) == 86399);

} // namespace stxlib::civil
