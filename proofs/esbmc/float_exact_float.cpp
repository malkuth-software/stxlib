// ESBMC harness: the float error-free transformations of
// src/core/float_exact.hpp, bit-precise (SMT floating-point theory).
// Not yet run. Intended command (the pinned ESBMC of
// notes planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/float_exact_float.cpp --std c++20 -I src -I include \
//     --floatbv --function <harness>
//
// for each harness below. Each assumes the primitive's range (and, for the
// sum, an exponent gap of at most 28, so the check in double is exact) and
// asserts exactness checked in double, where every value involved is
// exactly representable: a float sum within that gap, a float product (48
// significant bits), and the pair value + error.

#include "core/float_exact.hpp"

extern "C" float __VERIFIER_nondet_float(void);
extern "C" void __ESBMC_assume(_Bool);
extern "C" void __ESBMC_assert(_Bool, const char*);

namespace fe = stxlib::float_exact;

void harness_two_sum() {
    const float a = __VERIFIER_nondet_float();
    const float b = __VERIFIER_nondet_float();
    __ESBMC_assume(fe::in_sum_range(a) && fe::in_sum_range(b));
    __ESBMC_assume(a == 0.0f || b == 0.0f ||
                   (fe::exponent_of(a) - fe::exponent_of(b) <= 28 && fe::exponent_of(b) - fe::exponent_of(a) <= 28));
    const fe::Pair<float> sum = fe::raw::two_sum(a, b);
    __ESBMC_assume(fe::finite(sum.value));
    __ESBMC_assert(static_cast<double>(sum.value) + static_cast<double>(sum.error) ==
                       static_cast<double>(a) + static_cast<double>(b),
                   "two_sum is exact");
    __ESBMC_assert(fe::zero_or_normal(sum.error), "two_sum's error is zero or normal");
}

void harness_fast_two_sum() {
    const float a = __VERIFIER_nondet_float();
    const float b = __VERIFIER_nondet_float();
    __ESBMC_assume(fe::in_sum_range(a) && fe::in_sum_range(b));
    __ESBMC_assume(a == 0.0f || b == 0.0f || fe::exponent_of(a) >= fe::exponent_of(b));
    const fe::Pair<float> fast = fe::raw::fast_two_sum(a, b);
    const fe::Pair<float> sum = fe::raw::two_sum(a, b);
    __ESBMC_assume(fe::finite(sum.value));
    __ESBMC_assert(fast.value == sum.value && fast.error == sum.error, "fast_two_sum equals two_sum");
}

void harness_two_product() {
    const float a = __VERIFIER_nondet_float();
    const float b = __VERIFIER_nondet_float();
    __ESBMC_assume(fe::in_product_range(a, b));
    const fe::Pair<float> dekker = fe::raw::two_product_dekker(a, b);
    __ESBMC_assume(fe::finite(dekker.value));
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    __ESBMC_assert(static_cast<double>(dekker.value) + static_cast<double>(dekker.error) == exact,
                   "Dekker's product is exact");
    __ESBMC_assert(fe::zero_or_normal(dekker.error), "the product's error is zero or normal");
    const fe::Pair<float> fused = fe::raw::two_product_fused(a, b);
    __ESBMC_assert(fused.value == dekker.value && fused.error == dekker.error, "the two forms agree");
}

void harness_split() {
    const float a = __VERIFIER_nondet_float();
    __ESBMC_assume(fe::in_split_range(a));
    const fe::Pair<float> parts = fe::raw::split(a);
    __ESBMC_assert(static_cast<double>(parts.value) + static_cast<double>(parts.error) == static_cast<double>(a),
                   "the split is exact");
}
