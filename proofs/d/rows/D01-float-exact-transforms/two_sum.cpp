// Row D01 (phase D, targeted), two_sum: float_exact::raw::two_sum<float>
// (src/core/float_exact.hpp), bit-precise in the SMT floating-point theory.
// P1: for every pair of floats in two_sum's range (each zero or of magnitude
//     at least 2^-102 and at most 2^127, F3 = 1; the upper bound added after
//     the M5 counterexample a = 0x767e0000, b = -FLT_MAX, whose error was
//     NaN) whose sum is finite, the error is exactly the rounding error of
//     the sum (value + error == a + b in the reals; so every intermediate
//     is finite), and it is zero or normal.
// The check is exact, so a refutation cannot be hidden by rounding:
//   - exponents within 28 of each other (or a zero): a + b is exact in
//     double (at most 53 significant bits), and so is (a + b) - value
//     (value is the float rounding of a + b); the error must equal it;
//   - a larger gap: |smaller| < 2^-28 |larger| < ulp(larger) / 2, so the
//     sum rounds to the larger and the exact error is the smaller; both
//     are asserted as equalities.
// Bound: none (every float pair; no loops).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=91-111 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=116-232 namespace=stxlib::float_exact sha256=f036384bf9c4cfaf177b2a2d8c6a35286aa7937724f80e9927b3151e68bb97d6
//@ lift source=stxlib path=src/core/float_exact.hpp lines=239-309 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=244 from="+ (b - b_part)" to="- (b - b_part)"

namespace fe = stxlib::float_exact;

int main() {
    const float a = nondet_float();
    const float b = nondet_float();
    STX_ASSUME(fe::in_sum_range(a) && fe::in_sum_range(b));
    const fe::Pair<float> sum = fe::raw::two_sum(a, b);
    STX_ASSUME(fe::finite(sum.value));
    const int gap = fe::exponent_of(a) - fe::exponent_of(b);
    if (a == 0.0f || b == 0.0f || (gap <= 28 && gap >= -28)) {
        const double exact = static_cast<double>(a) + static_cast<double>(b);
        STX_ASSERT(static_cast<double>(sum.error) == exact - static_cast<double>(sum.value),
                   "two_sum's error is the exact rounding error");
    } else {
        const float larger = gap > 0 ? a : b;
        const float smaller = gap > 0 ? b : a;
        STX_ASSERT(sum.value == larger && sum.error == smaller, "two_sum is exact across a wide gap");
    }
    STX_ASSERT(fe::zero_or_normal(sum.error), "two_sum's error is zero or normal");
    STX_VACUITY_PROBE();
    return 0;
}
