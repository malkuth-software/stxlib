// The host's zone name on Emscripten (hosted/local_time_backend.hpp; owner
// decision 2026-11-15): the browser's or Node's own, from
// Intl.DateTimeFormat().resolvedOptions().timeZone (ECMA-402's
// resolvedOptions; the IANA name, unconfirmed at a dated revision of the
// standard). A runtime without Intl, or one that names no zone, gives "UTC",
// the POSIX module's default when nothing names one
// (posix/local_time_posix.cpp). Emscripten's file system has no
// /etc/localtime to read. Core then looks the name up in STXlib's compiled
// zones (D12 a), as on every family.
//
// Early exit: the EM_JS body catches every JavaScript exception and returns
// 0; it writes only into `text`'s own buffer, through stringToUTF8 with the
// buffer's capacity (Emscripten's JavaScript string helper, which stops at
// the capacity with a NUL; its source was not read at the pin), and `text`
// is set only on success.

#include "hosted/local_time_backend.hpp"

#include "core/host_failure.hpp"

#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>

EM_JS_DEPS(stxlib_web_local_time, "$stringToUTF8,$lengthBytesUTF8");

// clang-format off
EM_JS(int, stxlib_web_zone_name, (double buffer, int capacity), {
    try {
        if (typeof Intl === "undefined" || !Intl.DateTimeFormat) return 0;
        var name = Intl.DateTimeFormat().resolvedOptions().timeZone;
        if (typeof name !== "string" || name.length === 0) return 0;
        var needed = lengthBytesUTF8(name);
        if (needed + 1 > capacity) return -1;
        stringToUTF8(name, buffer, capacity);
        return needed;
    } catch (error) {
        return 0;
    }
});
// clang-format on

namespace stxlib::local_time_backend {

Status system_zone_text(ZoneText& text) noexcept {
    ZoneText found{};
    const double buffer = static_cast<double>(reinterpret_cast<uptr>(found.bytes));
    const int written = stxlib_web_zone_name(buffer, static_cast<int>(zone_text_capacity));
    if (written < 0) {
        return host_failure::failed(ErrorKind::limit_exceeded, "stxlib.local_time.host_zone", HostCode::too_long);
    }
    if (written == 0) {
        text.bytes[0] = 'U';
        text.bytes[1] = 'T';
        text.bytes[2] = 'C';
        text.count = 3;
        return Status::success();
    }
    const usize count = static_cast<usize>(written);
    for (usize index = 0; index < count; ++index) text.bytes[index] = found.bytes[index];
    text.count = count;
    return Status::success();
}

} // namespace stxlib::local_time_backend
