// Tests of the local time zone (include/stxlib/local_time.hpp). The POSIX
// part ports local_time_rules from stx_auxlib's tests/calendar_tests.cpp at
// b6e330a: TZ rules fix the zone without the host's zone files. Windows has
// no TZ rule here (the system zone is used), so it is checked for agreement
// between the civil reading and its offset, and for its refusal before 1601.
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

namespace {

using stxlib::CivilTime;
using stxlib::i64;
using stxlib::LocalTime;
using stxlib::Result;
using stxlib::WallTime;

// Days from 1970-01-01 to a proleptic Gregorian date (Howard Hinnant's
// days_from_civil, "chrono-Compatible Low-Level Date Algorithms"), written
// out again here so the test does not share the library's copy.
i64 days_from_civil(const i64 year_in, const i64 month, const i64 day) {
    const i64 year = month <= 2 ? year_in - 1 : year_in;
    const i64 era = (year >= 0 ? year : year - 399) / 400;
    const i64 year_of_era = year - era * 400;
    const i64 day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const i64 day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const i64 result = era * 146097 + day_of_era - 719468;
    return result;
}

i64 seconds_of(const CivilTime& civil) {
    const i64 days = days_from_civil(civil.year, civil.month, civil.day);
    const i64 result = days * 86400 + civil.hour * 3600 + civil.minute * 60 + civil.second;
    return result;
}

// The reading and its offset name the instant they were made from.
bool consistent(const LocalTime& local, const WallTime instant) {
    const bool result = seconds_of(local.civil) - local.offset_seconds == instant.unix_seconds() &&
                        static_cast<i64>(local.civil.nanosecond) == instant.unix_nanoseconds() -
                                                                         instant.unix_seconds() * 1000000000;
    return result;
}

constexpr i64 nanoseconds_per_second = 1000000000;

void reference_values() {
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(days_from_civil(1969, 12, 31) == -1);
    CHECK(days_from_civil(1601, 1, 1) == -134774);
    CHECK(days_from_civil(2026, 1, 15) * 86400 + 12 * 3600 == 1768478400);
}

void any_zone() {
    CHECK(stxlib::capability_supported(stxlib::Capability::local_time_zone));
    const WallTime now = WallTime::now();
    const Result<LocalTime> local = stxlib::local_time(now);
    CHECK(local.succeeded());
    if (local.succeeded()) {
        const LocalTime& reading = *local.value();
        CHECK(consistent(reading, now));
        // Zones in use lie between -12:00 and +14:00.
        CHECK(reading.offset_seconds >= -12 * 3600 && reading.offset_seconds <= 14 * 3600);
        CHECK(reading.civil.month >= 1 && reading.civil.month <= 12);
        CHECK(reading.civil.second <= 59);
    }
    // Instants before 1970 convert too.
    const WallTime before_epoch = WallTime::from_unix_nanoseconds(-nanoseconds_per_second / 2);
    const Result<LocalTime> earlier = stxlib::local_time(before_epoch);
    CHECK(earlier.succeeded());
    if (earlier.succeeded()) CHECK(consistent(*earlier.value(), before_epoch));
}

#if defined(_WIN32)
void windows_range() {
    // FILETIME starts at 1601-01-01; one second earlier is refused.
    const WallTime first = WallTime::from_unix_nanoseconds(-11644473600 * nanoseconds_per_second);
    const WallTime too_early = WallTime::from_unix_nanoseconds(-11644473601 * nanoseconds_per_second);
    const Result<LocalTime> refused = stxlib::local_time(too_early);
    CHECK(refused.failed());
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument);
    // 1601-01-01T00:00Z itself may fall before 1601 in a zone west of UTC,
    // where the host may refuse it; it must not be misread.
    const Result<LocalTime> edge = stxlib::local_time(first);
    if (edge.succeeded()) CHECK(consistent(*edge.value(), first));
}
#else
void posix_rules() {
    // 2026-01-15T12:00:00.25Z and 2026-07-15T12:00:00Z (Python's datetime).
    constexpr WallTime winter = WallTime::from_unix_nanoseconds(1768478400 * nanoseconds_per_second + 250000000);
    constexpr WallTime summer = WallTime::from_unix_nanoseconds(1784116800 * nanoseconds_per_second);
    const Result<stxlib::Utf8String> saved = stxlib::read_variable("TZ", 256);
    const bool had_zone = saved.succeeded();

    CHECK(stxlib::set_variable("TZ", "UTC0").succeeded());
    const Result<LocalTime> utc = stxlib::local_time(winter);
    CHECK(utc.succeeded());
    if (utc.succeeded()) {
        const LocalTime& reading = *utc.value();
        CHECK(reading.offset_seconds == 0);
        CHECK(reading.civil.year == 2026 && reading.civil.month == 1 && reading.civil.day == 15);
        CHECK(reading.civil.hour == 12 && reading.civil.minute == 0 && reading.civil.second == 0);
        CHECK(reading.civil.nanosecond == 250000000);
    }

    // A changed TZ is seen by the next call (environment generation).
    CHECK(stxlib::set_variable("TZ", "EST5EDT,M3.2.0,M11.1.0").succeeded());
    const Result<LocalTime> eastern_winter = stxlib::local_time(winter);
    CHECK(eastern_winter.succeeded());
    if (eastern_winter.succeeded()) {
        const LocalTime& reading = *eastern_winter.value();
        CHECK(reading.offset_seconds == -18000);
        CHECK(reading.civil.hour == 7 && reading.civil.nanosecond == 250000000);
        CHECK(consistent(reading, winter));
    }
    const Result<LocalTime> eastern_summer = stxlib::local_time(summer);
    CHECK(eastern_summer.succeeded());
    if (eastern_summer.succeeded()) {
        const LocalTime& reading = *eastern_summer.value();
        CHECK(reading.offset_seconds == -14400);
        CHECK(reading.civil.hour == 8);
        CHECK(consistent(reading, summer));
    }

    CHECK(stxlib::set_variable("TZ", "<+0530>-5:30").succeeded());
    const Result<LocalTime> india = stxlib::local_time(winter);
    CHECK(india.succeeded());
    if (india.succeeded()) {
        const LocalTime& reading = *india.value();
        CHECK(reading.offset_seconds == 19800);
        CHECK(reading.civil.hour == 17 && reading.civil.minute == 30);
        CHECK(consistent(reading, winter));
    }

    if (had_zone) {
        CHECK(stxlib::set_variable("TZ", (*saved.value()).view()).succeeded());
    } else {
        CHECK(stxlib::remove_variable("TZ").succeeded());
    }
}
#endif

} // namespace

int main() {
    reference_values();
    any_zone();
#if defined(_WIN32)
    windows_range();
#else
    posix_rules();
#endif
    return stxlib_tests::finish("local_time");
}
