// Tests of the calendar (include/stxlib/calendar.hpp). Ported from
// stx_auxlib's tests/calendar_tests.cpp at b6e330a with the same
// expectations. The civil-day algorithms are cross-checked against
// std::chrono's year_month_day, which the standard fixes exactly, over
// every day WallTime can hold (one instant per day, at noon: exhaustive
// over days, not over times of day). stx_auxlib's POSIX TZ rule checks are
// not carried: STXlib reads zones from its compiled database
// (local_time.hpp, tested in local_time_tests); one check here writes a
// zone's local time as ISO 8601 text.

#include "stxlib/calendar.hpp"
#include "stxlib/local_time.hpp"
#include "stxlib/text.hpp"

#include "core/checked.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace {

using stxlib::CivilTime;
using stxlib::i32;
using stxlib::i64;
using stxlib::TextView;
using stxlib::u64;
using stxlib::WallTime;

CivilTime civil(const i32 year, const unsigned month, const unsigned day, const unsigned hour, const unsigned minute,
                const unsigned second, const stxlib::u32 nanosecond) {
    CivilTime result{};
    result.year = year;
    result.month = static_cast<stxlib::u8>(month);
    result.day = static_cast<stxlib::u8>(day);
    result.hour = static_cast<stxlib::u8>(hour);
    result.minute = static_cast<stxlib::u8>(minute);
    result.second = static_cast<stxlib::u8>(second);
    result.nanosecond = nanosecond;
    return result;
}

bool same(const CivilTime& left, const CivilTime& right) {
    const bool result = left.year == right.year && left.month == right.month && left.day == right.day &&
                        left.hour == right.hour && left.minute == right.minute && left.second == right.second &&
                        left.nanosecond == right.nanosecond;
    return result;
}

// A 64-bit linear congruential step (Knuth's MMIX constants), modulo 2^64.
STXLIB_MODULAR u64 next_state(const u64 state) {
    const u64 result = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return result;
}

WallTime at(const i64 nanoseconds) {
    const WallTime result = WallTime::from_unix_nanoseconds(nanoseconds);
    return result;
}

// The instant, or i64_min with a failed check.
i64 converted(const CivilTime& time) {
    const stxlib::OperationResult<WallTime> instant = stxlib::wall_from_civil(time);
    CHECK(instant.succeeded());
    if (!instant.succeeded()) return std::numeric_limits<i64>::min();
    const i64 result = (*instant.value()).unix_nanoseconds();
    return result;
}

bool refused_as(const CivilTime& time, const stxlib::ErrorKind kind) {
    const stxlib::OperationResult<WallTime> instant = stxlib::wall_from_civil(time);
    const bool result = instant.failed() && (*instant.error()).kind() == kind;
    return result;
}

void against_standard_days() {
    bool dates = true;
    bool weekdays = true;
    bool round_trips = true;
    // WallTime spans about +-106,751 days around 1970.
    for (i64 day = -106750; day <= 106750; ++day) {
        const std::chrono::sys_days standard_day{std::chrono::days(day)};
        const std::chrono::year_month_day expected(standard_day);
        const i64 noon_nanoseconds = (day * 86400 + 43200) * 1000000000LL;
        const CivilTime noon = stxlib::civil_from_wall(at(noon_nanoseconds));
        dates = dates && noon.year == static_cast<int>(expected.year()) &&
                noon.month == static_cast<unsigned>(expected.month()) &&
                noon.day == static_cast<unsigned>(expected.day()) && noon.hour == 12;
        const std::chrono::weekday standard_weekday(standard_day);
        weekdays = weekdays && static_cast<unsigned>(stxlib::weekday_of(noon)) == standard_weekday.iso_encoding();
        const stxlib::OperationResult<WallTime> back = stxlib::wall_from_civil(noon);
        round_trips = round_trips && back.succeeded() && (*back.value()).unix_nanoseconds() == noon_nanoseconds;
    }
    CHECK(dates);
    CHECK(weekdays);
    CHECK(round_trips);
}

