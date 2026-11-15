// Row 18: hash::secure_erase (B1.md section 3.1).
// P1: every byte is zero on return. Source semantics only: that the
//     volatile stores survive optimisation is a property of the binary
//     (B2, T3), not of this run.
// Bound: length at most N, N in {4, 16, 64}; unwind N + 1.
#include "symbolic_bytes.hpp"
#include "verify.h"

#include <cstddef>
#include <memory>
#include <span>

#include "stx_auxlib/hash.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/hash.hpp sha256=1c11c55a13334b0dfd5b97f1ac14301765cad2e84b1d9bf12d3e6b21d8521a33
//@ lift source=stx_auxlib path=src/hash.cpp lines=169-172 namespace=stx_auxlib::hash sha256=979769b9ae95697c0b17588037ef3f0b45087240b2593d75a1b208699432b9d9
//@ mutant id=m1 source=stx_auxlib path=src/hash.cpp line=171 from="index < bytes.size()" to="index + 1 < bytes.size()"

int main() {
    const std::size_t length = nondet_size();
    STX_ASSUME(length <= STX_N);
    const std::unique_ptr<std::byte[]> buffer(new std::byte[length]);
    for (std::size_t index = 0; index < length; ++index) buffer[index] = static_cast<std::byte>(nondet_uchar());
    stx_auxlib::hash::secure_erase(std::span<std::byte>(buffer.get(), length));
    for (std::size_t index = 0; index < length; ++index)
        STX_ASSERT(buffer[index] == std::byte{0}, "P1: every byte is zero");
    STX_VACUITY_PROBE();
    return 0;
}
