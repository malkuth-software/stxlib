// Row 21, bound-effect configuration (owner direction of 2026-11-13,
// common/effect.h): math::sincos(float) with each floating-point result
// abstracted to any value within its stated bound. sincos_core.hpp is
// lifted here (it is included as is in treatment.cpp) so that the step's
// arithmetic can be abstracted while its index mask stays exact.
// Checked: P0's control-flow part: the path predicates (bit tests on x)
//     and the table index, bits(shifted) & 255, in [0, 256) whatever the
//     arithmetic gives; every conversion to float defined for results in
//     their bounds. The mutant reads one entry past the index.
// Not checked here (numeric): the values; P4 (correct rounding) is
//     established by exhaustive enumeration (stx_auxlib/math.hpp:24-27).
// Bounds (each an obligation, established outside this run):
//     shifted in 1.5 * 2^52 +- 2^38 and q in +-2^38, for |x| <= 2^32
//         (|x| * 128/pi < 2^38);
//     |r| <= 2^-4 (q within one of x * 128/pi: |r| <= 1.5 * pi/128 <
//         2^-4): exhaustive over the 2^32 floats, a compiled enumeration
//         on the M4, still to run;
//     r2, sin_r, cos_r_minus_1, the corrections and the sums: interval
//         arithmetic from |r| <= 2^-4 and the table's |s|, |c| <= 1 (the
//         256 entries, still to check).
#include "effect.h"
#include "verify.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

#include "stx_auxlib/math.hpp"

//@ lift source=stx_auxlib path=src/math/sincos_core.hpp lines=all sha256=bc7aa59bb172309901260fdb44d0d986b13eb2b8403563cde59a7ee43deefca1
//@ abstract source=stx_auxlib path=src/math/sincos_core.hpp line=83 from="x * kernel_constants.sincos_inverse_step + kernel_constants.rounding_shift" to="stx_effect_f64_given(x >= -0x1p32 && x <= 0x1p32, 0x1.8p52 - 0x1p38, 0x1.8p52 + 0x1p38)" bound="shifted in 1.5 * 2^52 +- 2^38 for |x| <= 2^32"
//@ abstract source=stx_auxlib path=src/math/sincos_core.hpp line=86 from="shifted - kernel_constants.rounding_shift" to="stx_effect_f64(-0x1p38, 0x1p38)" bound="q in +-2^38"
//@ lift source=stx_auxlib path=src/math/sincos.cpp lines=all sha256=ea2c4abe051a70c2e45651e1030a7e4ec6da12ecf1f83dfe9cbaaa10228720e2
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=6 from="#include \"sincos_core.hpp\"" to="// sincos_core.hpp is lifted above" reason="the header is lifted so its arithmetic can be abstracted"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=24 from="sincos_remainder(x, q)" to="stx_effect_f64(-0x1p-4, 0x1p-4)" bound="|r| <= 2^-4"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=29 from="sincos_remainder_medium(x, q, r, r_lo);" to="r = stx_effect_f64(-0x1p-4, 0x1p-4);" bound="|r| <= 2^-4"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=55 from="r * r" to="stx_effect_f64_given(r >= -0x1p-4 && r <= 0x1p-4, 0.0, 0x1p-8)" bound="r2 in [0, 2^-8]"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=56 from="r + r * (r2 * (detail::sin_3 + r2 * detail::sin_5))" to="stx_effect_f64_given(r2 >= 0.0 && r2 <= 0x1p-8, -0x1.1p-4, 0x1.1p-4)" bound="|sin_r| <= 1.0625 * 2^-4"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=58 from="(r2 * r2) * (detail::cos_4 + r2 * detail::cos_6) - r2 * detail::cos_half" to="stx_effect_f64_given(r2 >= 0.0 && r2 <= 0x1p-8, -0x1p-8, 0x1p-16)" bound="cos_r_minus_1 in [-2^-8, 2^-16]"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=60 from="entry.s * cos_r_minus_1 + entry.c * sin_r" to="stx_effect_f64(-0x1p-3, 0x1p-3)" bound="|sin_correction| <= 2^-3"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=61 from="entry.c * cos_r_minus_1 - entry.s * sin_r" to="stx_effect_f64(-0x1p-3, 0x1p-3)" bound="|cos_correction| <= 2^-3"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=62 from="entry.s + sin_correction" to="stx_effect_f64(-1.125, 1.125)" bound="|sin| before rounding <= 1.125"
//@ abstract source=stx_auxlib path=src/math/sincos.cpp line=63 from="entry.c + cos_correction" to="stx_effect_f64(-1.125, 1.125)" bound="|cos| before rounding <= 1.125"
//@ lift source=stx_auxlib path=src/math/constants.cpp lines=all sha256=d4efbc75fdebadf14484a33bc434641b7b95bf5fcf17947348b9f836293cd038
//@ subst source=stx_auxlib path=src/math/constants.cpp line=9 from="#include \"sincos_core.hpp\"" to="// sincos_core.hpp is lifted above" reason="the header is lifted so its arithmetic can be abstracted"
//@ mutant id=m1 source=stx_auxlib path=src/math/sincos.cpp line=59 from="[index]" to="[index + 1]"

int main() {
    const float angle = nondet_float();
    float sine = 0.0f;
    float cosine = 0.0f;
    stx_auxlib::math::native::sincos(angle, sine, cosine);
    STX_VACUITY_PROBE();
    return 0;
}