void calendar_rules() {
    CHECK(stxlib::is_leap_year(2000) && stxlib::is_leap_year(2024) && !stxlib::is_leap_year(1900) &&
          !stxlib::is_leap_year(2026));
    CHECK(stxlib::days_in_month(2024, 2) == 29 && stxlib::days_in_month(2026, 2) == 28 &&
          stxlib::days_in_month(2026, 4) == 30 && stxlib::days_in_month(2026, 13) == 0 &&
          stxlib::days_in_month(2026, 0) == 0);
    CHECK(stxlib::weekday_of(civil(2026, 10, 31, 0, 0, 0, 0)) == stxlib::Weekday::saturday);
    CHECK(stxlib::weekday_of(civil(1970, 1, 1, 0, 0, 0, 0)) == stxlib::Weekday::thursday);
    CHECK(stxlib::day_of_year(civil(2024, 12, 31, 0, 0, 0, 0)) == 366);
    CHECK(stxlib::day_of_year(civil(2026, 1, 1, 0, 0, 0, 0)) == 1);
    CHECK(!stxlib::is_valid_civil_time(civil(2026, 2, 29, 0, 0, 0, 0)));
    CHECK(!stxlib::is_valid_civil_time(civil(2026, 1, 1, 24, 0, 0, 0)));
    CHECK(!stxlib::is_valid_civil_time(civil(2026, 1, 1, 0, 0, 60, 0)));
    CHECK(!stxlib::is_valid_civil_time(civil(2026, 1, 1, 0, 0, 0, 1000000000)));
}

void instants() {
    CHECK(same(stxlib::civil_from_wall(at(-500000000)), civil(1969, 12, 31, 23, 59, 59, 500000000)));
    const CivilTime latest = civil(2262, 4, 11, 23, 47, 16, 854775807);
    const CivilTime earliest = civil(1677, 9, 21, 0, 12, 43, 145224192);
    CHECK(same(stxlib::civil_from_wall(at(std::numeric_limits<i64>::max())), latest));
    CHECK(same(stxlib::civil_from_wall(at(std::numeric_limits<i64>::min())), earliest));
    CHECK(converted(latest) == std::numeric_limits<i64>::max());
    CHECK(converted(earliest) == std::numeric_limits<i64>::min());
    CivilTime past_latest = latest;
    past_latest.nanosecond += 1;
    CivilTime before_earliest = earliest;
    before_earliest.nanosecond -= 1;
    CHECK(refused_as(past_latest, stxlib::ErrorKind::out_of_range));
    CHECK(refused_as(before_earliest, stxlib::ErrorKind::out_of_range));
    CHECK(refused_as(civil(2026, 2, 29, 0, 0, 0, 0), stxlib::ErrorKind::invalid_argument));

    u64 state = 1;
    bool round_trips = true;
    for (int sample = 0; sample < 100000; ++sample) {
        state = next_state(state);
        const i64 nanoseconds = static_cast<i64>(state);
        const stxlib::OperationResult<WallTime> back =
            stxlib::wall_from_civil(stxlib::civil_from_wall(at(nanoseconds)));
        round_trips = round_trips && back.succeeded() && (*back.value()).unix_nanoseconds() == nanoseconds;
    }
    CHECK(round_trips);
}

char storage[stxlib::maximum_iso8601_bytes];

std::string iso(const CivilTime& time, const i32 offset, const stxlib::Iso8601Format& format) {
    const stxlib::text::Formatted formatted =
        stxlib::format_iso8601(stxlib::MutableView<char>(storage), time, offset, format);
    CHECK(formatted.succeeded());
    if (!formatted.succeeded()) return std::string("<too small>");
    const TextView text = *formatted.text();
    const std::string result(text.data(), text.count());
    return result;
}

void iso8601_text() {
    const CivilTime time = civil(2026, 10, 31, 21, 30, 5, 123456789);
    CHECK(iso(time, 0, {}) == "2026-10-31T21:30:05Z");
    CHECK(iso(time, 0, {.separator = ' ', .zone = stxlib::ZoneDesignator::none}) == "2026-10-31 21:30:05");
    CHECK(iso(time, 7200, {.fraction_digits = 3}) == "2026-10-31T21:30:05.123+02:00");
    CHECK(iso(time, -19800, {.fraction_digits = 9}) == "2026-10-31T21:30:05.123456789-05:30");
    CHECK(iso(civil(-42, 1, 2, 3, 4, 5, 0), 0, {}) == "-0042-01-02T03:04:05Z");
    CHECK(iso(civil(12345, 6, 7, 8, 9, 10, 0), 0, {}) == "+12345-06-07T08:09:10Z");
    // The longest text fits maximum_iso8601_bytes exactly.
    const std::string longest = iso(civil(std::numeric_limits<i32>::min(), 12, 31, 23, 59, 59, 999999999), -86399,
                                    {.fraction_digits = 200});
    CHECK(longest.size() == stxlib::maximum_iso8601_bytes);
    // Too small: nothing written, the size needed reported.
    char small[10] = {};
    const stxlib::text::Formatted refused =
        stxlib::format_iso8601(stxlib::MutableView<char>(small), time, 0, {.fraction_digits = 3});
    CHECK(!refused.succeeded() && refused.required_bytes() == 24 && small[0] == '\0');
}

