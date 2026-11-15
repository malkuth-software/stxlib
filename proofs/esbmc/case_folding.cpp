// ESBMC harnesses: simple case folding's core
// (src/algorithms/case_folding_core.hpp; include/stxlib/case_folding.hpp;
// M2 C7). Memory safety (every table and text access in bounds) is checked
// by ESBMC on every access; the assertions add the stated properties.
//
// Not yet run. Intended command (the pinned ESBMC of
// notes planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/case_folding.cpp --std c++20 -I src -I include \
//     --function <harness> --unwind <bound> --unwinding-assertions
//
// with <bound> 1535 for harness_fold_scalar (the restating scan over the
// 1,533 entries) and 14 for the text harnesses (the lookup halves 1,533 to
// nothing in 11 steps; texts are at most 8 bytes). --unwinding-assertions
// makes a bound that is too small fail the run rather than pass it.
//
// Scope: harness_fold_scalar covers every u32 (all of the domain);
// the text harnesses cover well-formed texts of at most 8 bytes.
//
// Pointer width: run each harness twice, as above (64-bit) and with
// `--32 -D__wasm32__` added (wasm32's 32-bit usize and uptr; types.hpp
// admits 32-bit pointers only under __wasm32__, and the cores here have no
// other __wasm32__ branch). ESBMC's --32 and -D options are as its --help
// lists them; unconfirmed at the pinned ESBMC (VERIFICATION_PROGRAMME).

#include "algorithms/case_folding_core.hpp"
#include "stxlib/detail/utf8.hpp"

extern "C" unsigned char __VERIFIER_nondet_uchar(void);
extern "C" unsigned int __VERIFIER_nondet_uint(void);
extern "C" unsigned long __VERIFIER_nondet_ulong(void);
extern "C" void __ESBMC_assume(_Bool);
extern "C" void __ESBMC_assert(_Bool, const char*);

namespace core = stxlib::case_folding_core;

using stxlib::u32;
using stxlib::usize;

namespace {

constexpr usize text_bound = 8;

usize nondet_count(const usize bound) {
    const usize value = static_cast<usize>(__VERIFIER_nondet_ulong());
    __ESBMC_assume(value <= bound);
    return value;
}

// A well-formed UTF-8 text of at most text_bound bytes in `bytes`.
usize nondet_text(char (&bytes)[text_bound]) {
    for (usize index = 0; index < text_bound; ++index) bytes[index] = static_cast<char>(__VERIFIER_nondet_uchar());
    const usize count = nondet_count(text_bound);
    __ESBMC_assume(stxlib::detail::utf8_well_formed(bytes, count));
    return count;
}

} // namespace

// For every u32: the folding is the table's target where the value is a
// source and the value itself otherwise (restated by a linear scan, so the
// binary search is checked against it); ASCII folds A-Z to a-z only; a
// scalar folds to a scalar; folding is idempotent.
void harness_fold_scalar() {
    const u32 code_point = __VERIFIER_nondet_uint();
    const u32 folded = core::fold(code_point);
    u32 expected = code_point;
    for (usize index = 0; index < core::case_folding_count; ++index) {
        if (core::case_folding_sources[index] == code_point) expected = core::case_folding_targets[index];
    }
    __ESBMC_assert(folded == expected, "the lookup equals the scan");
    if (code_point < 0x80u) {
        const bool upper = code_point >= 0x41u && code_point <= 0x5au;
        __ESBMC_assert(folded == (upper ? code_point + 0x20u : code_point), "ASCII folds A-Z only");
    }
    const bool scalar = code_point <= core::highest_scalar &&
                        (code_point < core::surrogate_low || code_point > core::surrogate_high);
    if (scalar) {
        const bool folded_scalar =
            folded <= core::highest_scalar && (folded < core::surrogate_low || folded > core::surrogate_high);
        __ESBMC_assert(folded_scalar, "a scalar folds to a scalar");
    }
    __ESBMC_assert(core::fold(folded) == folded, "idempotent");
}

// For every well-formed text of at most 8 bytes: the folding's size is
// within count + count / 2, writing it stays in that many bytes, and the
// bytes written are well-formed UTF-8 of the size measured.
void harness_fold_text() {
    char bytes[text_bound];
    const usize count = nondet_text(bytes);
    const usize needed = core::folded_bytes(bytes, count);
    __ESBMC_assert(needed <= count + count / 2, "the growth bound");
    char output[text_bound + text_bound / 2];
    core::fold_into(bytes, count, output);
    __ESBMC_assert(stxlib::detail::utf8_well_formed(output, needed), "the folding is well-formed UTF-8");
    __ESBMC_assert(core::folded_bytes(output, needed) == needed, "folding the folding keeps its size");
}

// compare: reflexive, antisymmetric, and zero exactly when the foldings'
// bytes are equal.
void harness_compare() {
    char left[text_bound];
    char right[text_bound];
    const usize left_count = nondet_text(left);
    const usize right_count = nondet_text(right);
    const int forward = core::compare(left, left_count, right, right_count);
    const int backward = core::compare(right, right_count, left, left_count);
    __ESBMC_assert(forward >= -1 && forward <= 1, "a sign");
    __ESBMC_assert(forward == -backward, "antisymmetric");
    __ESBMC_assert(core::compare(left, left_count, left, left_count) == 0, "reflexive");
    char left_folded[text_bound + text_bound / 2];
    char right_folded[text_bound + text_bound / 2];
    const usize left_size = core::folded_bytes(left, left_count);
    const usize right_size = core::folded_bytes(right, right_count);
    core::fold_into(left, left_count, left_folded);
    core::fold_into(right, right_count, right_folded);
    bool same_bytes = left_size == right_size;
    for (usize index = 0; same_bytes && index < left_size; ++index) {
        if (left_folded[index] != right_folded[index]) same_bytes = false;
    }
    __ESBMC_assert((forward == 0) == same_bytes, "equal exactly when the foldings are");
}
