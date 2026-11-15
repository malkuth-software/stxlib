// Row D04 (phase D, targeted): Path's lossless POSIX names (owner D13 a;
// src/core/wtf8.hpp escape_posix and unescape_posix, with the generalized
// UTF-8 they use), for the solver.
// P0: no out-of-bounds access or overflow for any byte string.
// P1: for every byte string s of length at most N (every byte value):
//     escape_posix(s) is well-formed WTF-8 of at most 3 |s| bytes holding
//     no ':' and no '\\' (so Path's syntax never sees them), and
//     unescape_posix(escape_posix(s)) succeeds with s (losslessness), or
//     reports out_of_memory with nothing appended.
// Extends tests/path_names_exhaustive.cpp's N1 (every name of 1 to 3 bytes
//     over bytes 1..255 but '/') to every byte, NUL and '/' included, and
//     to N bytes.
// Bound: |s| at most N (N in {3, 5}).
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
//@ mutant id=m1 source=stxlib path=src/core/wtf8.hpp line=71 from="& 0x3fU" to="& 0x1fU"

#ifndef STX_N
#define STX_N 4
#endif

using stxlib::usize;
namespace w = stxlib::wtf8;

int main() {
    constexpr usize bound = STX_N;
    char storage[bound];
    for (usize index = 0; index < bound; ++index) storage[index] = static_cast<char>(nondet_uchar());
    const usize count = nondet_size();
    STX_ASSUME(count <= bound);
    const char* const name = storage + (bound - count);

    stxlib::Growable<char> escaped;
    if (!w::escape_posix(name, count, escaped)) return 0;  // memory ran out: the contract's failure
    STX_ASSERT(escaped.count() <= 3 * count, "an escaped name has at most three bytes per byte");
    STX_ASSERT(w::well_formed(escaped.data(), escaped.count()), "an escaped name is well-formed WTF-8");
    for (usize index = 0; index < escaped.count(); ++index) {
        STX_ASSERT(escaped[index] != ':' && escaped[index] != '\\', "an escaped name holds no ':' or '\\'");
    }

    stxlib::Growable<char> back;
    const w::Converted converted = w::unescape_posix(escaped.data(), escaped.count(), back);
    if (converted == w::Converted::out_of_memory) {
        STX_ASSERT(back.count() == 0, "a refused conversion appends nothing");
        return 0;
    }
    STX_ASSERT(converted == w::Converted::ok, "an escaped name converts back");
    STX_ASSERT(back.count() == count, "the round trip keeps the length");
    for (usize index = 0; index < count; ++index) {
        STX_ASSERT(back[index] == name[index], "the round trip keeps every byte");
    }
    STX_VACUITY_PROBE();
    return 0;
}