i64 parsed_nanoseconds(const TextView text) {
    const stxlib::text::Parsed<WallTime> parsed = stxlib::parse_iso8601(text);
    CHECK(parsed.succeeded());
    if (!parsed.succeeded()) return std::numeric_limits<i64>::min();
    const i64 result = (*parsed.value()).unix_nanoseconds();
    return result;
}

bool refused(const TextView text, const stxlib::text::ParseFailure failure) {
    const stxlib::text::Parsed<WallTime> parsed = stxlib::parse_iso8601(text);
    const bool result = !parsed.succeeded() && *parsed.failure() == failure;
    return result;
}

void iso8601_parsing() {
    const i64 base = parsed_nanoseconds("2026-10-31T21:30:05Z");
    CHECK(base == 1793482205LL * 1000000000LL);
    CHECK(parsed_nanoseconds("2026-10-31t21:30:05z") == base);
    CHECK(parsed_nanoseconds("2026-10-31 23:30:05+02:00") == base);
    CHECK(parsed_nanoseconds("2026-10-31T16:00:05-05:30") == base);
    CHECK(parsed_nanoseconds("2026-10-31T21:30:05.5Z") == base + 500000000);
    CHECK(parsed_nanoseconds("2026-10-31T21:30:05.123456789Z") == base + 123456789);
    CHECK(parsed_nanoseconds("1969-12-31T23:59:59.5Z") == -500000000);

    using stxlib::text::ParseFailure;
    CHECK(refused("", ParseFailure::empty));
    CHECK(refused("2026-10-31T21:30:05", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30", ParseFailure::invalid));
    CHECK(refused("2026-13-01T00:00:00Z", ParseFailure::invalid));
    CHECK(refused("2026-02-30T00:00:00Z", ParseFailure::invalid));
    CHECK(refused("2026-10-31T24:00:00Z", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05.1234567890Z", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05.Z", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05+2:00", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05+02:0", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05+02", ParseFailure::invalid));
    CHECK(refused("2026-10-31X21:30:05Z", ParseFailure::invalid));
    CHECK(refused("2026-10-31T21:30:05Z extra", ParseFailure::trailing_text));
    CHECK(refused("2263-01-01T00:00:00Z", ParseFailure::out_of_range));

    // Formatting then parsing gives the instant back (sampled).
    u64 state = 7;
    bool round_trips = true;
    for (int sample = 0; sample < 10000; ++sample) {
        state = next_state(state);
        const i64 nanoseconds = static_cast<i64>(state >> 2) - (1LL << 61);
        const std::string text = iso(stxlib::civil_from_wall(at(nanoseconds)), 0, {.fraction_digits = 9});
        const stxlib::OperationResult<TextView> view = stxlib::validate_utf8(text.data(), text.size());
        const stxlib::text::Parsed<WallTime> parsed = stxlib::parse_iso8601(*view.value());
        round_trips = round_trips && parsed.succeeded() && (*parsed.value()).unix_nanoseconds() == nanoseconds;
    }
    CHECK(round_trips);
}

// A zone's local time written as ISO 8601 names the same instant.
void zone_text() {
    const stxlib::OperationResult<stxlib::Zone> kolkata = stxlib::Zone::named("Asia/Kolkata");
    CHECK(kolkata.succeeded());
    if (!kolkata.succeeded()) return;
    const WallTime winter = at(parsed_nanoseconds("2026-01-15T12:00:00Z"));
    const stxlib::LocalTime local = stxlib::local_time_in(*kolkata.value(), winter);
    const std::string text = iso(local.civil, local.offset_seconds, {});
    CHECK(text == "2026-01-15T17:30:00+05:30");
    const stxlib::OperationResult<TextView> view = stxlib::validate_utf8(text.data(), text.size());
    CHECK(parsed_nanoseconds(*view.value()) == winter.unix_nanoseconds());
}

} // namespace

int main() {
    against_standard_days();
    calendar_rules();
    instants();
    iso8601_text();
    iso8601_parsing();
    zone_text();
    return stxlib_tests::finish("stxlib calendar");
}
