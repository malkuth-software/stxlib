#pragma once

// Civil-day arithmetic for the local time backends: Howard Hinnant's
// days_from_civil ("chrono-Compatible Low-Level Date Algorithms"), ported
// from stx_auxlib's src/calendar.cpp and src/calendar_detail.hpp at b6e330a
// (days_from_civil, seconds_from_civil, split_wall), unchanged but for
// STXlib's types. stxlib::algorithms (batch 3) carries the full calendar.

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

// Checked at compile time against dates whose day numbers are known.
static_assert(days_from_civil(1970, 1, 1) == 0);
static_assert(days_from_civil(2000, 3, 1) == 11017);
static_assert(days_from_civil(1969, 12, 31) == -1);
static_assert(days_from_civil(1601, 1, 1) == -134774);

} // namespace stxlib::civil
