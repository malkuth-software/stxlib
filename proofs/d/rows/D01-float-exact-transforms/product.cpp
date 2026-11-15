// Row D01 (phase D, targeted), product: float_exact::raw::two_product_dekker
// <float> (src/core/float_exact.hpp), bit-precise.
// P1: for every pair in two_product's range (F3 = 1: a zero, or both in
//     split's range with exponent(a) + exponent(b) >= -80) whose product is
//     finite, the error is exactly the product's rounding error: a * b is
//     exact in double (48 significant bits), and so is a * b - value; and
//     the error is zero or normal.
// Bound: none (every float pair).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=65-85 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=90-178 namespace=stxlib::float_exact sha256=dd429e02a06a83cf766d77c33f0eeae971ecb91df97a9b84169f2e8971f78e01
//@ lift source=stxlib path=src/core/float_exact.hpp lines=185-255 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=209 from="(scaled - a)" to="(scaled + a)"

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
