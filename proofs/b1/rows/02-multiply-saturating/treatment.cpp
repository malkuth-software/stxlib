// Row 02: stx_auxlib::checked::multiply_saturating (B1.md section 3.1).
// P0: no undefined behaviour for any pair of size_t.
// P2: the result is min(left * right, SIZE_MAX), computed in 128 bits.
// Bound: the whole 64-bit domain; no loop.
#include "verify.h"

#include <cstddef>
#include <limits>

//@ lift source=stx_auxlib path=src/checked.hpp lines=34-40 namespace=stx_auxlib::checked sha256=6b7ff4bed5f86774c7ea6d33bac14b9d15d4bf27543b6ed0926b8209683d1da3
//@ mutant id=m1 source=stx_auxlib path=src/checked.hpp line=36 from="right > std" to="right >= std"

int main() {
    const std::size_t left = nondet_size();
    const std::size_t right = nondet_size();
    const std::size_t result = stx_auxlib::checked::multiply_saturating(left, right);
    const unsigned __int128 exact = static_cast<unsigned __int128>(left) * static_cast<unsigned __int128>(right);
    const unsigned __int128 largest = std::numeric_limits<std::size_t>::max();
    const unsigned __int128 expected = exact < largest ? exact : largest;
    STX_ASSERT(static_cast<unsigned __int128>(result) == expected, "P2: multiply_saturating is min(l * r, SIZE_MAX)");
    STX_VACUITY_PROBE();
    return 0;
}
