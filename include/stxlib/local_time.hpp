#pragma once

// The host's local time zone (Capability::local_time_zone). Ported from
// stx_auxlib's include/stx_auxlib/calendar.hpp (CivilTime, LocalTime,
// local_time) and src/hosted/calendar_<host>.cpp at b6e330a. The rest of
// stx_auxlib's calendar (civil arithmetic, weekdays, ISO 8601 text) is
// host-free and comes with stxlib::algorithms (batch 3), over the CivilTime
// defined here.
//
// Survey D12 (clocks, time zones and calendars) is open; this is
// stx_auxlib's current behaviour, D12's option b for zones: the zone and its
// rules come from the host (TZ or /etc/localtime through tzset and
// localtime_r on POSIX hosts; the current dynamic time zone through
// SystemTimeToTzSpecificLocalTimeEx on Windows), so two hosts may disagree
// on a local time. D12 a would compile an IANA database into the library.
//
// Range: every WallTime, which counts signed 64-bit nanoseconds from 1970,
// so from 1677-09-21T00:12:43Z to 2262-04-11T23:47:16Z (Python's datetime).
// That range lies wholly after 1601-01-01, the start of Windows' FILETIME,
// so Windows refuses no WallTime. Leap seconds do not exist here (Unix
// time): a host reading of second 60 is shown as 59.
//
// Failures: io_failure with the host's code when the host's conversion
// fails.

#include "stxlib/clocks.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// A date and time in the proleptic Gregorian calendar.
struct CivilTime final {
    i32 year{1970};
    u8 month{1};  // 1 to 12
    u8 day{1};    // 1 to the month's length
    u8 hour{0};   // 0 to 23
    u8 minute{0}; // 0 to 59
    u8 second{0}; // 0 to 59
    u32 nanosecond{0};
};

struct LocalTime final {
    CivilTime civil{};
    // Seconds east of UTC (local = UTC + offset), daylight saving included.
    i32 offset_seconds{0};
};

// The instant in the host's local time zone (its TZ setting or system zone).
// On POSIX hosts a TZ changed through set_variable or remove_variable
// (environment.hpp) is seen by the next call; one changed with the host's
// setenv directly is not (stx_auxlib's rule).
[[nodiscard]] Result<LocalTime> local_time(WallTime instant) noexcept;

} // namespace stxlib
