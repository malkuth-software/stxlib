// Row 15: random::detail::multiply (B1.md section 3.1).
// P2: (high, low) is the 128-bit product of the two operands. The kernel is
//     defined modulo 2^64, so P0 holds trivially (no signed arithmetic); the
//     row is kept as a solver-strength measure (a multiplier equivalence).
// Bound: the whole 64 x 64-bit domain; no loop.
#include "verify.h"

#include <cstdint>

//@ lift source=stx_auxlib path=include/stx_auxlib/random.hpp lines=140-148 namespace=stx_auxlib::random::detail sha256=1c397ea0a4db64c3e4a90300a2b19b157ccb4591081412a19d6e6eefeae94404
//@ lift source=stx_auxlib path=src/random.cpp lines=195-210 namespace=stx_auxlib::random sha256=9a3bc204df677d1e4b979446efe69ef3674c5864db9000d951d9f899805f62d5
//@ mutant id=m1 source=stx_auxlib path=src/random.cpp line=205 from=" + low_high" to=""

int main() {
    const std::uint64_t left = static_cast<std::uint64_t>(nondet_ulonglong());
    const std::uint64_t right = static_cast<std::uint64_t>(nondet_ulonglong());
    const stx_auxlib::random::detail::Product product = stx_auxlib::random::detail::multiply(left, right);
    const unsigned __int128 exact = static_cast<unsigned __int128>(left) * static_cast<unsigned __int128>(right);
    STX_ASSERT(product.high == static_cast<std::uint64_t>(exact >> 64), "P2: the high half of the product");
    STX_ASSERT(product.low == static_cast<std::uint64_t>(exact), "P2: the low half of the product");
    STX_VACUITY_PROBE();
    return 0;
}
