// The compiled time zone database (include/stxlib/local_time.hpp: Zone,
// ZoneReading, tzdata_version, local_time_in).
//
// The tables (src/zones/zone_data.inc) are written by tools/tzcompile.py
// from IANA tzdata (third_party/tzdata; PROVENANCE.md; the update policy in
// tools/TZDATA_POLICY.md). To regenerate after taking a release:
//     python3 tools/tzcompile.py third_party/tzdata src/zones/zone_data.inc
// and check it against a zic-built reference with tools/tzcheck.py.
//
// The tables, per zone: the type in effect before its first transition,
// its transitions (UT seconds, ascending, each with the type it starts) up
// to a cutoff, and an optional footer: the two rules of the zone's last
// rule set that run to "max", which alternate every year from
// footer_start (1 January of the cutoff year plus two, UT) on. The reader
// finds the last transition at or before the instant by an upper-bound
// search; at or after footer_start it takes instead the latest footer
// event at or before the instant and not before footer_start, from the
// instant's civil year and the years either side (a rule's local date can
// fall up to a day into the next or previous UT year), or the searched
// type when there is none. tzcompile.py checks, for every zone, that this
// rule reproduces the zone's full expansion through 2263 at every instant
// where either changes, and refuses to write tables otherwise; the reader
// below is the same rule (footer_type_at there).

#include "stxlib/local_time.hpp"

#include "core/civil.hpp"

