// The host's zone (include/stxlib/local_time.hpp: host_zone, local_time):
// TZ when set, else the family's module (hosted/local_time_backend.hpp),
// reduced to a database name and looked up in the compiled zones
// (core/zones.cpp). Replaces batch 2c's port of stx_auxlib's
// calendar_<host>.cpp, which used the host's zone rules (owner decision
// D12 = a).

#include "stxlib/local_time.hpp"

#include "hosted/local_time_backend.hpp"
#include "stxlib/environment.hpp"
#include "stxlib/text.hpp"

namespace stxlib {

namespace {

constexpr Operation host_zone_operation = "stxlib.local_time.host_zone";

// TZ values longer than this are no zone name or path of ours.
constexpr usize tz_limit = local_time_backend::zone_text_capacity;

bool starts_with(const char* const bytes, const usize count, const char* const prefix,
                 const usize prefix_count) noexcept {
    if (count < prefix_count) return false;
    for (usize index = 0; index < prefix_count; ++index) {
        if (bytes[index] != prefix[index]) return false;
    }
    return true;
}

// The database name in the host's text (local_time.hpp, "The host's
// zone"): one leading ':' dropped; then, when the text holds "zoneinfo/",
// what follows its last occurrence, without a leading "posix/" or "right/".
// The result borrows from `bytes`.
struct NamePart final {
    usize start{0};
    usize count{0};
};

NamePart database_name_part(const char* const bytes, const usize count) noexcept {
    usize start = 0;
    if (count > 0 && bytes[0] == ':') start = 1;
    constexpr char marker[] = "zoneinfo/";
    constexpr usize marker_count = sizeof(marker) - 1;
    for (usize position = start; position + marker_count <= count; ++position) {
        if (starts_with(bytes + position, count - position, marker, marker_count)) {
            start = position + marker_count;
        }
    }
    constexpr char posix_directory[] = "posix/";
    constexpr char right_directory[] = "right/";
    if (starts_with(bytes + start, count - start, posix_directory, sizeof(posix_directory) - 1)) {
        start = start + sizeof(posix_directory) - 1;
    } else if (starts_with(bytes + start, count - start, right_directory, sizeof(right_directory) - 1)) {
        start = start + sizeof(right_directory) - 1;
    }
    const NamePart result{start, count - start};
    return result;
}

// The zone the host's text names. The text is the host's or the caller's
// TZ, already checked as UTF-8 (read_variable), or the module's ASCII.
Result<Zone> zone_of_text(const char* const bytes, const usize count) noexcept {
    const NamePart part = database_name_part(bytes, count);
    if (part.count == 0) return Result<Zone>::success(Zone::utc());
    const TextView name = detail::TextViewAccess::adopt_validated(bytes + part.start, part.count);
    Result<Zone> found = Zone::named(name);
    if (found.succeeded()) return found;
    const ErrorRecord record = ErrorRecord::make(ErrorKind::not_found, host_zone_operation);
    return Result<Zone>::failure(record);
}

} // namespace

Result<Zone> host_zone() noexcept {
    const Result<Utf8String> tz = read_variable("TZ", tz_limit);
    if (tz.succeeded()) {
        const TextView value = (*tz.value()).view();
        Result<Zone> result = zone_of_text(value.data(), value.count());
        return result;
    }
    if ((*tz.error()).kind() != ErrorKind::not_found) return Result<Zone>::failure(*tz.error());
    local_time_backend::ZoneText text{};
    const Status read = local_time_backend::system_zone_text(text);
    if (read.failed()) return Result<Zone>::failure(*read.error());
    // The module's text may be any bytes the host stored: names in the
    // database are ASCII, so anything else is no name of ours.
    for (usize index = 0; index < text.count; ++index) {
        const u8 byte = static_cast<u8>(text.bytes[index]);
        if (byte < 0x21 || byte > 0x7e) {
            const ErrorRecord record = ErrorRecord::make(ErrorKind::not_found, host_zone_operation);
            return Result<Zone>::failure(record);
        }
    }
    Result<Zone> result = zone_of_text(text.bytes, text.count);
    return result;
}

Result<LocalTime> local_time(const WallTime instant) noexcept {
    const Result<Zone> zone = host_zone();
    if (zone.failed()) return Result<LocalTime>::failure(*zone.error());
    const LocalTime result = local_time_in(*zone.value(), instant);
    return Result<LocalTime>::success(result);
}

} // namespace stxlib
