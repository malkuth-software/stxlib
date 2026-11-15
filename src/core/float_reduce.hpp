#pragma once

// STXlib's floating-point reductions in the adopted fixed order
// (planning/stages/A1-float-discipline.md, section 3; owner decision F5 = 1,
// 2026-11-13, notes decisions/2026-11-13-locals-float-mutability.md).
// Private to STXlib's sources. Every reduction STXlib writes over more than
// a handful of terms uses these shapes, or an exact accumulator
// (float_exact.hpp), and names which in its contract. No STXlib source
// reduced floating-point values before this header (survey at 544a244);
// these are the shapes the first ones take.
//
// The shape (F5 = 1), for n terms x[0..n):
// - Lanes. While a full block of 8 terms remains, term i goes to
//   accumulator i mod 8. The accumulators start as the first block's terms
//   (no added zero, so a sum of negative zeros stays negative), and each
//   later block adds its terms lane by lane: acc[l] = acc[l] + x[8k + l].
// - Combine. The 8 accumulators combine by the fixed tree
//   ((acc0 + acc1) + (acc2 + acc3)) + ((acc4 + acc5) + (acc6 + acc7)).
// - Tail. The n mod 8 remaining terms are added to the combined value one
//   at a time in index order. With no full block (n < 8) the result is
//   x[0] + x[1] + ... in index order, x[0] alone for one term, and +0 for
//   none.
// Every operation rounds once to nearest-even in the term's own type
// (rules 1, 3 and 5: no contraction, no reassociation, no wider
// intermediates). A vector implementation computes exactly this: AVX2
// holds 8 floats or two registers of 4 doubles, SSE2 and NEON two or four
// registers, and each register lane is one accumulator. The scalar form
// below is the definition; a vector form must give its bits.
//
// A dot product is the same shape over the products x[i] * y[i], each
// rounded on its own (a product is never fused with the addition that
// follows it).
//
// Chunks. A reduction that is split for the thread pool partitions its
// terms into fixed chunks of chunk_terms (4096) terms, the last chunk
// shorter, never by thread count. Each chunk reduces in the shape above,
// and the chunk results combine one at a time in chunk-index order:
// total = r0, then total = total + r1, and so on (a single chunk is its
// own result; no chunk gives +0). The serial chunked forms and the pool
// forms compute the same values in the same order, so the thread count
// changes the speed, never the bits.
//
// Subnormals. These are internal arithmetic: an intermediate or a result
// may be subnormal (IEEE gradual underflow, deterministic because STXlib
// never sets a flush mode). A caller that returns the result through a
// public interface applies the subnormal rule there
// (core/subnormal.hpp, stxlib::detail::flush_subnormal).

#include "core/float_discipline.hpp"
#include "stxlib/buffer.hpp"
#include "stxlib/error.hpp"
#include "stxlib/require.hpp"
#include "stxlib/result.hpp"
#include "stxlib/thread_pool.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::float_reduce {

inline constexpr usize lanes = 8;
inline constexpr usize chunk_terms = 4096;

// The fixed combining tree.
template <typename T>
[[nodiscard]] inline T combine_lanes(const T (&accumulator)[lanes]) noexcept {
    const T low = (accumulator[0] + accumulator[1]) + (accumulator[2] + accumulator[3]);
    const T high = (accumulator[4] + accumulator[5]) + (accumulator[6] + accumulator[7]);
    const T result = low + high;
    return result;
}

// The shape over terms[0..count).
template <typename T>
[[nodiscard]] inline T ordered_sum(const T* const terms, const usize count) noexcept {
    const usize blocked = count - count % lanes;
    usize index = 0;
    T result = T(0);
    if (blocked > 0) {
        T accumulator[lanes]{};
        for (usize lane = 0; lane < lanes; ++lane) accumulator[lane] = terms[lane];
        for (index = lanes; index < blocked; index += lanes) {
            for (usize lane = 0; lane < lanes; ++lane) accumulator[lane] = accumulator[lane] + terms[index + lane];
        }
        result = combine_lanes(accumulator);
    } else if (count > 0) {
        result = terms[0];
        index = 1;
    }
    for (; index < count; ++index) result = result + terms[index];
    return result;
}

