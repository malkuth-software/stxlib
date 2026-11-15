// Row 16: random::detail::distance and advance (B1.md section 3.1).
// P0: no undefined behaviour under the stated preconditions (random.hpp:
//     distance for low <= high; advance for offset <= distance(low, high),
//     that is low + offset <= INT64_MAX in exact arithmetic).
// P2: distance is high - low exactly, advance is low + offset exactly, and
//     advance(low, distance(low, high)) == high.
// Bound: the whole int64 domain; no loop.
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=include/stx_auxlib/random.hpp lines=140-148 namespace=stx_auxlib::random::detail sha256=1c397ea0a4db64c3e4a90300a2b19b157ccb4591081412a19d6e6eefeae94404
//@ lift source=stx_auxlib path=src/random.cpp lines=212-228 namespace=stx_auxlib::random sha256=f80e7446f46e8f64aa25c641cc320cae30e0167139f32dcca44846efc0dd4b4e
//@ mutant id=m1 source=stx_auxlib path=src/random.cpp line=226 from="offset - to_minus_one - 1" to="offset - to_minus_one"

namespace detail = stx_auxlib::random::detail;

int main() {
    const std::int64_t low = static_cast<std::int64_t>(nondet_longlong());
    const std::int64_t high = static_cast<std::int64_t>(nondet_longlong());
    STX_ASSUME(low <= high);
    const std::uint64_t span = detail::distance(low, high);
    STX_ASSERT(static_cast<__int128>(span) == static_cast<__int128>(high) - static_cast<__int128>(low), "P2: distance is exact");
    STX_ASSERT(detail::advance(low, span) == high, "P2: advance inverts distance");

    const std::int64_t start = static_cast<std::int64_t>(nondet_longlong());
    const std::uint64_t offset = static_cast<std::uint64_t>(nondet_ulonglong());
    STX_ASSUME(static_cast<__int128>(start) + static_cast<__int128>(offset) <= INT64_MAX);
    const std::int64_t reached = detail::advance(start, offset);
    STX_ASSERT(static_cast<__int128>(reached) == static_cast<__int128>(start) + static_cast<__int128>(offset), "P2: advance is exact");
    STX_VACUITY_PROBE();
    return 0;
}
