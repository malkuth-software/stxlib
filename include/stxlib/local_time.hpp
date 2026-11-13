#pragma once

// Time zones and the host's local time zone (Capability::local_time_zone).
// CivilTime, LocalTime and local_time are ported from stx_auxlib's
// include/stx_auxlib/calendar.hpp at b6e330a; the zones are new. The rest of
// stx_auxlib's calendar (civil arithmetic, weekdays, ISO 8601 text) is
// host-free and comes with stxlib::algorithms (batch 3), over the CivilTime
// defined here.
//
// Owner decision D12 = a (decisions/2026-11-13-d11-d12-threads-clocks.md):
// zones come from the IANA time zone database compiled into STXlib
// (third_party/tzdata, release 2026f; tools/tzcompile.py writes the tables
// src/core/zones.cpp reads), and the host supplies only the zone's name.
// A zone's rules, and so a local time, are the same on every host.
//
// The host's zone (host_zone):
//   - TZ, when set, on every host: an IANA name ("Europe/Berlin"), with or
//     without zic's leading ':', or a path to a compiled zone file whose
//     last "zoneinfo/" is followed by the name (a following "posix/" or
//     "right/" directory is dropped: both name the same zone here, leap
//     seconds being absent). An empty TZ, or ":" alone, is UTC, as on
//     POSIX hosts. A POSIX rule string ("EST5EDT,M3.2.0,M11.1.0") is not a
//     name and is refused.
//   - otherwise, on POSIX hosts, the target of the /etc/localtime link
//     (read as a path, as above), else the first line of /etc/timezone;
//     with neither, UTC, as POSIX hosts treat a missing zone;
//   - otherwise, on Windows, the current dynamic time zone's key name
//     ("W. Europe Standard Time"), mapped to its IANA name by the system's
//     ICU (icu.dll, Windows 10 version 1703 and later), for region "001".
// TZ is read on every call, so a change made through set_variable or
// remove_variable (environment.hpp) is seen by the next one.
//
// Range: every WallTime, which counts signed 64-bit nanoseconds from 1970,
// so from 1677-09-21T00:12:43Z to 2262-04-11T23:47:16Z (Python's datetime).
// Before a zone's first transition the database gives its local mean time
// (for example -04:56:02 for America/New_York); "Etc/" zones and UTC have
// none. Leap seconds do not exist here (Unix time).

#include "stxlib/clocks.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
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

// A zone's reading at one instant: the local time type in effect.
struct ZoneReading final {
    // Seconds east of UTC (local = UTC + offset), daylight saving included.
    i32 offset_seconds{0};
    // Whether the database marks the type as daylight saving time. A zone
    // may observe a negative save in winter (Europe/Dublin), so daylight
    // does not mean the offset is the larger one.
    bool daylight{false};
    // The type's abbreviation ("CET", "EDT", "+0530"): static storage,
    // valid for the whole run.
    TextView abbreviation{};
};

struct LocalTime final {
    CivilTime civil{};
    // Seconds east of UTC (local = UTC + offset), daylight saving included.
    i32 offset_seconds{0};
    bool daylight{false};
    TextView abbreviation{}; // static storage, as ZoneReading's
};

// A zone of the compiled database, found by name. A plain value: copying it
// copies the name's position; it never refers to the host.
class Zone final {
public:
    // UTC ("Etc/UTC").
    Zone() noexcept;

    [[nodiscard]] static Zone utc() noexcept;

    // The zone or link with exactly this name, as the database spells it
    // (case matters: "America/New_York"). Failures: not_found for any other
    // text, the empty text included.
    [[nodiscard]] static Result<Zone> named(TextView name) noexcept;

    // The name it was found by: a link ("US/Eastern") keeps its own name.
    // Static storage.
    [[nodiscard]] TextView name() const noexcept;

    // The local time type in effect at the instant.
    [[nodiscard]] ZoneReading at(WallTime instant) const noexcept;

private:
    explicit Zone(u16 name_index) noexcept : name_index_(name_index) {}

    u16 name_index_;
};

// The database's release ("2026f").
[[nodiscard]] TextView tzdata_version() noexcept;

// The host's zone, by the rules above. Failures: not_found when the name
// the host gives is not in the database (a POSIX rule string, a misspelt
// name) or when /etc/localtime is a file with no name to read and no
// /etc/timezone names it; io_failure with the host's code when its zone
// cannot be read; limit_exceeded with HostCode::too_long for a link target
// or /etc/timezone line of 1024 bytes or more; unsupported on Windows
// without ICU, foreign_failure when ICU refuses the key; the failures of
// read_variable (environment.hpp) for TZ other than not_found, a TZ of
// more than 1024 bytes included.
[[nodiscard]] Result<Zone> host_zone() noexcept;

// The instant in the zone.
[[nodiscard]] LocalTime local_time_in(const Zone& zone, WallTime instant) noexcept;

// The instant in the host's zone: host_zone(), then local_time_in. Failures
// as host_zone's.
[[nodiscard]] Result<LocalTime> local_time(WallTime instant) noexcept;

} // namespace stxlib
