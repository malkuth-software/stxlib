#pragma once

// A deterministic binary heap and priority queue (roadmap M2, R6; audit B:
// make_heap, push_heap, pop_heap and priority_queue specify only the heap
// property, not the layout, so which of two equal priorities comes out
// first depends on the library and its release; that decides replay in
// schedulers, timer queues, event ordering and path search).
//
// Every entry carries an insertion sequence, and ties in the caller's order
// are broken by it, earlier first. Given distinct sequences the combined
// order is a strict total order, so the order of removal is the sorted
// order of the entries, which is unique: it is a function of the operations
// alone, never of the layout, the library or the platform.
//
// The order is a type with a static member
//     static bool before(const Value& a, const Value& b) noexcept;
// true when a comes out before b. It must be a strict weak order (for
// floating-point values that means an order that places NaN, as math.hpp's
// total order does; SmallestFirst and LargestFirst refuse floating types).
//
// Two forms:
// - stable_heap_push and stable_heap_pop over a caller's storage (a
//   MutableView of HeapEntry), as push_heap and pop_heap are used; the
//   caller assigns the sequences;
// - PriorityQueue, which owns its entries in a Vector (allocation through
//   STXlib, exhaustion a named result) and assigns the sequences.
//
// Values are moved, never copied: a Value needs a nothrow move constructor,
// a nothrow move assignment and a nothrow destructor. tests/heap_tests.cpp
// compares both forms with a sorted model over every sequence of
// operations up to a bound (proofs/REPORT.md).

// Early exits (owner, 2026-11-15, decisions/2026-11-15-early-exit-
// integrity.md): Order::before and Value's moves and destructor are the
// consumer's code. Order::before must be noexcept (checked), so an unwind
// out of it ends the process. Form 2 at every comparison and move in
// stable_heap_push and stable_heap_pop: every slot of the caller's storage
// holds a live entry at every call (moved-from entries are live), so the
// storage is always safe to destroy; an early exit can lose only the entry
// held in a local, and leaves the heap order unrestored. PriorityQueue adds
// form 1 around them: Vector's own calls (vector.hpp) and the count changes
// only through Vector.

#include "stxlib/detail/construct.hpp"
#include "stxlib/optional.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

template <typename Value>
struct HeapEntry final {
    Value value;
    u64 sequence;
};

namespace detail {

template <typename Value>
struct IsFloating final {
    static constexpr bool value = false;
};
template <>
struct IsFloating<float> final {
    static constexpr bool value = true;
};
template <>
struct IsFloating<double> final {
    static constexpr bool value = true;
};
template <>
struct IsFloating<long double> final {
    static constexpr bool value = true;
};

template <typename Value>
inline constexpr bool heap_movable = __is_nothrow_constructible(Value, Value&&) &&
                                     __is_nothrow_assignable(Value&, Value&&) && nothrow_destructible<Value>;

// The combined order: the caller's, then the sequence.
template <typename Order, typename Value>
[[nodiscard]] bool heap_before(const HeapEntry<Value>& a, const HeapEntry<Value>& b) noexcept {
    static_assert(noexcept(Order::before(a.value, b.value)), "a heap Order's before must be noexcept");
    if (Order::before(a.value, b.value)) return true;
    if (Order::before(b.value, a.value)) return false;
    const bool result = a.sequence < b.sequence;
    return result;
}

} // namespace detail

// Smaller values first, for types with a strict weak operator<.
template <typename Value>
struct SmallestFirst final {
    static_assert(!detail::IsFloating<Value>::value, "SmallestFirst: a floating-point order must place NaN");
    [[nodiscard]] static bool before(const Value& a, const Value& b) noexcept {
        const bool result = a < b;
        return result;
    }
};

// Larger values first, for types with a strict weak operator<.
template <typename Value>
struct LargestFirst final {
    static_assert(!detail::IsFloating<Value>::value, "LargestFirst: a floating-point order must place NaN");
    [[nodiscard]] static bool before(const Value& a, const Value& b) noexcept {
        const bool result = b < a;
        return result;
    }
};

// Requirement: heap[0, count - 1) is a heap under Order and the sequences;
// heap[count - 1] is the new entry. Afterwards heap[0, count) is a heap.
template <typename Order, typename Value>
void stable_heap_push(const MutableView<HeapEntry<Value>> heap) noexcept {
    static_assert(detail::heap_movable<Value>, "a heap Value needs nothrow move construction, assignment and destruction");
    const usize count = heap.count();
    if (count < 2) return;
    HeapEntry<Value>* const entries = heap.data();
    usize index = count - 1;
    HeapEntry<Value> moving = static_cast<HeapEntry<Value>&&>(entries[index]);
    while (index > 0) {
        const usize parent = (index - 1) / 2;
        if (!detail::heap_before<Order>(moving, entries[parent])) break;
        entries[index] = static_cast<HeapEntry<Value>&&>(entries[parent]);
        index = parent;
    }
    entries[index] = static_cast<HeapEntry<Value>&&>(moving);
}

