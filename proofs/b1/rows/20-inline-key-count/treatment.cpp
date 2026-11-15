// Row 20: stx_btree::detail::InlineKeyCount::equal_range with count,
// sum_lanes and tail_mask (B1.md section 3.1), the vector-extension form
// Clang compiles.
// P0: no undefined behaviour.
// P2: begin is the number of live keys below the target and end is begin
//     plus the number equal to it, clipped to the live length: the
//     specification the plain loop (lines 122-132, compiled only without
//     GCC or Clang extensions) also computes. Slots are sorted over the
//     live length and padded with all ones after it, as the header states.
// Bound: Slot in {uint16, uint32, uint64}, Capacity in {16, 30} (with
//        uint16 and uint32, 30 adds an overlapping tail group); length at
//        most Capacity; unwind Capacity + 1.
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

//@ lift source=stx_auxlib path=btree/include/stx_btree/detail/vector_count.hpp lines=all sha256=9e0fbc0e604492019b0e8d4f9cf10541b9b6ba2f16cca7a70ca1ef6d6ef8c16d
//@ mutant id=m1 source=stx_auxlib path=btree/include/stx_btree/detail/vector_count.hpp line=50 from="begin + equal" to="begin + equal + 1"

using Slot = STX_SLOT;
constexpr std::size_t capacity = STX_CAPACITY;
using Count = stx_btree::detail::InlineKeyCount<Slot, capacity>;

int main() {
    Slot slots[capacity];
    const std::size_t length = nondet_size();
    STX_ASSUME(length <= capacity);
    for (std::size_t index = 0; index < capacity; ++index) {
        if (index < length) {
            slots[index] = static_cast<Slot>(nondet_ulonglong());
            if (index > 0) STX_ASSUME(slots[index - 1] <= slots[index]);
        } else {
            slots[index] = static_cast<Slot>(~static_cast<Slot>(0));
        }
    }
    const Slot target = static_cast<Slot>(nondet_ulonglong());
    const stx_btree::detail::EqualRange range = Count::equal_range(slots, target, length);
    std::size_t below = 0;
    std::size_t equal = 0;
    for (std::size_t index = 0; index < length; ++index) {
        if (slots[index] < target) below += 1;
        if (slots[index] == target) equal += 1;
    }
    STX_ASSERT(range.begin == below, "P2: begin counts the live keys below the target");
    STX_ASSERT(range.end == below + equal, "P2: end adds the live keys equal to the target");
    STX_VACUITY_PROBE();
    return 0;
}
