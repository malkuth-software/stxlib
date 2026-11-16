// Tests of time zones and the local time zone (include/stxlib/local_time.hpp,
// owner decision D12 = a). The compiled zones are checked against vectors
// that tools/tzvectors.py takes from tools/tzcompile.py's full expansion of
// the tzdata source (tests/zone_vectors.inc); the host's zone is checked by
// naming zones through TZ, which every host honours. The civil conversion
// is checked at every day of WallTime's range. The vectors and the TZ cases
// are tested, not proved (proofs/REPORT.md); the civil check is exhaustive
// over its domain.

#include "stxlib/capabilities.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <cstdio>

namespace {

using stxlib::CivilTime;
using stxlib::i64;
using stxlib::LocalTime;
using stxlib::OperationResult;
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

// The reading and its offset name the instant they were made from. The
// whole seconds are the floor of the nanoseconds, and the nanosecond field
// what lies above them, computed without multiplying back (at i64_min the
// floor times 10^9 is outside i64).
bool consistent(const LocalTime& local, const WallTime instant) {
    const i64 nanoseconds = instant.unix_nanoseconds();
    const i64 remainder = nanoseconds % 1000000000;
    const i64 floor_seconds = nanoseconds / 1000000000 - (remainder < 0 ? 1 : 0);
    const i64 above = remainder < 0 ? remainder + 1000000000 : remainder;
    const bool result = instant.unix_seconds() == floor_seconds &&
                        seconds_of(local.civil) - local.offset_seconds == floor_seconds &&
                        static_cast<i64>(local.civil.nanosecond) == above;
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
    const OperationResult<LocalTime> local = stxlib::local_time(now);
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
    const OperationResult<LocalTime> earlier = stxlib::local_time(before_epoch);
    CHECK(earlier.succeeded());
    if (earlier.succeeded()) CHECK(consistent(*earlier.value(), before_epoch));
}

// The ends of WallTime's range convert in any zone and agree with their
// offsets (CI run 43435039771: the earlier test of an instant before 1601
// overflowed i64 nanoseconds and named a time inside the range instead;
// WallTime cannot express 1601).
void range_ends() {
    const WallTime earliest = WallTime::from_unix_nanoseconds(stxlib::i64_min);
    const WallTime latest = WallTime::from_unix_nanoseconds(stxlib::i64_max);
    const OperationResult<LocalTime> first = stxlib::local_time(earliest);
    CHECK(first.succeeded());
    if (first.succeeded()) {
        CHECK(consistent(*first.value(), earliest));
        CHECK((*first.value()).civil.year == 1677 || (*first.value()).civil.year == 1678);
    }
    const OperationResult<LocalTime> last = stxlib::local_time(latest);
    CHECK(last.succeeded());
    if (last.succeeded()) {
        CHECK(consistent(*last.value(), latest));
        CHECK((*last.value()).civil.year == 2262);
    }
}

// The failure's kind, or 0 (no kind) on success.
template <typename Value>
stxlib::ErrorKind kind_of(const OperationResult<Value>& result) {
    if (result.succeeded()) return static_cast<stxlib::ErrorKind>(0);
    const stxlib::ErrorKind kind = (*result.error()).kind();
    return kind;
}

struct ZoneVector final {
    const char* name;
    i64 seconds;
    stxlib::i32 offset;
    int daylight;
    const char* abbreviation;
};

#include "zone_vectors.inc"

stxlib::TextView view_of_ascii(const char* const text) {
    stxlib::usize count = 0;
    while (text[count] != '\0') ++count;
    // The vectors are ASCII, so the bytes are valid UTF-8.
    const stxlib::TextView result = stxlib::detail::TextViewAccess::adopt_validated(text, count);
    return result;
}

// The WallTime of a whole Unix second; the earliest second of the range
// (-9223372037) is below i64_min nanoseconds, so it is read at i64_min,
// which lies inside that second.
WallTime wall_of_seconds(const i64 seconds) {
    i64 nanoseconds = 0;
    if (__builtin_mul_overflow(seconds, nanoseconds_per_second, &nanoseconds)) nanoseconds = stxlib::i64_min;
    const WallTime result = WallTime::from_unix_nanoseconds(nanoseconds);
    return result;
}

void compiled_vectors() {
    stxlib::usize failures = 0;
    for (const ZoneVector& vector : zone_vectors) {
        const OperationResult<stxlib::Zone> zone = stxlib::Zone::named(view_of_ascii(vector.name));
        if (zone.failed()) {
            ++failures;
            continue;
        }
        const stxlib::ZoneReading reading = (*zone.value()).at(wall_of_seconds(vector.seconds));
        const bool agrees = reading.offset_seconds == vector.offset && reading.daylight == (vector.daylight != 0) &&
                            reading.abbreviation == view_of_ascii(vector.abbreviation);
        if (!agrees) {
            if (failures < 10) {
                std::printf("zone vector differs: %s at %lld\n", vector.name, static_cast<long long>(vector.seconds));
            }
            ++failures;
        }
    }
    CHECK(failures == 0);
}

void names() {
    CHECK(stxlib::tzdata_version() == stxlib::TextView("2026f"));
    CHECK(stxlib::Zone::utc().name() == stxlib::TextView("Etc/UTC"));
    const stxlib::Zone default_zone;
    CHECK(default_zone.name() == stxlib::TextView("Etc/UTC"));
    const stxlib::ZoneReading utc = default_zone.at(WallTime::now());
    CHECK(utc.offset_seconds == 0 && !utc.daylight && utc.abbreviation == stxlib::TextView("UTC"));
    // A link keeps its own name and reads as its zone.
    const OperationResult<stxlib::Zone> link = stxlib::Zone::named("US/Eastern");
    const OperationResult<stxlib::Zone> target = stxlib::Zone::named("America/New_York");
    CHECK(link.succeeded() && target.succeeded());
    if (link.succeeded() && target.succeeded()) {
        CHECK((*link.value()).name() == stxlib::TextView("US/Eastern"));
        const WallTime instant = wall_of_seconds(1793512800);
        CHECK((*link.value()).at(instant).offset_seconds == (*target.value()).at(instant).offset_seconds);
    }
    CHECK(kind_of(stxlib::Zone::named("america/new_york")) == stxlib::ErrorKind::not_found);
    CHECK(kind_of(stxlib::Zone::named("")) == stxlib::ErrorKind::not_found);
    CHECK(kind_of(stxlib::Zone::named("America/New_York ")) == stxlib::ErrorKind::not_found);
    CHECK(kind_of(stxlib::Zone::named("Mars/Olympus_Mons")) == stxlib::ErrorKind::not_found);
}

// Readings with their civil time, from tzcompile.py --dump.
void zone_readings() {
    // 2026-11-01T06:00:00Z: New York's return to EST; one second earlier it
    // is still 01:59:59 EDT.
    const OperationResult<stxlib::Zone> new_york = stxlib::Zone::named("America/New_York");
    CHECK(new_york.succeeded());
    if (new_york.succeeded()) {
        const LocalTime after = stxlib::local_time_in(*new_york.value(), wall_of_seconds(1793512800));
        CHECK(after.offset_seconds == -18000 && !after.daylight && after.abbreviation == stxlib::TextView("EST"));
        CHECK(after.civil.year == 2026 && after.civil.month == 11 && after.civil.day == 1 && after.civil.hour == 1);
        CHECK(after.civil.minute == 0 && after.civil.second == 0);
        const LocalTime before = stxlib::local_time_in(*new_york.value(), wall_of_seconds(1793512799));
        CHECK(before.offset_seconds == -14400 && before.daylight && before.abbreviation == stxlib::TextView("EDT"));
        CHECK(before.civil.hour == 1 && before.civil.minute == 59 && before.civil.second == 59);
        // Before its first transition a zone keeps local mean time.
        const LocalTime early = stxlib::local_time_in(*new_york.value(), wall_of_seconds(-3000000000));
        CHECK(early.offset_seconds == -17762 && early.abbreviation == stxlib::TextView("LMT"));
    }
    // Europe/Dublin observes a negative save in winter (tzdata's main form):
    // GMT is its daylight type there.
    const OperationResult<stxlib::Zone> dublin = stxlib::Zone::named("Europe/Dublin");
    CHECK(dublin.succeeded());
    if (dublin.succeeded()) {
        const LocalTime winter = stxlib::local_time_in(*dublin.value(), wall_of_seconds(9208947600));
        CHECK(winter.offset_seconds == 0 && winter.daylight && winter.abbreviation == stxlib::TextView("GMT"));
        CHECK(winter.civil.year == 2261);
    }
    const OperationResult<stxlib::Zone> kolkata = stxlib::Zone::named("Asia/Kolkata");
    CHECK(kolkata.succeeded());
    if (kolkata.succeeded()) {
        constexpr WallTime winter = WallTime::from_unix_nanoseconds(1768478400 * nanoseconds_per_second + 250000000);
        const LocalTime reading = stxlib::local_time_in(*kolkata.value(), winter);
        CHECK(reading.offset_seconds == 19800 && reading.abbreviation == stxlib::TextView("IST"));
        CHECK(reading.civil.hour == 17 && reading.civil.minute == 30 && reading.civil.nanosecond == 250000000);
        CHECK(consistent(reading, winter));
    }
}

// Every day of WallTime's range, at a time of day that varies with the
// day, converts in UTC to the civil time that names it, with a valid month
// and day. Exhaustive over the days of the range (civil_from_days's domain
// here).
void civil_every_day() {
    const i64 first_day = -106752;
    const i64 last_day = 106751;
    stxlib::usize failures = 0;
    const stxlib::Zone utc = stxlib::Zone::utc();
    for (i64 day = first_day; day <= last_day; ++day) {
        const i64 seconds = day * 86400 + ((day * 7919) % 86400 + 86400) % 86400;
        if (seconds < -9223372036 || seconds > 9223372036) continue;
        const WallTime instant = wall_of_seconds(seconds);
        const LocalTime local = stxlib::local_time_in(utc, instant);
        const bool valid = local.civil.month >= 1 && local.civil.month <= 12 && local.civil.day >= 1 &&
                           local.civil.day <= 31 && seconds_of(local.civil) == seconds;
        if (!valid) ++failures;
    }
    CHECK(failures == 0);
}

// The host's zone through TZ, which every host honours.
void host_zone_by_tz() {
    constexpr WallTime winter = WallTime::from_unix_nanoseconds(1768478400 * nanoseconds_per_second + 250000000);
    const OperationResult<stxlib::Utf8String> saved = stxlib::read_variable("TZ", 4096);
    const bool had_zone = saved.succeeded();
    const struct Case {
        stxlib::TextView tz;
        stxlib::i32 offset;
        stxlib::TextView name;
    } cases[] = {
        {"America/New_York", -18000, "America/New_York"},
        {":Europe/Berlin", 3600, "Europe/Berlin"},
        {"/usr/share/zoneinfo/Asia/Kolkata", 19800, "Asia/Kolkata"},
        {"/usr/share/zoneinfo/posix/Asia/Tokyo", 32400, "Asia/Tokyo"},
        {"/var/db/timezone/zoneinfo/right/Australia/Adelaide", 37800, "Australia/Adelaide"},
        {"", 0, "Etc/UTC"},
        {":", 0, "Etc/UTC"},
        {"UTC", 0, "UTC"},
    };
    for (const Case& item : cases) {
        CHECK(stxlib::set_variable("TZ", item.tz).succeeded());
        const OperationResult<stxlib::Zone> zone = stxlib::host_zone();
        CHECK(zone.succeeded());
        if (zone.succeeded()) CHECK((*zone.value()).name() == item.name);
        const OperationResult<LocalTime> local = stxlib::local_time(winter);
        CHECK(local.succeeded());
        if (local.succeeded()) {
            CHECK((*local.value()).offset_seconds == item.offset);
            CHECK(consistent(*local.value(), winter));
        }
    }
    // POSIX rule strings and unknown names are no database names.
    // So is a path without "zoneinfo/" other than the host's own zone file.
    const stxlib::TextView refused[] = {"EST5EDT,M3.2.0,M11.1.0", "UTC0", "<+0530>-5:30", "Nowhere/Special",
                                        "/etc/other-localtime", ":/etc/localtime.bak", "/etc/localtime/"};
    for (const stxlib::TextView text : refused) {
        CHECK(stxlib::set_variable("TZ", text).succeeded());
        CHECK(kind_of(stxlib::host_zone()) == stxlib::ErrorKind::not_found);
        CHECK(kind_of(stxlib::local_time(winter)) == stxlib::ErrorKind::not_found);
    }
    // TZ naming the host's own zone file, with or without ':', reads the
    // zone as an unset TZ does (A-stxlib-core-01-F6).
    CHECK(stxlib::remove_variable("TZ").succeeded());
    const OperationResult<stxlib::Zone> unset = stxlib::host_zone();
    const stxlib::TextView own_file[] = {"/etc/localtime", ":/etc/localtime"};
    for (const stxlib::TextView text : own_file) {
        CHECK(stxlib::set_variable("TZ", text).succeeded());
        const OperationResult<stxlib::Zone> zone = stxlib::host_zone();
        CHECK(zone.succeeded() == unset.succeeded());
        if (zone.succeeded() && unset.succeeded()) CHECK((*zone.value()).name() == (*unset.value()).name());
        if (zone.failed() && unset.failed()) CHECK(kind_of(zone) == kind_of(unset));
    }
    if (had_zone) {
        CHECK(stxlib::set_variable("TZ", (*saved.value()).view()).succeeded());
    } else {
        CHECK(stxlib::remove_variable("TZ").succeeded());
    }
}


} // namespace

int main() {
    reference_values();
    any_zone();
    range_ends();
    compiled_vectors();
    names();
    zone_readings();
    civil_every_day();
    host_zone_by_tz();
    return stxlib_tests::finish("local_time");
}
