// Row 09: stx_auxlib::text::trim with is_space (B1.md section 3.1).
// P0: no undefined behaviour.
// P2: the result is the longest substring of the input with no leading or
//     trailing space byte (space, \t, \n, \r, \v, \f; text.hpp), returned
//     as a view into the input.
// Bound: length at most N, bytes unconstrained; unwind N + 1.
#include "symbolic_bytes.hpp"
#include "verify.h"

#include <cstddef>
#include <string_view>

#include "stx_auxlib/text.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/text.hpp sha256=5356cee096afb9642d733039274c328b0baac0da13af89a77836f66a84801bc8
//@ lift source=stx_auxlib path=src/text.cpp lines=55-59 namespace=stx_auxlib::text anonymous sha256=4ac7e7f6c660f9e4be8dc2fe6ddf281c11e7d065a3d9cca249d7e978dfac48f4
//@ lift source=stx_auxlib path=src/text.cpp lines=291-298 namespace=stx_auxlib::text sha256=69d2f0ada7277c2682750bda26d303bad8a83d93615703b42bc81dc6784c8922
//@ mutant id=m1 source=stx_auxlib path=src/text.cpp line=294 from="is_space(text[first])" to="is_space(text[first + 1])"

namespace {

bool spec_space(const unsigned char byte) {
    const bool result = byte == 0x20 || byte == 0x09 || byte == 0x0a || byte == 0x0d || byte == 0x0b || byte == 0x0c;
    return result;
}

} // namespace

int main() {
    const SymbolicBytes input(STX_N);
    const std::string_view text = input.view();
    std::size_t first = 0;
    while (first < input.size() && spec_space(input.bytes()[first])) ++first;
    std::size_t last = first;
    for (std::size_t index = first; index < input.size(); ++index) {
        if (!spec_space(input.bytes()[index])) last = index + 1;
    }
    const std::string_view result = stx_auxlib::text::trim(text);
    STX_ASSERT(result.data() == text.data() + first, "P2: trim starts at the first non-space byte");
    STX_ASSERT(result.size() == last - first, "P2: trim ends after the last non-space byte");
    STX_VACUITY_PROBE();
    return 0;
}
