// Row 17: hash::Blake2s::update and finish with compress, mix, load_little
// and reset (B1.md section 3.1).
// P0: no undefined behaviour (buffer indices, the byte counter).
// P2: the digest does not depend on how the input is split across update
//     calls: one update of the whole input equals up to three updates of
//     symbolic lengths. With STX_HAVE_RFC7693_REFERENCE, the digest also
//     equals the reference code of RFC 7693 appendix C, kept as
//     rfc7693_reference.h beside this file once the approved RFC text is on
//     the M4 (the reference file is not yet written; the define stays off
//     until it is).
// Bound: total input at most N bytes (the sweep, then the stated 192), in up
//        to three update calls; key length 0 or 1-32, symbolic contents.
//        Unwind max(N + 1, 33): the 32-byte key buffer, and the compress
//        loops (16, 8, 10 and 8 iterations).
#include "symbolic_bytes.hpp"
#include "verify.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "stx_auxlib/hash.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/hash.hpp sha256=1c11c55a13334b0dfd5b97f1ac14301765cad2e84b1d9bf12d3e6b21d8521a33
//@ lift source=stx_auxlib path=src/checked.hpp lines=16-22 sha256=d2a2b323f3710952a5fc8935b7ef2f7cdb806204c5050d14637f08f7f808f47a
//@ lift source=stx_auxlib path=src/hash.cpp lines=20-56 namespace=stx_auxlib::hash anonymous sha256=59d6ab1c6f3fa1d667d6d84f1be0ba67498fef14bb452aad194bf50d7ab69546
//@ lift source=stx_auxlib path=src/hash.cpp lines=60-172 namespace=stx_auxlib::hash sha256=05451a4a682f3a636c313ee5334fc939e1f61000957c04376ad43bbcd183f73e
//@ mutant id=m1 source=stx_auxlib path=src/hash.cpp line=103 from="word < 16" to="word < 17"

#if defined(STX_HAVE_RFC7693_REFERENCE)
#include "rfc7693_reference.h"
#endif

namespace hash = stx_auxlib::hash;

namespace {

std::span<const std::byte> bytes_of(const unsigned char* data, const std::size_t length) {
    const std::span<const std::byte> result(reinterpret_cast<const std::byte*>(data), length);
    return result;
}

} // namespace

int main() {
    const SymbolicBytes key(32);
    const SymbolicBytes input(STX_N);
    const std::size_t first_split = nondet_size();
    const std::size_t second_split = nondet_size();
    STX_ASSUME(first_split <= second_split && second_split <= input.size());

    hash::Digest whole{};
    hash::Digest split{};
    if (key.size() == 0) {
        hash::Blake2s one{};
        one.update(bytes_of(input.bytes(), input.size()));
        whole = one.finish();
        hash::Blake2s three{};
        three.update(bytes_of(input.bytes(), first_split));
        three.update(bytes_of(input.bytes() + first_split, second_split - first_split));
        three.update(bytes_of(input.bytes() + second_split, input.size() - second_split));
        split = three.finish();
    } else {
        hash::Blake2s one(bytes_of(key.bytes(), key.size()));
        one.update(bytes_of(input.bytes(), input.size()));
        whole = one.finish();
        hash::Blake2s three(bytes_of(key.bytes(), key.size()));
        three.update(bytes_of(input.bytes(), first_split));
        three.update(bytes_of(input.bytes() + first_split, second_split - first_split));
        three.update(bytes_of(input.bytes() + second_split, input.size() - second_split));
        split = three.finish();
    }
    STX_ASSERT(whole == split, "P2: the digest does not depend on the split");
#if defined(STX_HAVE_RFC7693_REFERENCE)
    std::array<unsigned char, 32> reference{};
    stx_rfc7693_blake2s(reference.data(), 32, key.bytes(), key.size(), input.bytes(), input.size());
    for (std::size_t index = 0; index < 32; ++index)
        STX_ASSERT(std::to_integer<unsigned char>(whole.bytes[index]) == reference[index], "P2: equals RFC 7693's reference");
#endif
    STX_VACUITY_PROBE();
    return 0;
}
