// The clocks (include/stxlib/clocks.hpp) over the C++ runtime's
// std::chrono::steady_clock and system_clock, ported from stx_auxlib
// (src/hosted/clocks.cpp at b6e330a). Since C++20, system_clock's epoch is
// the Unix epoch on every host, so converting its duration to nanoseconds
// is all WallTime needs.

#include "stxlib/clocks.hpp"

#include <chrono>

namespace stxlib {

MonotonicTime MonotonicTime::now() noexcept {
    const std::chrono::steady_clock::time_point host = std::chrono::steady_clock::now();
    const std::chrono::nanoseconds since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(host.time_since_epoch());
    const MonotonicTime result = MonotonicTime::from_nanoseconds(static_cast<i64>(since_epoch.count()));
    return result;
}

WallTime WallTime::now() noexcept {
    const std::chrono::system_clock::time_point host = std::chrono::system_clock::now();
    const std::chrono::nanoseconds since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(host.time_since_epoch());
    const WallTime result = WallTime::from_unix_nanoseconds(static_cast<i64>(since_epoch.count()));
    return result;
}

} // namespace stxlib
