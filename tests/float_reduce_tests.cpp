// Tests of src/core/float_reduce.hpp, STXlib's floating-point reductions in
// the adopted fixed order (planning/stages/A1-float-discipline.md, section
// 3; F5 = 1).
//
// What each test establishes:
// - shape_by_hand: values chosen so that the stated shape, sequential
//   order, and a misplaced tail give different bits; the expected results
//   are worked out in the comments. These are what make the other tests
//   discriminating: a sequential or differently grouped sum fails them.
// - shape_against_reference: the shape restated as an index-mod-8
//   formulation, compared bit for bit for every length 0..300 over terms
//   whose order matters (mixed signs, exponents over 2^-40..2^40). Sampled
//   terms; every length in that range.
// - signed_zeros: the shape's stated start (no added zero).
// - chunks: the serial chunked forms against the chunk rule restated, at
//   the chunk boundaries.
// - pool: the pool forms give the serial chunked bits at 1, 2, 3 and 8
//   threads, for batches small enough to run on the calling thread and
//   large enough (601 chunks > 8 * 64) to run on the workers in
//   unspecified order. Sampled terms.
// - pool_refusals: a reduction on its own pool's worker is refused.

#include "core/float_reduce.hpp"

#include "core/checked.hpp"
#include "stxlib/thread_pool.hpp"
#include "test_support.hpp"

#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

namespace fr = stxlib::float_reduce;
using stxlib::usize;

struct Lcg final {
    std::uint64_t state{0};

    // A 64-bit linear congruential step: arithmetic modulo 2^64 by design.
    STXLIB_MODULAR std::uint64_t next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t result = state;
        return result;
    }
};

// A term whose sign and exponent (2^-40..2^40) vary, so the order of a sum
// changes its bits.
double mixed_term(Lcg& generator) {
    const std::uint64_t bits = generator.next();
    const std::uint64_t significand = bits >> 12;
    const int exponent = static_cast<int>((bits >> 4) % 81) - 40;
    double value = 1.0 + static_cast<double>(significand) * 0x1.0p-52;
    for (int step = 0; step < exponent; ++step) value = value * 2.0;
    for (int step = 0; step > exponent; --step) value = value * 0.5;
    const double result = (bits & 1) != 0 ? -value : value;
    return result;
}

std::vector<double> mixed_terms(const std::uint64_t seed, const usize count) {
    Lcg generator{seed};
    std::vector<double> result(count);
    for (usize index = 0; index < count; ++index) result[index] = mixed_term(generator);
    return result;
}

bool same_bits(const double left, const double right) {
    const bool result = std::bit_cast<std::uint64_t>(left) == std::bit_cast<std::uint64_t>(right);
    return result;
}

bool same_bits(const float left, const float right) {
    const bool result = std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
    return result;
}

// The shape restated: term i < blocked goes to accumulator i mod 8 (the
// first block starts the accumulators), the fixed tree, then the tail.
template <typename T>
T reference_sum(const T* const terms, const usize count) {
    const usize blocked = (count / 8) * 8;
    if (blocked == 0) {
        if (count == 0) return T(0);
        T sequential = terms[0];
        for (usize index = 1; index < count; ++index) sequential = sequential + terms[index];
        return sequential;
    }
    T accumulator[8]{};
    for (usize index = 0; index < blocked; ++index) {
        const usize lane = index % 8;
        accumulator[lane] = index < 8 ? terms[index] : accumulator[lane] + terms[index];
    }
    const T left_pair = accumulator[0] + accumulator[1];
    const T right_pair = accumulator[2] + accumulator[3];
    const T left_half = left_pair + right_pair;
    const T third_pair = accumulator[4] + accumulator[5];
    const T fourth_pair = accumulator[6] + accumulator[7];
    const T right_half = third_pair + fourth_pair;
    T result = left_half + right_half;
    for (usize index = blocked; index < count; ++index) result = result + terms[index];
    return result;
}

template <typename T>
T reference_dot(const T* const left, const T* const right, const usize count) {
    std::vector<T> products(count);
    for (usize index = 0; index < count; ++index) products[index] = left[index] * right[index];
    const T result = reference_sum(products.data(), count);
    return result;
}

// The chunk rule restated: chunk results in chunk-index order.
template <typename T>
T reference_chunked(const T* const left, const T* const right, const usize count) {
    T result = T(0);
    bool first = true;
    for (usize start = 0; start < count; start += 4096) {
        const usize length = count - start < 4096 ? count - start : 4096;
        const T partial = right == nullptr ? reference_sum(left + start, length)
                                           : reference_dot(left + start, right + start, length);
        result = first ? partial : result + partial;
        first = false;
    }
    return result;
}

