// Row 14: calendar_detail::wall_from_seconds (B1.md section 3.1).
// P0: no undefined behaviour: the comment's claim (lines 149-151) that no
//     intermediate overflows.
// P2: the result is seconds * 10^9 + nanosecond, computed in 128 bits, when
//     that fits int64, and empty otherwise.
// Bound: all int64 seconds, nanosecond < 10^9; no loop.
#include "verify.h"

#include <chrono>
#include <cstdint>
#include <optional>

#include "stx_auxlib/clocks.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/clocks.hpp sha256=aea15d362d6aee788416da898bfbe3c697a96091ee6365cccfde8507c9de49b6
//@ lift source=stx_auxlib path=src/calendar_detail.hpp lines=39-40 namespace=stx_auxlib::calendar_detail sha256=f4453aa2f7c5287bee04f05e2f5d61050547af469e231ab41b2b5e1c030e911e
//@ lift source=stx_auxlib path=src/calendar.cpp lines=24-25 namespace=stx_auxlib anonymous sha256=fc88790a2a3e0247da732b8bdb8f5db64e4bfa538cdd3feed41b70ca1afc57c4
//@ lift source=stx_auxlib path=src/calendar.cpp lines=142-161 namespace=stx_auxlib sha256=735ff5f570e0c8ae92101eb3ec2523223ad0ccd96224cd7fffffce003831b041
//@ mutant id=m1 source=stx_auxlib path=src/calendar.cpp line=147 from="854775807U" to="854775808U"

int main() {
    const std::int64_t seconds = static_cast<std::int64_t>(nondet_longlong());
    const std::uint32_t nanosecond = nondet_uint();
    STX_ASSUME(nanosecond < 1000000000U);
    const std::optional<stx_auxlib::WallClock::time_point> result =
        stx_auxlib::calendar_detail::wall_from_seconds(seconds, nanosecond);
    const __int128 exact = static_cast<__int128>(seconds) * 1000000000 + static_cast<__int128>(nanosecond);
    const bool fits = exact >= INT64_MIN && exact <= INT64_MAX;
    STX_ASSERT(result.has_value() == fits, "P2: a result exactly when the count fits int64");
    if (fits) {
        STX_ASSERT(static_cast<__int128>((*result).time_since_epoch().count()) == exact, "P2: the count is exact");
    }
    STX_VACUITY_PROBE();
    return 0;
}
