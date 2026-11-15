// The interval and elapsed clocks on Emscripten (include/stxlib/clocks.hpp,
// owner decision D12 = a; the Emscripten family, owner decision
// 2026-11-15). Both read emscripten_get_now (system/include/emscripten/
// emscripten.h:94 at emscripten 6.0.13), milliseconds as a double: with
// threads, performance.timeOrigin plus performance.now()
// (src/lib/libcore.js:1490-1494), else performance.now() (:1517); the
// runtime states it monotonic (nowIsMonotonic, :1538-1546). Whether it
// counts time the device is suspended is not stated, so elapsed_clock is
// false (core/capabilities.cpp) and ElapsedTime reads the same value, as
// on FreeBSD (clocks_posix.cpp).
//
// The reading is converted to nanoseconds and clamped to [0, i64_max]: a
// double of milliseconds since the time origin is far inside that range
// (2^63 ns is about 292 years), and the clamp keeps a reading outside it
// (a NaN included) from being an out-of-range conversion.
//
// Early exit: emscripten_get_now is an import implemented in JavaScript
// that reads performance (libcore.js:1490-1517), which does not throw; no
// STXlib state is involved.

#include "stxlib/clocks.hpp"

#include <emscripten/emscripten.h>

namespace stxlib {

namespace {

i64 read_nanoseconds() noexcept {
    const double milliseconds = emscripten_get_now();
    const double nanoseconds = milliseconds * 1000000.0;
    // 2^63 as a double; every double below it converts to an i64.
    constexpr double limit = 9223372036854775808.0;
    if (!(nanoseconds >= 0.0)) return 0;
    if (nanoseconds >= limit) return i64_max;
    const i64 result = static_cast<i64>(nanoseconds);
    return result;
}

} // namespace

IntervalTime IntervalTime::now() noexcept {
    const IntervalTime result = IntervalTime::from_nanoseconds(read_nanoseconds());
    return result;
}

ElapsedTime ElapsedTime::now() noexcept {
    const ElapsedTime result = ElapsedTime::from_nanoseconds(read_nanoseconds());
    return result;
}

} // namespace stxlib
