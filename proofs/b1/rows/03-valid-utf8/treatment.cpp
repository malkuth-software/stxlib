// Row 03: stx_auxlib::unicode::valid_utf8 with decode and is_continuation
// (B1.md section 3.1).
// P0: no undefined behaviour, the 8-byte memcpy word read included (line
//     88); the input buffer has exactly the input's length, so any read past
//     it is a memory error.
// P2: true exactly when the bytes are RFC 3629 UTF-8 (common/utf8_spec.h,
//     written from RFC 3629 section 4).
// Bound: length at most N, bytes unconstrained; unwind N + 1.
#include "symbolic_bytes.hpp"
#include "utf8_spec.h"
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

//@ lift source=stx_auxlib path=src/unicode.cpp lines=17-76 namespace=stx_auxlib::unicode anonymous sha256=8a953148cc79a4b3b2a96dc1ac8c28153bf3597fa62fb3d78e6f7919d96a3dcb
//@ lift source=stx_auxlib path=src/unicode.cpp lines=82-103 namespace=stx_auxlib::unicode sha256=6c95c9ba894504f76ccde3be4bc5e47a82de605235cfe20a256fbb61f23e3a87
//@ mutant id=m1 source=stx_auxlib path=src/unicode.cpp line=67 from="available <= trailing" to="available < trailing"

int main() {
    const SymbolicBytes input(STX_N);
    const bool actual = stx_auxlib::unicode::valid_utf8(input.view());
    const bool expected = stx_spec_valid_utf8(input.bytes(), input.size());
    STX_ASSERT(actual == expected, "P2: valid_utf8 is RFC 3629 validity");
    STX_VACUITY_PROBE();
    return 0;
}
