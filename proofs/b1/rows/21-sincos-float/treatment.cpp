// Row 21: math::sincos(float, float&, float&) with float_remainder and
// sincos_core.hpp (B1.md section 3.1), the native variant without FMA
// (STX_AUXLIB_MATH_DEKKER, as the build's Dekker configuration).
// P0: no undefined behaviour for every float: the table index in [0, 256),
//     every conversion defined. P4 (correct rounding) is established by
//     exhaustive enumeration (include/stx_auxlib/math.hpp:24-27) and is not
//     re-run.
// Bound: all 2^32 float bit patterns, symbolic. Unwind 257 in case a tool
//        evaluates the constexpr cut table (sincos_core.hpp:56-66) as code.
// kernel_constants (declared extern in constants.hpp) is defined by lifting
// constants.cpp; before 2026-11-13's fix it was left undefined, so the
// tools saw arbitrary constants and those runs are void.
#include "verify.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

//@ lift source=stx_auxlib path=src/math/sincos.cpp lines=all sha256=ea2c4abe051a70c2e45651e1030a7e4ec6da12ecf1f83dfe9cbaaa10228720e2
//@ lift source=stx_auxlib path=src/math/constants.cpp lines=all sha256=d4efbc75fdebadf14484a33bc434641b7b95bf5fcf17947348b9f836293cd038
//@ mutant id=m1 source=stx_auxlib path=src/math/sincos.cpp line=59 from="[index]" to="[index + 1]"

int main() {
    const float angle = nondet_float();
    float sine = 0.0f;
    float cosine = 0.0f;
    stx_auxlib::math::native::sincos(angle, sine, cosine);
    STX_VACUITY_PROBE();
    return 0;
}
