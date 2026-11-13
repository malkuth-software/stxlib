// The local time zone (include/stxlib/local_time.hpp) over the family's
// module (hosted/local_time_backend.hpp). Ported from stx_auxlib's
// src/hosted/calendar_<host>.cpp at b6e330a: the offset is the local reading
// minus the UTC one, so no non-standard field (tm_gmtoff) is needed.

#include "stxlib/local_time.hpp"

#include "core/civil.hpp"
#include "hosted/local_time_backend.hpp"

namespace stxlib {

Result<LocalTime> local_time(const WallTime instant) noexcept {
    const civil::WallSplit split = civil::split_wall(instant);
    LocalTime result{};
    const Status read = local_time_backend::local_reading(split.seconds, result.civil);
    if (read.failed()) return Result<LocalTime>::failure(*read.error());
    result.civil.nanosecond = split.nanosecond;
    // Host zones offset by less than a day (the largest in use are +14:00
    // and -12:00), so the difference fits i32.
    const i64 local_seconds = civil::seconds_from_civil(result.civil);
    result.offset_seconds = static_cast<i32>(local_seconds - split.seconds);
    return Result<LocalTime>::success(result);
}

} // namespace stxlib
