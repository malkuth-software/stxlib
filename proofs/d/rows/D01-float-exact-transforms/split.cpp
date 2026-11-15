// Row D01 (phase D, targeted), split: float_exact::raw::split<float>
// (Veltkamp's split, src/core/float_exact.hpp), bit-precise.
// P1: for every float in split's range (zero, or exponent >= -103 and
//     |a| <= 2^115), hi + lo == a exactly (lo == a - hi, computed exactly in
//     double: a and hi are floats within a few binades of each other), each
//     half is zero or normal, and each has at most 12 significant bits (the
//     low 12 bits of a normal half's 24-bit significand are zero).
// Bound: none (every float). tests/float_exact_tests.cpp --exhaustive also
//     enumerates every float; this is the solver's independent proof.
#include "verify.h"

#include <bit>
#include <cstdint>

//@ lift source=stxlib path=src/core/float_exact.hpp lines=91-111 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=116-232 namespace=stxlib::float_exact sha256=f036384bf9c4cfaf177b2a2d8c6a35286aa7937724f80e9927b3151e68bb97d6
//@ lift source=stxlib path=src/core/float_exact.hpp lines=239-309 namespace=stxlib::float_exact::raw sha256=739554fe1becd806427b891bcedf279c360bbd1acf95318e2c2aad319de1f46a
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=263 from="(scaled - a)" to="(scaled + a)"

namespace fe = stxlib::float_exact;

namespace {

bool at_most_12_bits(const float value) {
    if (value == 0.0f) return true;
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    return (bits & 0xfffU) == 0;
}

} // namespace

int main() {
    const float a = nondet_float();
    STX_ASSUME(fe::in_split_range(a));
    const fe::Pair<float> parts = fe::raw::split(a);
    STX_ASSERT(static_cast<double>(parts.error) == static_cast<double>(a) - static_cast<double>(parts.value),
               "the split is exact");
    STX_ASSERT(fe::zero_or_normal(parts.value) && fe::zero_or_normal(parts.error), "both halves zero or normal");
    STX_ASSERT(at_most_12_bits(parts.value) && at_most_12_bits(parts.error), "each half has at most 12 bits");
    STX_VACUITY_PROBE();
    return 0;
}
