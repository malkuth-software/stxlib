// The host-free calendar (include/stxlib/calendar.hpp). Ported from
// stx_auxlib's src/calendar.cpp at b6e330a: the same arithmetic, validity
// rules, text forms and parse rules. Changes: the civil-day conversions are
// the boundary's src/core/civil.hpp (the same Hinnant algorithms, already
// ported in batch 2c and 3b); WallTime in place of WallClock::time_point;
// Result in place of std::optional; the text goes into caller storage.

#include "stxlib/calendar.hpp"

#include "core/civil.hpp"

namespace stxlib {

namespace {

bool is_digit(const char character) noexcept {
    const bool result = character >= '0' && character <= '9';
    return result;
}

// The number in `text` at [first, first + count), all digits; -1 otherwise.
i64 digits_at(const TextView text, const usize first, const usize count) noexcept {
    if (first > text.count() || count > text.count() - first) return -1;
    const char* const bytes = text.data();
    i64 value = 0;
    for (usize index = first; index < first + count; ++index) {
        if (!is_digit(bytes[index])) return -1;
        value = value * 10 + (bytes[index] - '0');
    }
    return value;
}

usize decimal_length(u64 value) noexcept {
    usize length = 1;
    while (value >= 10) {
        value /= 10;
        ++length;
    }
    return length;
}

// Writes into one caller buffer whose size the caller has checked
// against the text's length.
struct Cursor final {
    char* next{nullptr};

    void put(const char character) noexcept {
        *next = character;
        ++next;
    }

