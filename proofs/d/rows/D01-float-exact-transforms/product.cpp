// Row D01 (phase D, targeted), product: float_exact::raw::two_product_dekker
// <float> (src/core/float_exact.hpp), bit-precise.
// P1: for every pair in two_product's range (F3 = 1: both in split's range,
//     a zero included, and for nonzero operands -80 <= exponent(a) +
//     exponent(b) <= 125; the zero rule and the upper bound added after the
//     M5 counterexamples a = -0, b = -FLT_MAX (NaN) and a = 0x5edbbe36,
//     b = 0x60151e43 (an infinite error)) whose product is
//     finite, the error is exactly the product's rounding error: a * b is
//     exact in double (48 significant bits), and so is a * b - value; and
//     the error is zero or normal.
// Bound: none (every float pair).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=96-116 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=121-238 namespace=stxlib::float_exact sha256=34db4ddd7d8dbe16480845b870b984f00c68f7a3a33268116e88b207eebea1f9
//@ lift source=stxlib path=src/core/float_exact.hpp lines=245-315 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=269 from="(scaled - a)" to="(scaled + a)"

namespace fe = stxlib::float_exact;

int main() {
    const float a = nondet_float();
    const float b = nondet_float();
    STX_ASSUME(fe::in_product_range(a, b));
    const fe::Pair<float> product = fe::raw::two_product_dekker(a, b);
    STX_ASSUME(fe::finite(product.value));
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    STX_ASSERT(static_cast<double>(product.error) == exact - static_cast<double>(product.value),
               "Dekker's product error is exact");
    STX_ASSERT(fe::zero_or_normal(product.error), "the product's error is zero or normal");
    STX_VACUITY_PROBE();
    return 0;
}
