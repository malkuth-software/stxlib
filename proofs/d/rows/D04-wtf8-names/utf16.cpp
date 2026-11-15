// Row D04 (phase D, targeted): Windows names as WTF-8 (src/core/wtf8.hpp
// from_utf16 and to_utf16, the WTF-8 bijection behind D13 a).
// P0: no out-of-bounds access or overflow for any unit sequence.
// P1: for every sequence u of 16-bit units of length at most N (unpaired
//     surrogates included): from_utf16(u) is well-formed WTF-8 of at most
//     3 |u| bytes, and to_utf16 of it gives u back; a false return happens
//     only when memory runs out.
// Extends tests/path_names_exhaustive.cpp's N3 (every unit, and pairs from
//     a 2,056-unit set) to every sequence of N units.
// Bound: |u| at most N (N in {2, 4}).
#include "verify.h"

#include "stxlib/types.hpp"

#include <cstdlib>
#include <cstring>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=e8c563f2e84124ab284680198c844b971d8f0e48a2c0bec2f9ee8cf400b67b7f
//@ model-begin reason="Buffer and its adoption, declared only: Growable::take_as_buffer names them and is never instantiated here"
namespace stxlib {
template <typename Element>
class Buffer;
namespace detail {
template <typename Element>
struct BufferAdoption;
} // namespace detail
} // namespace stxlib
//@ model-end
//@ lift source=stxlib path=src/core/growable.hpp lines=16-135 namespace=stxlib sha256=220990226a949df693d389e7b15c248db3374c03555f046893a6501a798d4f65
//@ subst source=stxlib path=src/core/growable.hpp line=28 from="memory::release(held)" to="std::free(held)" reason="STXlib's allocator modelled by the tool's malloc and free, which may refuse (out_of_memory paths are explored)"
//@ subst source=stxlib path=src/core/growable.hpp line=122 from="memory::allocate(next * sizeof(Element), alignof(Element))" to="std::malloc(next * sizeof(Element))" reason="STXlib's allocator modelled by the tool's malloc"
//@ subst source=stxlib path=src/core/growable.hpp line=128 from="memory::release(previous)" to="std::free(previous)" reason="STXlib's allocator modelled by the tool's free"
//@ lift source=stxlib path=include/stxlib/detail/utf8.hpp lines=30-71 namespace=stxlib::detail sha256=114b432b16110af4be99580d8b7f3a335e09622a7d9ef8f15dc70735e44e73ff
//@ lift source=stxlib path=src/core/wtf8.hpp lines=45-241 namespace=stxlib::wtf8 sha256=33331acebd911cc031c421f1a13a4e790f5d6a2641ca413c68daaef9af4b60b5
//@ mutant id=m1 source=stxlib path=src/core/wtf8.hpp line=209 from="low <= 0xdfffU" to="low <= 0xdbffU"

#ifndef STX_N
#define STX_N 4
#endif

using stxlib::usize;
namespace w = stxlib::wtf8;

int main() {
    constexpr usize bound = STX_N;
    stxlib::u16 storage[bound];
    for (usize index = 0; index < bound; ++index) storage[index] = static_cast<stxlib::u16>(nondet_ushort());
    const usize count = nondet_size();
    STX_ASSUME(count <= bound);
    const stxlib::u16* const units = storage + (bound - count);

    stxlib::Growable<char> text;
    if (!w::from_utf16(units, count, text)) return 0;  // memory ran out
    STX_ASSERT(text.count() <= 3 * count, "at most three bytes per unit");
    STX_ASSERT(w::well_formed(text.data(), text.count()), "the WTF-8 is well-formed");
    stxlib::Growable<stxlib::u16> back;
    if (!w::to_utf16(text.data(), text.count(), back)) return 0;  // memory ran out (the text is well-formed)
    STX_ASSERT(back.count() == count, "the round trip keeps the unit count");
    for (usize index = 0; index < count; ++index) {
        STX_ASSERT(back[index] == units[index], "the round trip keeps every unit");
    }
    STX_VACUITY_PROBE();
    return 0;
}
