// Row 02, bound-effect configuration (owner direction of 2026-11-13,
// common/effect.h): checked::multiply_saturating with its quotient and
// product abstracted to any value within their stated bounds.
// Checked: P0's control-flow part: the division on line 36 is reached only
//     with left != 0 (the short-circuit guard), asserted where the quotient
//     is abstracted. The mutant removes the guard.
// Not checked here (numeric, in the row's record): P2.
// Bounds: SIZE_MAX / left in [1, SIZE_MAX] for left >= 1; left * right in
//     [0, SIZE_MAX] (size_t arithmetic is modular). Both follow from the
//     types; no further obligation.
#include "effect.h"
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <limits>

//@ lift source=stx_auxlib path=src/checked.hpp lines=34-40 namespace=stx_auxlib::checked sha256=6b7ff4bed5f86774c7ea6d33bac14b9d15d4bf27543b6ed0926b8209683d1da3
//@ abstract source=stx_auxlib path=src/checked.hpp line=36 from="std::numeric_limits<std::size_t>::max() / left" to="stx_effect_u64_given(left != 0, 1, SIZE_MAX)" bound="SIZE_MAX / left in [1, SIZE_MAX] for left >= 1"
//@ abstract source=stx_auxlib path=src/checked.hpp line=38 from="left * right" to="stx_effect_u64(0, SIZE_MAX)" bound="left * right in [0, SIZE_MAX] (modular)"
//@ mutant id=m2 source=stx_auxlib path=src/checked.hpp line=36 from="left != 0 && " to=""

int main() {
    const std::size_t left = nondet_size();
    const std::size_t right = nondet_size();
    const std::size_t result = stx_auxlib::checked::multiply_saturating(left, right);
    (void)result;
    STX_VACUITY_PROBE();
    return 0;
}
