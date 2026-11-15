// Row 04: stx_auxlib::unicode::append_utf16 (B1.md section 3.1).
// P0: no undefined behaviour.
// P1: false leaves `output` unchanged; true appends exactly the UTF-16 of
//     the input after the units `output` already held.
// P2: the appended units equal the reference encoder (common/utf8_spec.h).
// Bound: input length at most N, bytes unconstrained; `output` starts with
//        0 to 2 symbolic units; unwind N + 1.
#include "symbolic_bytes.hpp"
#include "utf8_spec.h"
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

//@ lift source=stx_auxlib path=src/checked.hpp lines=27-31 namespace=stx_auxlib::checked sha256=9876341cc7ada133234c8923111088d3c725908dd9053937ea3f0b96aabcc56c
//@ lift source=stx_auxlib path=src/unicode.cpp lines=17-76 namespace=stx_auxlib::unicode anonymous sha256=8a953148cc79a4b3b2a96dc1ac8c28153bf3597fa62fb3d78e6f7919d96a3dcb
//@ lift source=stx_auxlib path=src/unicode.cpp lines=82-121 namespace=stx_auxlib::unicode sha256=f277b0693179d42b833a9e9ae65e30c47759adf65abf242a42dfcf789bae270b
//@ mutant id=m1 source=stx_auxlib path=src/unicode.cpp line=117 from="0x3ffU" to="0x1ffU"

int main() {
    const SymbolicBytes input(STX_N);
    std::u16string output{};
    const std::size_t prefix_length = nondet_size();
    STX_ASSUME(prefix_length <= 2);
    for (std::size_t index = 0; index < prefix_length; ++index) output.push_back(static_cast<char16_t>(nondet_ushort()));
    const std::u16string before = output;

    const bool accepted = stx_auxlib::unicode::append_utf16(input.view(), output);

    const std::unique_ptr<std::uint16_t[]> units(new std::uint16_t[input.size() + 1]);
    std::size_t count = 0;
    const bool valid = stx_spec_utf8_to_utf16(input.bytes(), input.size(), units.get(), &count);
    STX_ASSERT(accepted == valid, "P1: append_utf16 accepts exactly RFC 3629 UTF-8");
    if (!accepted) {
        STX_ASSERT(output == before, "P1: a refusal leaves output unchanged");
    } else {
        STX_ASSERT(output.size() == before.size() + count, "P2: one unit per reference unit");
        for (std::size_t index = 0; index < before.size(); ++index)
            STX_ASSERT(output[index] == before[index], "P1: the prior units are kept");
        for (std::size_t index = 0; index < count; ++index)
            STX_ASSERT(static_cast<std::uint16_t>(output[before.size() + index]) == units[index],
                       "P2: the appended units are the reference encoding");
    }
    STX_VACUITY_PROBE();
    return 0;
}