template <typename T>
[[nodiscard]] inline T ordered_sum(const View<T> terms) noexcept {
    const T result = ordered_sum(terms.data(), terms.count());
    return result;
}

// The shape over the products left[i] * right[i], i in [0, count).
template <typename T>
[[nodiscard]] inline T ordered_dot(const T* const left, const T* const right, const usize count) noexcept {
    const usize blocked = count - count % lanes;
    usize index = 0;
    T result = T(0);
    if (blocked > 0) {
        T accumulator[lanes]{};
        for (usize lane = 0; lane < lanes; ++lane) accumulator[lane] = left[lane] * right[lane];
        for (index = lanes; index < blocked; index += lanes) {
            for (usize lane = 0; lane < lanes; ++lane) {
                const T product = left[index + lane] * right[index + lane];
                accumulator[lane] = accumulator[lane] + product;
            }
        }
        result = combine_lanes(accumulator);
    } else if (count > 0) {
        result = left[0] * right[0];
        index = 1;
    }
    for (; index < count; ++index) {
        const T product = left[index] * right[index];
        result = result + product;
    }
    return result;
}

// Requirement: left.count() == right.count().
template <typename T>
[[nodiscard]] inline T ordered_dot(const View<T> left, const View<T> right) noexcept {
    STXLIB_REQUIRE(left.count() == right.count(), "float_reduce::ordered_dot: lengths differ");
    const T result = ordered_dot(left.data(), right.data(), left.count());
    return result;
}

// The number of chunks `count` terms make.
[[nodiscard]] inline usize chunk_count(const usize count) noexcept {
    const usize result = count / chunk_terms + (count % chunk_terms != 0 ? 1 : 0);
    return result;
}

// One chunk's reduction: a sum when `right` is null, a dot product
// otherwise. `result` is written by the reduction.
template <typename T>
struct Chunk final {
    const T* left{nullptr};
    const T* right{nullptr};
    usize count{0};
    T result{T(0)};
};

template <typename T>
inline void reduce_chunk(Chunk<T>& chunk) noexcept {
    if (chunk.right == nullptr) {
        chunk.result = ordered_sum(chunk.left, chunk.count);
    } else {
        chunk.result = ordered_dot(chunk.left, chunk.right, chunk.count);
    }
}

// The pool's entry for one chunk (a PoolTask function).
template <typename T>
void reduce_chunk_task(void* const argument) noexcept {
    Chunk<T>& chunk = *static_cast<Chunk<T>*>(argument);
    reduce_chunk(chunk);
}

// The chunk results combined in chunk-index order.
template <typename T>
[[nodiscard]] inline T combine_chunks(const Chunk<T>* const chunks, const usize count) noexcept {
    if (count == 0) return T(0);
    T result = chunks[0].result;
    for (usize index = 1; index < count; ++index) result = result + chunks[index].result;
    return result;
}

// Describes the chunks of terms[0..count) (and right[0..count) for a dot
// product) into chunks[0..chunk_count(count)).
template <typename T>
inline void describe_chunks(const T* const left, const T* const right, const usize count, Chunk<T>* const chunks) noexcept {
    const usize chunks_needed = chunk_count(count);
    for (usize index = 0; index < chunks_needed; ++index) {
        const usize first = index * chunk_terms;
        const usize remaining = count - first;
        Chunk<T>& chunk = chunks[index];
        chunk.left = left + first;
        chunk.right = right == nullptr ? nullptr : right + first;
        chunk.count = remaining < chunk_terms ? remaining : chunk_terms;
        chunk.result = T(0);
    }
}

