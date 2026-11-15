// Row D04 (phase D, targeted): unescape_posix accepts only canonical text
// (src/core/wtf8.hpp; the injectivity half of D13 a's losslessness).
// P0: no out-of-bounds access or overflow for any byte string.
// P1: for every byte string t of length at most N, appended to an output
//     already holding one byte: when unescape_posix succeeds, escaping the
//     bytes it appended gives t back (so two different texts never name
//     the same POSIX file); when it fails, the output is unchanged.
// Bound: |t| at most N (N in {3, 6}).
#include "verify.h"

#include "stxlib/types.hpp"

#include <cstdlib>
#include <cstring>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=7d1b60e63a2dc6a28180f30d1941df8b628ea53c676cb251c65495c2a02f4b58
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
//@ mutant id=m1 source=stxlib path=src/core/wtf8.hpp line=191 from="output.shrink(start);" to=""

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
    const char* const text = storage + (bound - count);

    stxlib::Growable<char> output;
    if (!output.push('x')) return 0;
    const w::Converted converted = w::unescape_posix(text, count, output);
    if (converted != w::Converted::ok) {
        STX_ASSERT(output.count() == 1 && output[0] == 'x', "a refused conversion leaves the output unchanged");
        return 0;
    }
    stxlib::Growable<char> again;
    if (!w::escape_posix(output.data() + 1, output.count() - 1, again)) return 0;
    STX_ASSERT(again.count() == count, "accepted text is canonical (length)");
    for (usize index = 0; index < count; ++index) {
        STX_ASSERT(again[index] == text[index], "accepted text is canonical (bytes)");
    }
    STX_VACUITY_PROBE();
    return 0;
}
