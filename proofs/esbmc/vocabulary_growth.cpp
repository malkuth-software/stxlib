// ESBMC harness: the owned vocabulary's growth policy
// (include/stxlib/detail/growth.hpp) over 64-bit operands, bit-precise.
// Not yet run. Intended command (the pinned ESBMC of notes
// planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/vocabulary_growth.cpp --std c++20 -I include \
//     --function harness_grown_capacity --unwind 66
//
// The doubling loop runs at most 64 times (each step doubles a value of at
// least 1 that stays below 2^64), so an unwinding bound of 66 is complete;
// ESBMC's unwinding assertion (on by default) confirms it. The 8-bit
// version of the same properties is checked by exhaustion in
// tests/owned_vocabulary_tests.cpp (growth_policy_exhaustive_8bit).

#include "stxlib/detail/growth.hpp"

extern "C" unsigned long __VERIFIER_nondet_ulong(void);
extern "C" void __ESBMC_assume(bool);
extern "C" void __ESBMC_assert(bool, const char*);

using stxlib::usize;

void harness_grown_capacity() {
    const usize current = __VERIFIER_nondet_ulong();
    const usize wanted = __VERIFIER_nondet_ulong();
    const usize element_size = __VERIFIER_nondet_ulong();
    __ESBMC_assume(element_size > 0);
    const usize maximum = stxlib::usize_max / element_size;
    __ESBMC_assume(current <= maximum);
    const usize result = stxlib::detail::grown_capacity<usize>(current, wanted, element_size);
    if (wanted > maximum) {
        __ESBMC_assert(result == 0, "an unrepresentable request yields 0");
        return;
    }
    __ESBMC_assert(result >= wanted, "the result holds what is wanted");
    __ESBMC_assert(result >= current, "the result never shrinks");
    __ESBMC_assert(result <= maximum, "the result's byte size is representable");
    if (wanted <= current) __ESBMC_assert(result == current, "no growth when there is room");
}
