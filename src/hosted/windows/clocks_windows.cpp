// The interval and elapsed clocks on Windows (include/stxlib/clocks.hpp,
// owner decision D12 = a): QueryUnbiasedInterruptTimePrecise, the interrupt
// time without the time spent in sleep or hibernation, and
// QueryInterruptTimePrecise, the interrupt time with it. Both count 100 ns
// units and are documented for Windows 10 and later (realtimeapiset.h, as
// published; not re-read in this batch). Neither can fail.

#include "stxlib/clocks.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <realtimeapiset.h>

namespace stxlib {

namespace {

constexpr i64 nanoseconds_per_unit = 100;

i64 nanoseconds_of(const ULONGLONG units) noexcept {
    // Interrupt time counts from boot: below 2^63 / 100 units (29,000
    // years), so the scaled value fits; it saturates past that regardless.
    const i64 result = detail::saturating_scale(static_cast<i64>(units), nanoseconds_per_unit);
    return result;
}

} // namespace

IntervalTime IntervalTime::now() noexcept {
    ULONGLONG units = 0;
    QueryUnbiasedInterruptTimePrecise(&units);
    const IntervalTime result = IntervalTime::from_nanoseconds(nanoseconds_of(units));
    return result;
}

ElapsedTime ElapsedTime::now() noexcept {
    ULONGLONG units = 0;
    QueryInterruptTimePrecise(&units);
    const ElapsedTime result = ElapsedTime::from_nanoseconds(nanoseconds_of(units));
    return result;
}

} // namespace stxlib
