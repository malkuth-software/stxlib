// The wall clock (include/stxlib/clocks.hpp) over the C++ runtime's
// std::chrono::system_clock, ported from stx_auxlib (src/hosted/clocks.cpp
// at b6e330a). The interval and elapsed clocks are the families' own
// (posix/clocks_posix.cpp, windows/clocks_windows.cpp; owner decision
// D12 = a). Since C++20, system_clock's epoch is
// the Unix epoch on every host, so converting its duration to nanoseconds
// is all WallTime needs.

#include "stxlib/clocks.hpp"

#include <chrono>

namespace stxlib {

WallTime WallTime::now() noexcept {
    const std::chrono::system_clock::time_point host = std::chrono::system_clock::now();
    const std::chrono::nanoseconds since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(host.time_since_epoch());
    const WallTime result = WallTime::from_unix_nanoseconds(static_cast<i64>(since_epoch.count()));
    return result;
}

} // namespace stxlib