namespace stxlib {

namespace {

// A local time type: the offset, the daylight flag and the abbreviation's
// position in abbreviation_pool (NUL-terminated there).
struct ZoneType final {
    i32 offset;
    u8 is_dst;
    u16 abbreviation;
};

// The day a footer rule names in its month (tzdata's ON field).
enum class OnKind : u8 {
    day_of_month = 0, // "5"
    weekday_on_or_after = 1, // "Sun>=8"
    weekday_on_or_before = 2, // "Sun<=25"
    last_weekday = 3, // "lastSun"
};

struct FooterRule final {
    u8 month;   // 1 to 12
    u8 on_kind; // OnKind
    u8 day;     // the day of month, for all but last_weekday
    u8 weekday; // 0 = Sunday, for all but day_of_month
    i32 at;     // seconds after the day's midnight; may pass 24:00
    u8 at_standard; // AT is standard time ('s')
    u8 at_ut;       // AT is UT ('u', 'g', 'z')
    i32 save;       // seconds
    u16 type;       // the type this rule starts
};

struct ZoneFooter final {
    i32 standard_offset{0};
    i64 footer_start{0};
    FooterRule rules[2]{};
};

struct ZoneRecord final {
    u32 first_transition;
    u32 transition_count;
    u16 initial_type;
    u16 footer; // index into zone_footers; 0 is none
};

struct ZoneName final {
    const char* name; // ASCII, NUL-terminated
    u32 length;
    u16 zone;
};

#include "zones/zone_data.inc"

constexpr usize zone_count = sizeof(zone_records) / sizeof(zone_records[0]);
constexpr usize name_count = sizeof(zone_names) / sizeof(zone_names[0]);
constexpr usize type_count = sizeof(zone_types) / sizeof(zone_types[0]);
constexpr usize transition_count = sizeof(transition_instants) / sizeof(transition_instants[0]);
static_assert(transition_count == sizeof(transition_types) / sizeof(transition_types[0]));
static_assert(name_count <= 65535 && zone_count <= 65535 && type_count <= 65535);
static_assert(utc_name_index < name_count);

// The tables' internal agreement, checked once at compile time: every index
// is in range and every zone's transitions ascend. Generated data is input
// (house style section 2), so it is validated before the reader trusts it.
consteval bool tables_agree() {
    for (usize zone = 0; zone < zone_count; ++zone) {
        const ZoneRecord& record = zone_records[zone];
        if (record.first_transition > transition_count) return false;
        if (record.transition_count > transition_count - record.first_transition) return false;
        if (record.initial_type >= type_count) return false;
        if (record.footer >= sizeof(zone_footers) / sizeof(zone_footers[0])) return false;
        for (u32 index = 0; index < record.transition_count; ++index) {
            const usize position = record.first_transition + index;
            if (transition_types[position] >= type_count) return false;
            if (index > 0 && transition_instants[position - 1] >= transition_instants[position]) return false;
        }
    }
    for (usize footer = 1; footer < sizeof(zone_footers) / sizeof(zone_footers[0]); ++footer) {
        for (const FooterRule& rule : zone_footers[footer].rules) {
            if (rule.month < 1 || rule.month > 12 || rule.on_kind > 3 || rule.weekday > 6) return false;
            if (rule.type >= type_count) return false;
        }
    }
    for (usize type = 0; type < type_count; ++type) {
        if (zone_types[type].abbreviation >= sizeof(abbreviation_pool) - 1) return false;
    }
    for (usize name = 0; name < name_count; ++name) {
        if (zone_names[name].zone >= zone_count) return false;
    }
    return true;
}
static_assert(tables_agree(), "src/zones/zone_data.inc is inconsistent; regenerate it");

// Byte order of two names, as tzcompile.py sorted them: -1, 0 or 1.
int compare_names(const char* const left, const usize left_count, const char* const right,
                  const usize right_count) noexcept {
    const usize shorter = left_count < right_count ? left_count : right_count;
    for (usize index = 0; index < shorter; ++index) {
        const u8 left_byte = static_cast<u8>(left[index]);
        const u8 right_byte = static_cast<u8>(right[index]);
        if (left_byte != right_byte) return left_byte < right_byte ? -1 : 1;
    }
    if (left_count == right_count) return 0;
    return left_count < right_count ? -1 : 1;
}

// The first position in [first, first + count) whose instant is after
// `seconds` (an upper bound); first + count when there is none.
usize upper_bound_instant(const usize first, const usize count, const i64 seconds) noexcept {
    usize low = first;
    usize high = first + count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (transition_instants[middle] <= seconds) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return low;
}

// tzcompile.py's day_of: the day (from 1970-01-01) the rule names in its
// month of `year`.
i64 rule_day(const FooterRule& rule, const i64 year) noexcept {
    const OnKind kind = static_cast<OnKind>(rule.on_kind);
    if (kind == OnKind::last_weekday) {
        const i64 next_month_first = rule.month == 12 ? civil::days_from_civil(year + 1, 1, 1)
                                                      : civil::days_from_civil(year, rule.month + 1u, 1);
        const i64 last = next_month_first - 1;
        const i64 back = civil::floor_remainder(static_cast<i64>(civil::weekday_of(last)) - rule.weekday, 7);
        return last - back;
    }
    const i64 base = civil::days_from_civil(year, rule.month, rule.day);
    if (kind == OnKind::weekday_on_or_after) {
        const i64 forward = civil::floor_remainder(static_cast<i64>(rule.weekday) - civil::weekday_of(base), 7);
        return base + forward;
    }
    if (kind == OnKind::weekday_on_or_before) {
        const i64 back = civil::floor_remainder(static_cast<i64>(civil::weekday_of(base)) - rule.weekday, 7);
        return base - back;
    }
    return base; // day_of_month (tables_agree admits no other kind)
}

// tzcompile.py's footer_events: the UT instant of rule `which` in `year`,
// the save before it being the other rule's (the two alternate).
i64 rule_instant(const ZoneFooter& footer, const usize which, const i64 year) noexcept {
    const FooterRule& rule = footer.rules[which];
    const FooterRule& other = footer.rules[1 - which];
    i64 offset = rule.at_ut != 0 ? 0 : footer.standard_offset;
    if (rule.at_standard == 0) offset = offset + other.save;
    const i64 local = rule_day(rule, year) * civil::seconds_per_day + rule.at;
    const i64 result = local - offset;
    return result;
}

// tzcompile.py's footer_type_at. Requirement: seconds >= footer_start.
u16 footer_type_at(const ZoneFooter& footer, const u16 before_type, const i64 seconds) noexcept {
    const i64 year = civil::civil_from_days(civil::floor_divide(seconds, civil::seconds_per_day)).year;
    i64 best_moment = footer.footer_start - 1;
    u16 best_type = before_type;
    for (i64 candidate_year = year - 1; candidate_year <= year + 1; ++candidate_year) {
        for (usize which = 0; which < 2; ++which) {
            const i64 moment = rule_instant(footer, which, candidate_year);
            if (moment <= seconds && moment > best_moment) {
                best_moment = moment;
                best_type = footer.rules[which].type;
            }
        }
    }
    return best_type;
}

u16 type_at(const usize zone, const i64 seconds) noexcept {
    const ZoneRecord& record = zone_records[zone];
    const usize after = upper_bound_instant(record.first_transition, record.transition_count, seconds);
    u16 type = record.initial_type;
    if (after > record.first_transition) type = transition_types[after - 1];
    if (record.footer != 0) {
        const ZoneFooter& footer = zone_footers[record.footer];
        if (seconds >= footer.footer_start) type = footer_type_at(footer, type, seconds);
    }
    return type;
}

TextView pool_text(const u16 position) noexcept {
    const char* const start = abbreviation_pool + position;
    usize count = 0;
    while (start[count] != '\0') ++count;
    // Abbreviations are ASCII (tzcompile.py writes the data's FORMAT text,
    // which zic limits to ASCII letters, digits, '+' and '-').
    const TextView result = detail::TextViewAccess::adopt_validated(start, count);
    return result;
}

} // namespace

Zone::Zone() noexcept : name_index_(utc_name_index) {}

Zone Zone::utc() noexcept {
    const Zone result(utc_name_index);
    return result;
}

OperationResult<Zone> Zone::named(const TextView name) noexcept {
    usize low = 0;
    usize high = name_count;
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const ZoneName& entry = zone_names[middle];
        const int order = compare_names(entry.name, entry.length, name.data(), name.count());
        if (order == 0) {
            const Zone result(static_cast<u16>(middle));
            return OperationResult<Zone>::success(result);
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const ErrorRecord record = ErrorRecord::make(ErrorKind::not_found, "stxlib.zone.named");
    return OperationResult<Zone>::failure(record);
}

TextView Zone::name() const noexcept {
    const ZoneName& entry = zone_names[name_index_];
    const TextView result = detail::TextViewAccess::adopt_validated(entry.name, entry.length);
    return result;
}

ZoneReading Zone::at(const WallTime instant) const noexcept {
    const civil::WallSplit split = civil::split_wall(instant);
    const ZoneType& type = zone_types[type_at(zone_names[name_index_].zone, split.seconds)];
    ZoneReading result{};
    result.offset_seconds = type.offset;
    result.daylight = type.is_dst != 0;
    result.abbreviation = pool_text(type.abbreviation);
    return result;
}

TextView tzdata_version() noexcept {
    const TextView result = detail::TextViewAccess::adopt_validated(tzdata_version_text,
                                                                    sizeof(tzdata_version_text) - 1);
    return result;
}

LocalTime local_time_in(const Zone& zone, const WallTime instant) noexcept {
    const civil::WallSplit split = civil::split_wall(instant);
    const ZoneReading reading = zone.at(instant);
    // Offsets in the database lie within a day of UTC (tzdata's largest is
    // under 16 hours), and WallTime's seconds within ±9.3e9, so the sum
    // fits i64 and its year fits i32.
    const i64 local_seconds = split.seconds + reading.offset_seconds;
    LocalTime result{};
    result.civil = civil::civil_from_seconds(local_seconds);
    result.civil.nanosecond = split.nanosecond;
    result.offset_seconds = reading.offset_seconds;
    result.daylight = reading.daylight;
    result.abbreviation = reading.abbreviation;
    return result;
}

} // namespace stxlib
