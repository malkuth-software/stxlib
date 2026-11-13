#pragma once

// The primitive each family's local time module supplies
// (posix/local_time_posix.cpp, windows/local_time_windows.cpp): the name the
// host gives its zone when TZ is not set. core/local_time.cpp reads TZ,
// reduces the text to a database name and looks it up (local_time.hpp,
// "The host's zone").

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib::local_time_backend {

// Room for a link target or a zone file's first line; database names are
// at most 32 bytes (zone_data.inc's longest_zone_name).
inline constexpr usize zone_text_capacity = 1024;

struct ZoneText final {
    char bytes[zone_text_capacity]{};
    usize count{0};
};

// The host's text for its zone, as found: on POSIX hosts the target of the
// /etc/localtime link or the first line of /etc/timezone (without its line
// ending), or "UTC" when neither exists; on Windows the IANA name ICU maps
// the current dynamic time zone to. `text` is written only on success.
// Failures as host_zone's (local_time.hpp).
[[nodiscard]] Status system_zone_text(ZoneText& text) noexcept;

} // namespace stxlib::local_time_backend
