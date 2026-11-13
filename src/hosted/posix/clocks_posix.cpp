// The interval and elapsed clocks on POSIX hosts (include/stxlib/clocks.hpp,
// owner decision D12 = a), through clock_gettime:
//   - Linux: CLOCK_MONOTONIC, which does not count time suspended, and
//     CLOCK_BOOTTIME (Linux 2.6.39), which is CLOCK_MONOTONIC plus it
//     (clock_gettime(2));
//   - macOS: CLOCK_UPTIME_RAW, which does not advance while the system is
//     asleep, and CLOCK_MONOTONIC, which does (clock_gettime(3)).
// The host semantics are stated from those manual pages as published, not
// re-read in this batch. The clocks named are always present on the hosts
// STXlib supports, so a failure of clock_gettime is a defect.

#include "stxlib/clocks.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>

#include <time.h>

namespace stxlib {

namespace {

#if defined(__linux__)
constexpr clockid_t interval_clock_id = CLOCK_MONOTONIC;
constexpr clockid_t elapsed_clock_id = CLOCK_BOOTTIME;
#elif defined(__APPLE__)
constexpr clockid_t interval_clock_id = CLOCK_UPTIME_RAW;
constexpr clockid_t elapsed_clock_id = CLOCK_MONOTONIC;
#else
#error "the interval and elapsed clocks are defined for Linux and macOS only"
#endif

[[noreturn]] void clock_defect(const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.clocks",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = "clock_gettime failed"});
    fail_fast(record);
}

i64 read_nanoseconds(const clockid_t clock) noexcept {
    timespec now{};
    if (::clock_gettime(clock, &now) != 0) clock_defect(errno);
    const i64 seconds = detail::saturating_scale(static_cast<i64>(now.tv_sec), 1000000000);
    const i64 result = detail::saturating_add(seconds, static_cast<i64>(now.tv_nsec));
    return result;
}

} // namespace

IntervalTime IntervalTime::now() noexcept {
    const IntervalTime result = IntervalTime::from_nanoseconds(read_nanoseconds(interval_clock_id));
    return result;
}

ElapsedTime ElapsedTime::now() noexcept {
    const ElapsedTime result = ElapsedTime::from_nanoseconds(read_nanoseconds(elapsed_clock_id));
    return result;
}

} // namespace stxlib
