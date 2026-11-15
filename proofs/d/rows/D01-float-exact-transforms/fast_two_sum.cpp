// Row D01 (phase D, targeted), fast_two_sum: float_exact::raw::fast_two_sum
// <float> (src/core/float_exact.hpp), bit-precise.
// P1: for every pair in the sums' range with exponent(a) >= exponent(b) or
//     a == 0 (the precondition) and a finite sum, fast_two_sum gives the
//     same value and error as two_sum, bit for bit (so it is exact by
//     two_sum's P1, the harness two_sum.cpp of this row).
// Bound: none (every float pair).
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=65-85 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=90-178 namespace=stxlib::float_exact sha256=dd429e02a06a83cf766d77c33f0eeae971ecb91df97a9b84169f2e8971f78e01
//@ lift source=stxlib path=src/core/float_exact.hpp lines=185-255 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=199 from="(sum - a)" to="(a - sum)"

namespace fe = stxlib::float_exact;

int main() {
    const float a = nondet_float();
    const float b = nondet_float();
    STX_ASSUME(fe::in_sum_range(a) && fe::in_sum_range(b));
    STX_ASSUME(a == 0.0f || b == 0.0f || fe::exponent_of(a) >= fe::exponent_of(b));
    const fe::Pair<float> fast = fe::raw::fast_two_sum(a, b);
    const fe::Pair<float> sum = fe::raw::two_sum(a, b);
    STX_ASSUME(fe::finite(sum.value));
    STX_ASSERT(std::bit_cast<std::uint32_t>(fast.value) == std::bit_cast<std::uint32_t>(sum.value) &&
                   std::bit_cast<std::uint32_t>(fast.error) == std::bit_cast<std::uint32_t>(sum.error),
               "fast_two_sum equals two_sum bit for bit");
    STX_VACUITY_PROBE();
    return 0;
}