    // `value` as exactly `digits` decimal digits, zero-padded (its low
    // digits when it has more).
    void digits(u64 value, const usize count) noexcept {
        for (usize index = count; index > 0; --index) {
            next[index - 1] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
        next += count;
    }
};

// The instant of floor seconds and the nanoseconds past them; failure
// out_of_range when it does not fit WallTime. The bounds: i64_max is
// 9223372036 s + 854775807 ns, and i64_min is -9223372037 s + 145224192 ns.
bool wall_from_seconds(const i64 seconds, const u32 nanosecond, WallTime& instant) noexcept {
    constexpr i64 largest_seconds = 9223372036;
    constexpr i64 smallest_seconds = -9223372037;
    if (seconds > largest_seconds || seconds < smallest_seconds) return false;
    if (seconds == largest_seconds && nanosecond > 854775807U) return false;
    if (seconds == smallest_seconds && nanosecond < 145224192U) return false;
    // For negative seconds, (seconds + 1) * 10^9 - (10^9 - nanosecond) keeps
    // every intermediate in range at the lower bound, where seconds * 10^9
    // alone would overflow.
    i64 count = 0;
    if (seconds >= 0) {
        count = seconds * civil::nanoseconds_per_second + static_cast<i64>(nanosecond);
    } else {
        count = (seconds + 1) * civil::nanoseconds_per_second -
                (civil::nanoseconds_per_second - static_cast<i64>(nanosecond));
    }
    instant = WallTime::from_unix_nanoseconds(count);
    return true;
}

} // namespace

bool is_leap_year(const i32 year) noexcept {
    const bool result = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return result;
}

u8 days_in_month(const i32 year, const u8 month) noexcept {
    constexpr u8 lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return lengths[month - 1];
}

bool is_valid_civil_time(const CivilTime& time) noexcept {
    const bool result = time.day >= 1 && time.day <= days_in_month(time.year, time.month) && time.hour < 24 &&
                        time.minute < 60 && time.second < 60 && time.nanosecond < 1000000000U;
    return result;
}

namespace {

// The requirement weekday_of and day_of_year state (calendar.hpp).
void require_valid_date(const CivilTime& time) noexcept {
    STXLIB_REQUIRE(time.day >= 1 && time.day <= days_in_month(time.year, time.month),
                   "calendar: the date does not exist");
}

} // namespace

Weekday weekday_of(const CivilTime& time) noexcept {
    require_valid_date(time);
    // 1970-01-01 was a Thursday (ISO 4).
    const i64 days = civil::days_from_civil(time.year, time.month, time.day);
    const Weekday result = static_cast<Weekday>(civil::floor_remainder(days + 3, 7) + 1);
    return result;
}

u16 day_of_year(const CivilTime& time) noexcept {
    require_valid_date(time);
    const i64 days = civil::days_from_civil(time.year, time.month, time.day);
    const i64 first = civil::days_from_civil(time.year, 1, 1);
    const u16 result = static_cast<u16>(days - first + 1);
    return result;
}

CivilTime civil_from_wall(const WallTime instant) noexcept {
    const civil::WallSplit split = civil::split_wall(instant);
    CivilTime result = civil::civil_from_seconds(split.seconds);
    result.nanosecond = split.nanosecond;
    return result;
}

Result<WallTime> wall_from_civil(const CivilTime& time) noexcept {
    if (!is_valid_civil_time(time)) {
        return Result<WallTime>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.calendar.wall_from_civil"));
    }
    WallTime instant{};
    if (!wall_from_seconds(civil::seconds_from_civil(time), time.nanosecond, instant)) {
        return Result<WallTime>::failure(ErrorRecord::make(ErrorKind::out_of_range, "stxlib.calendar.wall_from_civil"));
    }
    return Result<WallTime>::success(instant);
}

text::Formatted format_iso8601(const MutableView<char> output, const CivilTime& time, const i32 offset_seconds,
                               const Iso8601Format& format) noexcept {
    STXLIB_REQUIRE(static_cast<unsigned char>(format.separator) < 0x80U, "format_iso8601: the separator is not ASCII");
    STXLIB_REQUIRE(is_valid_civil_time(time), "format_iso8601: the civil time is not valid");
    STXLIB_REQUIRE(offset_seconds > -100 * 3600 && offset_seconds < 100 * 3600,
                   "format_iso8601: the offset is 100 hours or more");
    // The year: four digits from 0 to 9999, else a sign and at least four.
    const bool plain_year = time.year >= 0 && time.year <= 9999;
    const i64 year = time.year;
    const u64 year_magnitude = static_cast<u64>(year < 0 ? -year : year);
    const usize year_length = decimal_length(year_magnitude);
    const usize year_digits = plain_year ? 4 : (year_length < 4 ? 4 : year_length);
    const usize sign_bytes = plain_year ? 0 : 1;
    const usize fraction_digits = format.fraction_digits > 9 ? 9 : format.fraction_digits;
    const usize fraction_bytes = fraction_digits > 0 ? 1 + fraction_digits : 0;
    usize zone_bytes = 0;
    if (format.zone != ZoneDesignator::none) zone_bytes = offset_seconds == 0 ? 1 : 6;
    // "-MM-DD", the separator, "HH:MM:SS".
    const usize total = sign_bytes + year_digits + 6 + 1 + 8 + fraction_bytes + zone_bytes;
    if (total > output.count()) return text::Formatted::too_small(total);

    Cursor cursor{output.data()};
    if (!plain_year) cursor.put(time.year < 0 ? '-' : '+');
    cursor.digits(year_magnitude, year_digits);
    cursor.put('-');
    cursor.digits(time.month, 2);
    cursor.put('-');
    cursor.digits(time.day, 2);
    cursor.put(format.separator);
    cursor.digits(time.hour, 2);
    cursor.put(':');
    cursor.digits(time.minute, 2);
    cursor.put(':');
    cursor.digits(time.second, 2);
    if (fraction_digits > 0) {
        u64 scale = 1;
        for (usize index = fraction_digits; index < 9; ++index) scale *= 10;
        cursor.put('.');
        cursor.digits(time.nanosecond / scale, fraction_digits);
    }
    if (zone_bytes == 1) cursor.put('Z');
    if (zone_bytes == 6) {
        const i64 offset = offset_seconds;
        const i64 magnitude = offset < 0 ? -offset : offset;
        cursor.put(offset < 0 ? '-' : '+');
        cursor.digits(static_cast<u64>(magnitude / 3600), 2);
        cursor.put(':');
        cursor.digits(static_cast<u64>(magnitude % 3600 / 60), 2);
    }
    const TextView text = detail::TextViewAccess::adopt_validated(output.data(), total);
    const text::Formatted result = text::Formatted::written(text);
    return result;
}

// Fixed positions: YYYY-MM-DDTHH:MM:SS at 0..18, then an optional fraction,
// then the zone.
text::Parsed<WallTime> parse_iso8601(const TextView value) noexcept {
    using Parsed = text::Parsed<WallTime>;
    if (value.empty()) return Parsed::failure(text::ParseFailure::empty);
    const char* const bytes = value.data();
    const usize length = value.count();
    const i64 year = digits_at(value, 0, 4);
    const i64 month = digits_at(value, 5, 2);
    const i64 day = digits_at(value, 8, 2);
    const i64 hour = digits_at(value, 11, 2);
    const i64 minute = digits_at(value, 14, 2);
    const i64 second = digits_at(value, 17, 2);
    // Each digits_at above has checked its bytes lie inside the text, so a
    // text this far is at least 19 bytes long.
    if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0) {
        return Parsed::failure(text::ParseFailure::invalid);
    }
    const char separator = bytes[10];
    if (bytes[4] != '-' || bytes[7] != '-' || bytes[13] != ':' || bytes[16] != ':' ||
        (separator != 'T' && separator != 't' && separator != ' ')) {
        return Parsed::failure(text::ParseFailure::invalid);
    }
    usize position = 19;
    u32 nanosecond = 0;
    if (position < length && bytes[position] == '.') {
        ++position;
        usize digits = 0;
        while (position < length && is_digit(bytes[position])) {
            if (digits == 9) return Parsed::failure(text::ParseFailure::invalid);
            nanosecond = nanosecond * 10 + static_cast<u32>(bytes[position] - '0');
            ++digits;
            ++position;
        }
        if (digits == 0) return Parsed::failure(text::ParseFailure::invalid);
        for (usize index = digits; index < 9; ++index) nanosecond *= 10;
    }
    if (position >= length) return Parsed::failure(text::ParseFailure::invalid);
    i64 offset = 0;
    const char zone = bytes[position];
    if (zone == 'Z' || zone == 'z') {
        ++position;
    } else if (zone == '+' || zone == '-') {
        const i64 offset_hours = digits_at(value, position + 1, 2);
        const i64 offset_minutes = digits_at(value, position + 4, 2);
        // offset_minutes >= 0 places position + 5 inside the text, so the
        // colon's byte at position + 3 is inside too.
        if (offset_hours < 0 || offset_minutes < 0 || bytes[position + 3] != ':' || offset_hours > 23 ||
            offset_minutes > 59) {
            return Parsed::failure(text::ParseFailure::invalid);
        }
        offset = offset_hours * 3600 + offset_minutes * 60;
        if (zone == '-') offset = -offset;
        position += 6;
    } else {
        return Parsed::failure(text::ParseFailure::invalid);
    }
    if (position != length) return Parsed::failure(text::ParseFailure::trailing_text);

    CivilTime time{};
    time.year = static_cast<i32>(year);
    time.month = static_cast<u8>(month);
    time.day = static_cast<u8>(day);
    time.hour = static_cast<u8>(hour);
    time.minute = static_cast<u8>(minute);
    time.second = static_cast<u8>(second);
    time.nanosecond = nanosecond;
    if (!is_valid_civil_time(time)) return Parsed::failure(text::ParseFailure::invalid);
    // The text's local reading minus its offset is the instant.
    const i64 seconds = civil::seconds_from_civil(time) - offset;
    WallTime instant{};
    if (!wall_from_seconds(seconds, nanosecond, instant)) return Parsed::failure(text::ParseFailure::out_of_range);
    return Parsed::success(instant);
}

} // namespace stxlib
