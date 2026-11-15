// Row 01: stx_auxlib::checked::add_saturating (B1.md section 3.1).
// P0: no undefined behaviour for any pair of size_t.
// P2: the result is min(left + right, SIZE_MAX), computed in 128 bits.
// Bound: the whole 64-bit domain; the function has no loop.
#include "verify.h"

#include <cstddef>
#include <limits>

//@ lift source=stx_auxlib path=src/checked.hpp lines=27-31 namespace=stx_auxlib::checked sha256=9876341cc7ada133234c8923111088d3c725908dd9053937ea3f0b96aabcc56c
//@ mutant id=m1 source=stx_auxlib path=src/checked.hpp line=28 from="return std::numeric_limits<std::size_t>::max();" to="return std::numeric_limits<std::size_t>::max() - 1;"

int main() {
    const std::size_t left = nondet_size();
    const std::size_t right = nondet_size();
    const std::size_t result = stx_auxlib::checked::add_saturating(left, right);
    const unsigned __int128 exact = static_cast<unsigned __int128>(left) + static_cast<unsigned __int128>(right);
    const unsigned __int128 largest = std::numeric_limits<std::size_t>::max();
    const unsigned __int128 expected = exact < largest ? exact : largest;
    STX_ASSERT(static_cast<unsigned __int128>(result) == expected, "P2: add_saturating is min(l + r, SIZE_MAX)");
    STX_VACUITY_PROBE();
    return 0;
}
