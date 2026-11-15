// Row 05: stx_auxlib::unicode::append_utf8 (B1.md section 3.1).
// P0: no undefined behaviour.
// P1: an unpaired surrogate gives false with `output` unchanged (the body
//     builds into a local first, line 124); true appends the UTF-8 of the
//     input after the bytes `output` already held.
// P2: the appended bytes equal the reference encoder (common/utf8_spec.h).
// Bound: at most N / 2 UTF-16 units, units unconstrained; `output` starts
//        with 0 to 2 symbolic bytes; unwind N + 1.
#include "utf8_spec.h"
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

//@ lift source=stx_auxlib path=src/checked.hpp lines=34-40 namespace=stx_auxlib::checked sha256=6b7ff4bed5f86774c7ea6d33bac14b9d15d4bf27543b6ed0926b8209683d1da3
//@ lift source=stx_auxlib path=src/unicode.cpp lines=123-157 namespace=stx_auxlib::unicode sha256=91b6c6072667c415ce4d9c11487455727c81edb076cb1b94d85bd54a91aa4258
//@ mutant id=m1 source=stx_auxlib path=src/unicode.cpp line=133 from="low > 0xdfffU" to="low > 0xdffeU"

int main() {
    const std::size_t count = nondet_size();
    STX_ASSUME(count <= STX_N / 2);
    const std::unique_ptr<char16_t[]> units(new char16_t[count]);
    const std::unique_ptr<std::uint16_t[]> plain(new std::uint16_t[count]);
#if !defined(STX_UNBOUNDED)
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint16_t unit = nondet_ushort();
        units[index] = static_cast<char16_t>(unit);
        plain[index] = unit;
    }
#else
    for (std::size_t index = 0; index < count; ++index) plain[index] = static_cast<std::uint16_t>(units[index]);
#endif
    std::string output{};
    const std::size_t prefix_length = nondet_size();
    STX_ASSUME(prefix_length <= 2);
    for (std::size_t index = 0; index < prefix_length; ++index) output.push_back(static_cast<char>(nondet_uchar()));
    const std::string before = output;

    const bool accepted = stx_auxlib::unicode::append_utf8(std::u16string_view(units.get(), count), output);

    const std::unique_ptr<unsigned char[]> bytes(new unsigned char[3 * count + 1]);
    std::size_t length = 0;
    const bool valid = stx_spec_utf16_to_utf8(plain.get(), count, bytes.get(), &length);
    STX_ASSERT(accepted == valid, "P1: append_utf8 refuses exactly the unpaired surrogates");
    if (!accepted) {
        STX_ASSERT(output == before, "P1: a refusal leaves output unchanged");
    } else {
        STX_ASSERT(output.size() == before.size() + length, "P2: one byte per reference byte");
        for (std::size_t index = 0; index < before.size(); ++index)
            STX_ASSERT(output[index] == before[index], "P1: the prior bytes are kept");
        for (std::size_t index = 0; index < length; ++index)
            STX_ASSERT(static_cast<unsigned char>(output[before.size() + index]) == bytes[index],
                       "P2: the appended bytes are the reference encoding");
    }
    STX_VACUITY_PROBE();
    return 0;
}
