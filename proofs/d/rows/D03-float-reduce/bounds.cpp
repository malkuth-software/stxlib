// Row D03 (phase D, targeted): the reductions of src/core/float_reduce.hpp
// in double, for the solver.
// P0: ordered_sum, ordered_dot, chunked_sum and chunked_dot read only
//     terms[0, count) (the terms sit at the end of their storage, so a read
//     at [count] is outside the object), with no overflow in their index
//     arithmetic; chunk_count is ceil(count / chunk_terms).
// P1 (signed zero, F5's "no added zero"): over count negative zeros the sum
//     is -0 for count >= 1 and +0 for none, bit for bit.
// Environment model: chunk_terms is scaled from 4096 to 16 (the chunk loops
//     are parametric in it; 16 lets a bound of 40 terms cross two chunk
//     boundaries). The shape at 4096 is shape.cpp's enumeration.
// Bound: count at most N (N in {17, 40}).
#include "verify.h"

#include "stxlib/types.hpp"

#include <bit>
#include <cstdint>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=7d1b60e63a2dc6a28180f30d1941df8b628ea53c676cb251c65495c2a02f4b58

//@ model-begin reason="View and STXLIB_REQUIRE reduced to what the lifted code uses"
#define STXLIB_REQUIRE(condition, message) STX_ASSERT(condition, message)
namespace stxlib {
template <typename T>
class View final {
public:
    View(const T* const data, const usize count) noexcept : data_(data), count_(count) {}
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    const T* data_;
    usize count_;
};
} // namespace stxlib
//@ model-end

//@ lift source=stxlib path=src/core/float_reduce.hpp lines=60-223 namespace=stxlib::float_reduce sha256=f6df01e5adb86ae6f2cdbf233afefb0f29e05075051fa73349799d249729a7c1
//@ subst source=stxlib path=src/core/float_reduce.hpp line=61 from="4096" to="16" reason="chunk_terms scaled for the unwinding bound; shape.cpp covers 4096"
//@ mutant id=m1 source=stxlib path=src/core/float_reduce.hpp line=75 from="count - count % lanes" to="count - count % lanes + lanes"

#ifndef STX_N
#define STX_N 17
#endif

using stxlib::usize;
namespace fr = stxlib::float_reduce;

int main() {
    constexpr usize bound = STX_N;
    double left_storage[bound];
    double right_storage[bound];
    for (usize index = 0; index < bound; ++index) {
        left_storage[index] = nondet_double();
        right_storage[index] = nondet_double();
    }
    const usize count = nondet_size();
    STX_ASSUME(count <= bound);
    const double* const left = left_storage + (bound - count);
    const double* const right = right_storage + (bound - count);
    const double sum = fr::ordered_sum(left, count);
    const double dot = fr::ordered_dot(left, right, count);
    const double chunked = fr::chunked_sum(stxlib::View<double>(left, count));
    const double chunked_dot = fr::chunked_dot(stxlib::View<double>(left, count), stxlib::View<double>(right, count));
    (void)sum;
    (void)dot;
    (void)chunked;
    (void)chunked_dot;
    STX_ASSERT(fr::chunk_count(count) == (count + fr::chunk_terms - 1) / fr::chunk_terms, "chunk_count is the ceiling");

    double zeros_storage[bound];
    for (usize index = 0; index < bound; ++index) zeros_storage[index] = -0.0;
    const double* const zeros = zeros_storage + (bound - count);
    const std::uint64_t bits = std::bit_cast<std::uint64_t>(fr::ordered_sum(zeros, count));
    const std::uint64_t chunked_bits = std::bit_cast<std::uint64_t>(fr::chunked_sum(stxlib::View<double>(zeros, count)));
    const std::uint64_t negative_zero = 0x8000000000000000ULL;
    STX_ASSERT(bits == (count == 0 ? 0 : negative_zero), "a sum of negative zeros is -0, and +0 for none");
    STX_ASSERT(chunked_bits == (count == 0 ? 0 : negative_zero), "the chunked sum keeps the sign of zero");
    STX_VACUITY_PROBE();
    return 0;
}
