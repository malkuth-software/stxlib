// The local time zone on POSIX hosts (hosted/local_time_backend.hpp):
// tzset and localtime_r, under the environment lock because both read TZ.
// Ported from stx_auxlib's src/hosted/calendar_posix.cpp at b6e330a.
// localtime_r need not reread TZ (POSIX: localtime_r need not behave as
// though tzset were called; from the published text, not re-read in this
// batch), so tzset runs on the first call and after every change made
// through set_variable or remove_variable, which count in
// environment_generation (environment.cpp). The load state lives in
// stxlib_state, so every module shares it.

#include "hosted/local_time_backend.hpp"

#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "state/state.hpp"

#include <cerrno>
#include <ctime>

#include <time.h>

namespace stxlib::local_time_backend {

static_assert(sizeof(time_t) >= 8, "time_t must hold 64-bit seconds");

Status local_reading(const i64 seconds, CivilTime& local) noexcept {
    const time_t host_seconds = static_cast<time_t>(seconds);
    tm reading{};
    {
        state::ProcessState& process = state::current_process();
        const spin::Guard lock(process.environment_lock);
        if (!process.zone_loaded || process.zone_generation != process.environment_generation) {
            ::tzset();
            process.zone_loaded = true;
            process.zone_generation = process.environment_generation;
        }
        errno = 0;
        if (::localtime_r(&host_seconds, &reading) == nullptr) {
            const int error = errno;
            if (error == 0) return host_failure::failed(ErrorKind::io_failure, "stxlib.local_time", HostCode::none);
            return host_failure::failed(ErrorKind::io_failure, "stxlib.local_time", HostCode::none,
                                        ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
        }
    }
    local.year = static_cast<i32>(reading.tm_year + 1900);
    local.month = static_cast<u8>(reading.tm_mon + 1);
    local.day = static_cast<u8>(reading.tm_mday);
    local.hour = static_cast<u8>(reading.tm_hour);
    local.minute = static_cast<u8>(reading.tm_min);
    // A leap-second-aware zone may report second 60; Unix time has none.
    local.second = static_cast<u8>(reading.tm_sec > 59 ? 59 : reading.tm_sec);
    return Status::success();
}

} // namespace stxlib::local_time_backend