void shape_by_hand() {
    const double big = 0x1.0p53;
    // Sequentially, 2^53 + 1 rounds to 2^53 (a tie, to even) every time:
    // the sum is 2^53. In the shape, lane 0 holds 2^53 and lanes 1..7 hold
    // 1: (2^53 + 1) + (1 + 1) = 2^53 + 2, (1 + 1) + (1 + 1) = 4, so the
    // result is 2^53 + 6, which is representable.
    const double block[8] = {big, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    CHECK(same_bits(fr::ordered_sum(block, 8), big + 6.0));
    double sequential = block[0];
    for (usize index = 1; index < 8; ++index) sequential = sequential + block[index];
    CHECK(same_bits(sequential, big));
    // A ninth term is tail: (2^53 + 6) + 1 = 2^53 + 7 is a tie between
    // 2^53 + 6 and 2^53 + 8, and rounds to the even significand, 2^53 + 8.
    // Had the tail joined lane 0, the result would stay 2^53 + 6.
    const double nine[9] = {big, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    CHECK(same_bits(fr::ordered_sum(nine, 9), big + 8.0));
    // Two blocks: lane 0 holds 2^53 + 1 = 2^53 (tie to even), lanes 1..7
    // hold 2 each: (2^53 + 2) + (2 + 2) = 2^53 + 6, (2 + 2) + (2 + 2) = 8,
    // so 2^53 + 14.
    const double sixteen[16] = {big, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    CHECK(same_bits(fr::ordered_sum(sixteen, 16), big + 14.0));
    // The tree's grouping: lanes {2^53, 1, 1, -2^53, 0, 0, 0, 0}. The tree
    // gives (2^53 + 1) + (1 - 2^53) = 2^53 + (-(2^53 - 1)) = 1 (the first
    // pair rounds to 2^53, a tie to even; the second is exact), where a
    // left-to-right sum of the lanes gives ((2^53 + 1) + 1) - 2^53 = 0.
    const double tree[8] = {big, 1.0, 1.0, -big, 0.0, 0.0, 0.0, 0.0};
    CHECK(same_bits(fr::ordered_sum(tree, 8), 1.0));
    double tree_sequential = tree[0];
    for (usize index = 1; index < 8; ++index) tree_sequential = tree_sequential + tree[index];
    CHECK(same_bits(tree_sequential, 0.0));
    // Fewer than 8 terms are summed in index order.
    const double short_terms[3] = {big, 1.0, 1.0};
    CHECK(same_bits(fr::ordered_sum(short_terms, 3), big));
    // The dot product: products 2^53, 1, ..., 1 as above.
    const double left[9] = {0x1.0p27, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    const double right[9] = {0x1.0p26, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    CHECK(same_bits(fr::ordered_dot(left, right, 8), big + 6.0));
    CHECK(same_bits(fr::ordered_dot(left, right, 9), big + 8.0));
    // Floats: 2^24 plays 2^53's part.
    const float float_big = 0x1.0p24f;
    const float float_block[9] = {float_big, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(same_bits(fr::ordered_sum(float_block, 8), float_big + 6.0f));
    CHECK(same_bits(fr::ordered_sum(float_block, 9), float_big + 8.0f));
}

void shape_against_reference() {
    for (usize count = 0; count <= 300; ++count) {
        const std::vector<double> left = mixed_terms(0x5eed0000 + count, count);
        const std::vector<double> right = mixed_terms(0xd07000 + count, count);
        CHECK(same_bits(fr::ordered_sum(left.data(), count), reference_sum(left.data(), count)));
        CHECK(same_bits(fr::ordered_dot(left.data(), right.data(), count),
                        reference_dot(left.data(), right.data(), count)));
        std::vector<float> narrow(count);
        for (usize index = 0; index < count; ++index) narrow[index] = static_cast<float>(left[index]);
        CHECK(same_bits(fr::ordered_sum(narrow.data(), count), reference_sum(narrow.data(), count)));
    }
    // The views agree with the pointer forms.
    const std::vector<double> terms = mixed_terms(77, 123);
    const stxlib::View<double> view(terms.data(), terms.size());
    CHECK(same_bits(fr::ordered_sum(view), fr::ordered_sum(terms.data(), terms.size())));
    CHECK(same_bits(fr::ordered_dot(view, view), fr::ordered_dot(terms.data(), terms.data(), terms.size())));
}

void signed_zeros() {
    const double negative_zeros[9] = {-0.0, -0.0, -0.0, -0.0, -0.0, -0.0, -0.0, -0.0, -0.0};
    CHECK(same_bits(fr::ordered_sum(negative_zeros, 0), 0.0));
    CHECK(same_bits(fr::ordered_sum(negative_zeros, 1), -0.0));
    CHECK(same_bits(fr::ordered_sum(negative_zeros, 7), -0.0));
    CHECK(same_bits(fr::ordered_sum(negative_zeros, 8), -0.0));
    CHECK(same_bits(fr::ordered_sum(negative_zeros, 9), -0.0));
    const double mixed[2] = {0.0, -0.0};
    CHECK(same_bits(fr::ordered_sum(mixed, 2), 0.0));
    const stxlib::View<double> none;
    CHECK(same_bits(fr::chunked_sum(none), 0.0));
}

void chunks() {
    const usize counts[] = {0, 1, 7, 8, 9, 4095, 4096, 4097, 8191, 8192, 8193, 3 * 4096 + 17};
    for (const usize count : counts) {
        const std::vector<double> left = mixed_terms(0xc4c4 + count, count);
        const std::vector<double> right = mixed_terms(0x9e9e + count, count);
        const stxlib::View<double> left_view(left.data(), count);
        const stxlib::View<double> right_view(right.data(), count);
        CHECK(fr::chunk_count(count) == (count + 4095) / 4096);
        CHECK(same_bits(fr::chunked_sum(left_view), reference_chunked<double>(left.data(), nullptr, count)));
        CHECK(same_bits(fr::chunked_dot(left_view, right_view),
                        reference_chunked<double>(left.data(), right.data(), count)));
        // A single chunk is the plain shape.
        if (count <= 4096) CHECK(same_bits(fr::chunked_sum(left_view), fr::ordered_sum(left_view)));
    }
}

stxlib::AtomicPool make_pool(const stxlib::u32 thread_count) {
    stxlib::OperationResult<stxlib::AtomicPool> created = stxlib::AtomicPool::create(thread_count);
    CHECK(created.succeeded());
    stxlib::AtomicPool result = std::move(created).take_value();
    return result;
}

void pool() {
    const usize large = 601 * 4096 + 123;
    const std::vector<double> left = mixed_terms(0xabcdef, large);
    const std::vector<double> right = mixed_terms(0xfedcba, large);
    std::vector<float> narrow(large);
    for (usize index = 0; index < large; ++index) narrow[index] = static_cast<float>(left[index]);
    const usize counts[] = {0, 1, 4095, 4096, 4097, 20 * 4096 + 5, large};
    const stxlib::u32 thread_counts[] = {1, 2, 3, 8};
    for (const stxlib::u32 thread_count : thread_counts) {
        stxlib::AtomicPool workers = make_pool(thread_count);
        for (const usize count : counts) {
            const stxlib::View<double> left_view(left.data(), count);
            const stxlib::View<double> right_view(right.data(), count);
            const stxlib::View<float> narrow_view(narrow.data(), count);
            const stxlib::OperationResult<double> sum = fr::pooled_sum(workers, left_view);
            const stxlib::OperationResult<double> dot = fr::pooled_dot(workers, left_view, right_view);
            const stxlib::OperationResult<float> narrow_sum = fr::pooled_sum(workers, narrow_view);
            CHECK(sum.succeeded() && dot.succeeded() && narrow_sum.succeeded());
            if (!sum.succeeded() || !dot.succeeded() || !narrow_sum.succeeded()) continue;
            CHECK(same_bits(*sum.value(), fr::chunked_sum(left_view)));
            CHECK(same_bits(*dot.value(), fr::chunked_dot(left_view, right_view)));
            CHECK(same_bits(*narrow_sum.value(), fr::chunked_sum(narrow_view)));
        }
        // And the serial chunked form against the restated rule, once, at
        // the size that runs on the workers.
        if (thread_count == 1) {
            const stxlib::View<double> left_view(left.data(), large);
            CHECK(same_bits(fr::chunked_sum(left_view), reference_chunked<double>(left.data(), nullptr, large)));
        }
    }
}

struct WorkerCall final {
    stxlib::AtomicPool* pool{nullptr};
    const double* terms{nullptr};
    bool refused{false};
    stxlib::ErrorKind kind{stxlib::ErrorKind::invalid_argument};
};

void reduce_on_worker(void* const argument) noexcept {
    WorkerCall& call = *static_cast<WorkerCall*>(argument);
    const stxlib::View<double> view(call.terms, 16);
    const stxlib::OperationResult<double> attempt = fr::pooled_sum(*call.pool, view);
    call.refused = !attempt.succeeded();
    if (call.refused) call.kind = (*attempt.error()).kind();
}

void pool_refusals() {
    stxlib::AtomicPool workers = make_pool(2);
    const std::vector<double> terms = mixed_terms(5, 16);
    WorkerCall call{&workers, terms.data(), false, stxlib::ErrorKind::invalid_argument};
    const stxlib::PoolTask task{&reduce_on_worker, &call};
    const stxlib::PoolTask tasks[1] = {task};
    CHECK(workers.begin(stxlib::View<stxlib::PoolTask>(tasks)).succeeded());
    CHECK(workers.wait().succeeded());
    CHECK(call.refused);
    CHECK(call.kind == stxlib::ErrorKind::invalid_argument);
    // An empty pool refuses with the pool's own record.
    stxlib::AtomicPool empty;
    const stxlib::View<double> view(terms.data(), terms.size());
    const stxlib::OperationResult<double> refused = fr::pooled_sum(empty, view);
    CHECK(!refused.succeeded());
}

} // namespace

int main() {
    shape_by_hand();
    shape_against_reference();
    signed_zeros();
    chunks();
    pool();
    pool_refusals();
    return stxlib_tests::finish("stxlib float_reduce");
}