// The serial chunked forms: chunk by chunk on the calling thread, the same
// values as the pool forms below.
template <typename T>
[[nodiscard]] inline T chunked_sum(const View<T> terms) noexcept {
    const usize count = terms.count();
    const usize chunks_needed = chunk_count(count);
    T result = T(0);
    for (usize index = 0; index < chunks_needed; ++index) {
        const usize first = index * chunk_terms;
        const usize remaining = count - first;
        const usize length = remaining < chunk_terms ? remaining : chunk_terms;
        const T partial = ordered_sum(terms.data() + first, length);
        result = index == 0 ? partial : result + partial;
    }
    return result;
}

// Requirement: left.count() == right.count().
template <typename T>
[[nodiscard]] inline T chunked_dot(const View<T> left, const View<T> right) noexcept {
    STXLIB_REQUIRE(left.count() == right.count(), "float_reduce::chunked_dot: lengths differ");
    const usize count = left.count();
    const usize chunks_needed = chunk_count(count);
    T result = T(0);
    for (usize index = 0; index < chunks_needed; ++index) {
        const usize first = index * chunk_terms;
        const usize remaining = count - first;
        const usize length = remaining < chunk_terms ? remaining : chunk_terms;
        const T partial = ordered_dot(left.data() + first, right.data() + first, length);
        result = index == 0 ? partial : result + partial;
    }
    return result;
}

// The pool forms: one task per chunk on `pool`, then the chunk results
// combined in chunk-index order on the calling thread. Identical bits to
// chunked_sum and chunked_dot for every thread count. The chunk records and
// tasks are blocks of STXlib's allocator, released before return.
//
// Outcomes: the value; out_of_memory or limit_exceeded when the chunk
// records cannot be allocated; the pool's refusal (an empty pool, or more
// than 2^32 - 1 chunks); invalid_argument when called on one of `pool`'s
// own workers (a task must not call into its own pool). Nothing is reduced
// on a failure. Requirement (dot): left.count() == right.count().
template <typename T>
[[nodiscard]] Result<T> pooled_reduce(AtomicPool& pool, const T* const left, const T* const right,
                                      const usize count) noexcept {
    if (pool.is_worker()) {
        const ErrorRecord refused = ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.float_reduce.pooled",
            ErrorDetail{.message = "a reduction must not run on a worker of its own pool"});
        return Result<T>::failure(refused);
    }
    const usize chunks_needed = chunk_count(count);
    if (chunks_needed == 0) return Result<T>::success(T(0));
    Result<Buffer<Chunk<T>>> chunk_storage = Buffer<Chunk<T>>::allocate_zeroed(chunks_needed);
    if (chunk_storage.failed()) return Result<T>::failure(*chunk_storage.error());
    Result<Buffer<PoolTask>> task_storage = Buffer<PoolTask>::allocate_zeroed(chunks_needed);
    if (task_storage.failed()) return Result<T>::failure(*task_storage.error());
    Buffer<Chunk<T>>& chunks = *chunk_storage.value();
    Buffer<PoolTask>& tasks = *task_storage.value();
    describe_chunks(left, right, count, &chunks[0]);
    for (usize index = 0; index < chunks_needed; ++index) {
        tasks[index].function = &reduce_chunk_task<T>;
        tasks[index].argument = &chunks[index];
    }
    const Status ran = pool.run(tasks.view());
    if (!ran.succeeded()) return Result<T>::failure(*ran.error());
    const T result = combine_chunks(&chunks[0], chunks_needed);
    return Result<T>::success(result);
}

template <typename T>
[[nodiscard]] Result<T> pooled_sum(AtomicPool& pool, const View<T> terms) noexcept {
    Result<T> result = pooled_reduce<T>(pool, terms.data(), nullptr, terms.count());
    return result;
}

template <typename T>
[[nodiscard]] Result<T> pooled_dot(AtomicPool& pool, const View<T> left, const View<T> right) noexcept {
    STXLIB_REQUIRE(left.count() == right.count(), "float_reduce::pooled_dot: lengths differ");
    Result<T> result = pooled_reduce<T>(pool, left.data(), right.data(), left.count());
    return result;
}

} // namespace stxlib::float_reduce
