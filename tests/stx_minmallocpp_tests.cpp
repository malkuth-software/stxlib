// Exhaustive checks of stx_minmallocpp's arithmetic (src/state/
// stx_minmallocpp/layout.hpp, page.hpp, region.hpp). Each loop covers its
// whole domain, so a pass is a proof by enumeration of the property named
// (proofs/REPORT.md, stx_minmallocpp):
//
//   classes     for every size from 1 to 524,288, its class holds it and no
//               smaller class does;
//   alignment   for every alignment from 16 to 65,536 and every size whose
//               rounding stays within the classes, the class of the rounded
//               size has a block size that is a multiple of the alignment;
//   index       for every class and every byte offset of its page, the
//               multiply-and-shift block index equals the division;
//   run lists   for every run length from 1 to 16,383, the list that holds
//               it starts at or below it and the next list starts above it,
//               and list floors increase.
//
// Header-only: nothing of the allocator is linked.

#include "state/stx_minmallocpp/region.hpp"

#include "test_support.hpp"

namespace {

using stxlib::u64;
using stxlib::usize;
using namespace stxlib::stx_minmallocpp;

void classes_hold_every_size() {
    bool holds = true;
    bool smallest = true;
    for (usize bytes = 1; bytes <= largest_class_bytes; ++bytes) {
        const usize index = class_of(bytes);
        if (index >= class_count || class_bytes(index) < bytes) holds = false;
        if (index > 0 && class_bytes(index - 1) >= bytes) smallest = false;
    }
    CHECK(holds);
    CHECK(smallest);
}

void rounded_sizes_are_aligned() {
    bool aligned = true;
    usize cases = 0;
    for (usize alignment = block_alignment; alignment <= class_alignment_limit; alignment *= 2) {
        for (usize bytes = 1; bytes <= largest_class_bytes; ++bytes) {
            const usize rounded = round_up(bytes, alignment);
            if (rounded > largest_class_bytes) break;
            if (class_bytes(class_of(rounded)) % alignment != 0) aligned = false;
            cases = cases + 1;
        }
    }
    CHECK(aligned);
    CHECK(cases > 6000000);
}

void block_index_is_the_division() {
    bool exact = true;
    for (usize index = 0; index < class_count; ++index) {
        const ClassShape& shape = class_table.shapes[index];
        const u64 page_bytes = static_cast<u64>(shape.page_slices) * slice_bytes;
        for (u64 offset = 0; offset < page_bytes; ++offset) {
            if (block_index(offset, shape.divide_magic) != offset / shape.block_bytes) exact = false;
        }
    }
    CHECK(exact);
    // A page of one block: every offset is in block 0.
    CHECK(block_index(0, 0) == 0);
    CHECK(block_index(static_cast<u64>(huge_threshold) - 1, 0) == 0);
}

// The list after `list`, in the order the index is searched.
RunList next_list(const RunList list) {
    RunList result = list;
    const usize seconds = list.first < 2 ? (static_cast<usize>(1) << list.first) : run_second_levels;
    result.second = result.second + 1;
    if (result.second == seconds) {
        result.first = result.first + 1;
        result.second = 0;
    }
    return result;
}

void run_lists_partition_the_lengths() {
    bool floor_at_or_below = true;
    bool next_above = true;
    bool first_in_range = true;
    for (usize length = 1; length < region_slices; ++length) {
        const RunList list = run_list_of(length);
        if (list.first >= run_first_levels || list.second >= run_second_levels) first_in_range = false;
        if (run_list_floor(list) > length) floor_at_or_below = false;
        const RunList after = next_list(list);
        if (after.first < run_first_levels && run_list_floor(after) <= length) next_above = false;
    }
    CHECK(first_in_range);
    CHECK(floor_at_or_below);
    CHECK(next_above);
    // The lengths the allocator asks for most are a list's smallest.
    CHECK(run_list_floor(run_list_of(small_page_slices)) == small_page_slices);
    CHECK(run_list_floor(run_list_of(medium_page_slices)) == medium_page_slices);
    CHECK(run_list_floor(run_list_of(large_page_slices)) == large_page_slices);
}

void layout_adds_up() {
    CHECK(first_data_slice * slice_bytes == 18 * unit_bytes);
    CHECK(region_slices - first_data_slice == 16096);
    CHECK(huge_threshold / slice_bytes == 512);
    CHECK(class_table.shapes[0].capacity == page_blocks_limit);
    CHECK(class_table.shapes[class_count - 1].capacity == 8);
}

} // namespace

int main() {
    classes_hold_every_size();
    rounded_sizes_are_aligned();
    block_index_is_the_division();
    run_lists_partition_the_lengths();
    layout_adds_up();
    const int status = stxlib_tests::finish("stxlib stx_minmallocpp arithmetic");
    return status;
}