// Requirement: heap[0, count) is a heap. Afterwards the first entry in the
// combined order is at heap[count - 1] and heap[0, count - 1) is a heap.
template <typename Order, typename Value>
void stable_heap_pop(const MutableView<HeapEntry<Value>> heap) noexcept {
    static_assert(detail::heap_movable<Value>, "a heap Value needs nothrow move construction, assignment and destruction");
    const usize count = heap.count();
    if (count < 2) return;
    HeapEntry<Value>* const entries = heap.data();
    const usize limit = count - 1;
    HeapEntry<Value> top = static_cast<HeapEntry<Value>&&>(entries[0]);
    HeapEntry<Value> moving = static_cast<HeapEntry<Value>&&>(entries[limit]);
    usize index = 0;
    // An entry is at least 8 bytes, so 2 * index + 2 does not overflow.
    while (true) {
        usize child = 2 * index + 1;
        if (child >= limit) break;
        if (child + 1 < limit && detail::heap_before<Order>(entries[child + 1], entries[child])) child = child + 1;
        if (!detail::heap_before<Order>(entries[child], moving)) break;
        entries[index] = static_cast<HeapEntry<Value>&&>(entries[child]);
        index = child;
    }
    entries[index] = static_cast<HeapEntry<Value>&&>(moving);
    entries[limit] = static_cast<HeapEntry<Value>&&>(top);
}

// A priority queue that removes values in Order, ties in insertion order.
// Layout: the Vector of entries, then the next sequence. The sequence
// restarts at zero whenever the queue becomes empty (ties are only ever
// between entries present together), so it is exhausted only after 2^64 - 1
// insertions without the queue ever emptying, which push reports as
// limit_exceeded.
template <typename Value, typename Order>
class PriorityQueue final {
    static_assert(detail::heap_movable<Value>, "a PriorityQueue Value needs nothrow move construction, assignment and destruction");

public:
    PriorityQueue() noexcept = default;
    PriorityQueue(const PriorityQueue&) = delete;
    PriorityQueue& operator=(const PriorityQueue&) = delete;
    PriorityQueue(PriorityQueue&& other) noexcept
        : entries_(static_cast<Vector<HeapEntry<Value>>&&>(other.entries_)), next_sequence_(other.next_sequence_) {
        other.next_sequence_ = 0;
    }
    PriorityQueue& operator=(PriorityQueue&& other) noexcept {
        if (this == &other) return *this;
        entries_ = static_cast<Vector<HeapEntry<Value>>&&>(other.entries_);
        next_sequence_ = other.next_sequence_;
        other.next_sequence_ = 0;
        return *this;
    }
    ~PriorityQueue() = default;

    [[nodiscard]] static Result<PriorityQueue> with_capacity(const usize room) noexcept {
        PriorityQueue result;
        const Status reserved = result.entries_.reserve(room);
        if (reserved.failed()) return Result<PriorityQueue>::failure(*reserved.error());
        return Result<PriorityQueue>::success(static_cast<PriorityQueue&&>(result));
    }

    [[nodiscard]] usize count() const noexcept { return entries_.count(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] usize capacity() const noexcept { return entries_.capacity(); }

    [[nodiscard]] Status reserve(const usize wanted) noexcept {
        const Status result = entries_.reserve(wanted);
        return result;
    }

    // Adds a value. On failure (out_of_memory, limit_exceeded) the queue and
    // `value` are unchanged.
    [[nodiscard]] Status push(Value&& value) noexcept {
        if (next_sequence_ == u64_max) {
            const ErrorRecord exhausted = ErrorRecord::make(
                ErrorKind::limit_exceeded, "stxlib.priority_queue.push",
                ErrorDetail{.message = "2^64 - 1 insertions without the queue emptying"});
            return Status::failure(exhausted);
        }
        HeapEntry<Value> entry{static_cast<Value&&>(value), next_sequence_};
        const Status pushed = entries_.push(static_cast<HeapEntry<Value>&&>(entry));
        if (pushed.failed()) {
            value = static_cast<Value&&>(entry.value);
            return pushed;
        }
        next_sequence_ = next_sequence_ + 1;
        stable_heap_push<Order>(entries_.mutable_view());
        return pushed;
    }

    // The value that pop would remove, or null when empty. Borrowed: valid
    // until the queue next changes.
    [[nodiscard]] const Value* first() const& noexcept {
        if (entries_.empty()) return nullptr;
        const Value* const result = &entries_[0].value;
        return result;
    }
    const Value* first() const&& = delete;

    // Removes and returns the first value, or none when empty.
    [[nodiscard]] Optional<Value> pop() noexcept {
        if (entries_.empty()) return Optional<Value>::none();
        stable_heap_pop<Order>(entries_.mutable_view());
        HeapEntry<Value> entry = entries_.take_last();
        if (entries_.empty()) next_sequence_ = 0;
        return Optional<Value>::of(static_cast<Value&&>(entry.value));
    }

    void clear() noexcept {
        entries_.clear();
        next_sequence_ = 0;
    }

private:
    Vector<HeapEntry<Value>> entries_;
    u64 next_sequence_{0};
};

} // namespace stxlib
