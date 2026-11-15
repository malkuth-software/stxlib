// Row D01 (phase D, targeted), fast_two_sum: float_exact::raw::fast_two_sum
// <float> (src/core/float_exact.hpp), bit-precise.
// P1: for every pair in the sums' range (zero, or 2^-102 <= |x| <= 2^126,
//     which implies a finite sum: no assumption after the add)
//     with exponent(a) >= exponent(b) or
//     a == 0 (the precondition) and a finite sum, fast_two_sum gives the
//     same value as two_sum, bit for bit, and the same error as a number
//     (so it is exact by two_sum's P1, the harness two_sum.cpp of this row).
//     A zero error may differ in sign: with b == -0, fast_two_sum's error is
//     b - (sum - a) = -0 - +0 = -0, while two_sum's is +0 + -0 = +0 (M5,
//     2026-11-15: the bit-for-bit form was refuted by that case, which is
//     not a defect; the sign of a zero error carries nothing).
// Bound: none (every float pair).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=96-116 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=121-238 namespace=stxlib::float_exact sha256=34db4ddd7d8dbe16480845b870b984f00c68f7a3a33268116e88b207eebea1f9
//@ lift source=stxlib path=src/core/float_exact.hpp lines=245-315 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=259 from="(sum - a)" to="(a - sum)"

namespace fe = stxlib::float_exact;

int main() {
    const float a = nondet_float();
    const float b = nondet_float();
    STX_ASSUME(fe::in_sum_range(a) && fe::in_sum_range(b));
    STX_ASSUME(a == 0.0f || b == 0.0f || fe::exponent_of(a) >= fe::exponent_of(b));
    const fe::Pair<float> fast = fe::raw::fast_two_sum(a, b);
    const fe::Pair<float> sum = fe::raw::two_sum(a, b);
    STX_ASSERT(fe::finite(sum.value), "the sum is finite over the range");
    STX_ASSERT(std::bit_cast<std::uint32_t>(fast.value) == std::bit_cast<std::uint32_t>(sum.value) &&
                   fast.error == sum.error,
               "fast_two_sum's value is two_sum's bit for bit, and its error the same number");
    STX_VACUITY_PROBE();
    return 0;
}
