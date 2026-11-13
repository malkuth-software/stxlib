// The local time zone on Windows (hosted/local_time_backend.hpp):
// FileTimeToSystemTime for UTC, then SystemTimeToTzSpecificLocalTimeEx with
// the current dynamic time zone (a null zone argument). Ported from
// stx_auxlib's src/hosted/calendar_windows.cpp at b6e330a. FILETIME counts
// 100 ns ticks from 1601-01-01, so earlier instants are refused.

#include "hosted/local_time_backend.hpp"

#include "core/host_failure.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib::local_time_backend {

namespace {

// 100 ns ticks from 1601-01-01 to 1970-01-01.
constexpr i64 unix_epoch_ticks = 116444736000000000LL;
constexpr i64 ticks_per_second = 10000000;

} // namespace

Status local_reading(const i64 seconds, CivilTime& local) noexcept {
    const Operation operation = "stxlib.local_time";
    // Whole seconds only: core carries the nanoseconds over unchanged. The
    // largest WallTime second (about 9.2e9) times 10^7 plus the epoch fits i64.
    if (seconds < -unix_epoch_ticks / ticks_per_second) {
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    }
    const i64 ticks = seconds * ticks_per_second + unix_epoch_ticks;
    FILETIME file_time{};
    file_time.dwLowDateTime = static_cast<DWORD>(static_cast<u64>(ticks) & 0xffffffffULL);
    file_time.dwHighDateTime = static_cast<DWORD>(static_cast<u64>(ticks) >> 32);
    SYSTEMTIME utc{};
    SYSTEMTIME reading{};
    if (!FileTimeToSystemTime(&file_time, &utc) || !SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &reading)) {
        return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none,
                                    ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError())));
    }
    local.year = static_cast<i32>(reading.wYear);
    local.month = static_cast<u8>(reading.wMonth);
    local.day = static_cast<u8>(reading.wDay);
    local.hour = static_cast<u8>(reading.wHour);
    local.minute = static_cast<u8>(reading.wMinute);
    local.second = static_cast<u8>(reading.wSecond);
    return Status::success();
}

} // namespace stxlib::local_time_backend
