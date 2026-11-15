// Row D01 (phase D, targeted), product_fused: float_exact's two product
// forms agree (src/core/float_exact.hpp, F4 = 1: identical bits from the
// fused and Dekker forms, the error term being exact and so unique).
// P1: for every pair in two_product's range (both in split's range, and
//     for nonzero operands -80 <= exponent(a) + exponent(b) <= 125) with a
//     finite product,
//     raw::two_product_fused and raw::two_product_dekker give the same value
//     and error, bit for bit.
// Needs the tool's model of __builtin_fmaf (a correctly rounded fused
// multiply-add); where the tool has none the run is a tool gap, recorded,
// and the agreement rests on product.cpp plus the uniqueness argument.
// Bound: none (every float pair).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=91-111 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=116-232 namespace=stxlib::float_exact sha256=f036384bf9c4cfaf177b2a2d8c6a35286aa7937724f80e9927b3151e68bb97d6
//@ lift source=stxlib path=src/core/float_exact.hpp lines=239-309 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=303 from="-product" to="product"

namespace fe = stxlib::float_exact;

int main() {
    const float a = nondet_float();
    const float b = nondet_float();
    STX_ASSUME(fe::in_product_range(a, b));
    const fe::Pair<float> dekker = fe::raw::two_product_dekker(a, b);
    STX_ASSUME(fe::finite(dekker.value));
    const fe::Pair<float> fused = fe::raw::two_product_fused(a, b);
    STX_ASSERT(std::bit_cast<std::uint32_t>(fused.value) == std::bit_cast<std::uint32_t>(dekker.value) &&
                   std::bit_cast<std::uint32_t>(fused.error) == std::bit_cast<std::uint32_t>(dekker.error),
               "the fused and Dekker forms agree bit for bit");
    STX_VACUITY_PROBE();
    return 0;
}
