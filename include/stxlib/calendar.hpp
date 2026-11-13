#pragma once

// Calendar arithmetic and ISO 8601 text over CivilTime (local_time.hpp),
// replacing gmtime, mktime, timegm, strftime and std::put_time. Ported from
// stx_auxlib's include/stx_auxlib/calendar.hpp at b6e330a: the civil-day
// algorithms (Hinnant's), the validity rules, the ISO 8601 forms and the
// parse rules are unchanged. The interface is restated in STXlib's
// vocabulary: WallTime for instants, Result in place of std::optional,
// text written into caller storage (numeric_text.hpp's Formatted), TextView
// in. CivilTime, LocalTime and the local time zone stayed in the boundary
// (local_time.hpp); this header is host-free.
//
// Range: WallTime counts signed 64-bit nanoseconds from 1970, so civil
// times from 1677-09-21T00:12:43.145224192Z to
// 2262-04-11T23:47:16.854775807Z convert; others are refused. Leap seconds
// do not exist here (Unix time): second 60 is refused.

#include "stxlib/clocks.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// ISO 8601 numbering: Monday is 1, Sunday 7.
enum class Weekday : u8 {
    monday = 1,
    tuesday = 2,
    wednesday = 3,
    thursday = 4,
    friday = 5,
    saturday = 6,
    sunday = 7,
};

[[nodiscard]] bool is_leap_year(i32 year) noexcept;
// 0 for a month outside 1 to 12.
[[nodiscard]] u8 days_in_month(i32 year, u8 month) noexcept;
// True when every field is in range (the date exists, hour < 24,
// minute < 60, second < 60, nanosecond < 10^9); says nothing about whether
// it fits WallTime.
[[nodiscard]] bool is_valid_civil_time(const CivilTime& time) noexcept;
// Requirement for the next two: the date exists (is_valid_civil_time, or at
// least a valid year, month and day).
[[nodiscard]] Weekday weekday_of(const CivilTime& time) noexcept;
// 1 for January 1.
[[nodiscard]] u16 day_of_year(const CivilTime& time) noexcept;

// The UTC civil time of an instant.
[[nodiscard]] CivilTime civil_from_wall(WallTime instant) noexcept;
// The instant of a UTC civil time. Failures: invalid_argument when the time
// is not valid (is_valid_civil_time); out_of_range when it is valid but
// outside WallTime's range.
[[nodiscard]] Result<WallTime> wall_from_civil(const CivilTime& time) noexcept;

enum class ZoneDesignator : u8 {
    // No zone ("2026-10-31 21:30:05"), as "%Y-%m-%d %H:%M:%S" prints.
    none = 0,
    // "Z" for offset zero, "+02:00" otherwise.
    designated = 1,
};

struct Iso8601Format final {
    // The character between date and time: 'T' (ISO 8601) or ' ' (RFC 3339
    // allows it, and it reads more easily). Requirement: ASCII.
    char separator{'T'};
    // Digits of the second's fraction, 0 to 9 (more are treated as 9).
    u8 fraction_digits{0};
    ZoneDesignator zone{ZoneDesignator::designated};
};

// The most bytes format_iso8601 writes: a sign and ten year digits, "-MM-DD",
// the separator, "HH:MM:SS", nine fraction digits with their point, and
// "+HH:MM".
inline constexpr usize maximum_iso8601_bytes = 1 + 10 + 6 + 1 + 8 + 10 + 6;

// Writes "YYYY-MM-DD<separator>HH:MM:SS[.fff][zone]" into `output`, whole or
// not at all (text::Formatted). Years outside 0 to 9999 are written with a
// sign and at least four digits ("-0042", "+12345"), as ISO 8601's expanded
// form does. ISO 8601 offsets are whole minutes; any seconds of the offset
// are dropped. The fields are written as given: pass a valid time (a field
// out of range is written as its two low decimal digits).
[[nodiscard]] text::Formatted format_iso8601(MutableView<char> output, const CivilTime& time, i32 offset_seconds,
                                             const Iso8601Format& format = {}) noexcept;

// Parses "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)", with 'T', 't'
// or ' ' between date and time, up to nine fraction digits, and 'Z' or 'z',
// as an instant. A zone is required, since text without one names no
// instant. Failures: empty; invalid for any other form or an invalid civil
// time; trailing_text after a complete form; out_of_range outside WallTime.
[[nodiscard]] text::Parsed<WallTime> parse_iso8601(TextView value) noexcept;

} // namespace stxlib
