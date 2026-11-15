// ESBMC harnesses: stx_minmallocpp's arithmetic (src/state/stx_minmallocpp/
// layout.hpp, page.hpp, region.hpp). Each property is also checked
// exhaustively by tests/stx_minmallocpp_tests.cpp; these state it for the
// solver, over the whole domain, with no loop to unwind.
//
// Not yet run. Intended command (the pinned ESBMC of notes
// planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/stx_minmallocpp.cpp --std c++20 -I include -I src \
//     --function <harness>
//
// Not covered here, and not yet proved anywhere: the ownership protocol
// (heap.cpp: release, collect, abandon, claim across threads), the free-run
// index's invariants (region.cpp), and the page bitmap operations against a
// set model. proofs/REPORT.md lists them.

#include "state/stx_minmallocpp/region.hpp"

extern "C" unsigned long long __VERIFIER_nondet_ulonglong(void);
extern "C" void __ESBMC_assume(bool);
extern "C" void __ESBMC_assert(bool, const char*);

using stxlib::u64;
using stxlib::usize;
using namespace stxlib::stx_minmallocpp;

// class_of: the class holds the size, and the class before it does not.
void harness_class_of() {
    const usize bytes = static_cast<usize>(__VERIFIER_nondet_ulonglong());
    __ESBMC_assume(bytes >= 1 && bytes <= largest_class_bytes);
    const usize index = class_of(bytes);
    __ESBMC_assert(index < class_count, "a class");
    __ESBMC_assert(class_bytes(index) >= bytes, "the class holds the size");
    __ESBMC_assert(index == 0 || class_bytes(index - 1) < bytes, "no smaller class does");
    __ESBMC_assert(class_bytes(index) % block_alignment == 0, "a multiple of 16");
}

// The alignment rule (layout.hpp): the class of a size rounded up to a
// power-of-two alignment from 16 to 65,536 has a block size that is a
// multiple of that alignment.
void harness_alignment_rule() {
    const usize bytes = static_cast<usize>(__VERIFIER_nondet_ulonglong());
    const usize shift = static_cast<usize>(__VERIFIER_nondet_ulonglong());
    __ESBMC_assume(shift >= 4 && shift <= 16);
    const usize alignment = static_cast<usize>(1) << shift;
    __ESBMC_assume(bytes >= 1 && bytes <= largest_class_bytes);
    const usize rounded = round_up(bytes, alignment);
    __ESBMC_assume(rounded <= largest_class_bytes);
    const usize block = class_bytes(class_of(rounded));
    __ESBMC_assert(block % alignment == 0, "the block size is a multiple of the alignment");
    __ESBMC_assert(block >= bytes, "and holds the size");
}

// block_index: the multiply and shift is the division, for every class and
// every offset of its page.
void harness_block_index() {
    const usize index = static_cast<usize>(__VERIFIER_nondet_ulonglong());
    __ESBMC_assume(index < class_count);
    const ClassShape shape = shape_of_class(index);
    const u64 offset = __VERIFIER_nondet_ulonglong();
    __ESBMC_assume(offset < static_cast<u64>(shape.page_slices) * slice_bytes);
    __ESBMC_assert(block_index(offset, shape.divide_magic) == offset / shape.block_bytes, "the index is the quotient");
}

// Run lists: the list of a length starts at or below it.
void harness_run_list_floor() {
    const usize length = static_cast<usize>(__VERIFIER_nondet_ulonglong());
    __ESBMC_assume(length >= 1 && length < region_slices);
    const RunList list = run_list_of(length);
    __ESBMC_assert(list.first < run_first_levels && list.second < run_second_levels, "a list");
    __ESBMC_assert(run_list_floor(list) <= length, "the list's smallest length is at or below");
    __ESBMC_assert(run_list_of(run_list_floor(list)).first == list.first &&
                       run_list_of(run_list_floor(list)).second == list.second,
                   "the smallest length is in the list");
}
