#pragma once

// A 32-bit atomic word that threads can wait on (Capability::threads; the
// shield's item 1, owner-approved 2026-11-22): the replacement for
// std::atomic<u32> with C++20's wait, notify_one and notify_all
// ([atomics.wait]), so a thread can sleep until another changes a word
// without a Mutex and ConditionVariable beside it.
//
// Operations: the loads, stores and read-modify-writes are the compiler's
// __atomic built-ins on the word (the GCC and Clang built-ins types.hpp
// admits; this header needs no standard header), inline, with the memory
// order the caller names (MemoryOrder; sequentially consistent when not
// named). fetch_add and fetch_sub wrap modulo 2^32, as std::atomic<u32>'s
// do: a counter that must not wrap checks before it adds.
//
// Waits (out of line, src/core/atomic_word.cpp, over each family's host
// primitive, src/hosted/atomic_wait_backend.hpp):
//   Linux (glibc, musl)  futex(2), FUTEX_WAIT_PRIVATE and FUTEX_WAKE_PRIVATE
//   macOS                __ulock_wait and __ulock_wake, UL_COMPARE_AND_WAIT
//   FreeBSD              _umtx_op(2), UMTX_OP_WAIT_UINT_PRIVATE and
//                        UMTX_OP_WAKE_PRIVATE
//   Windows              WaitOnAddress, WakeByAddressSingle and
//                        WakeByAddressAll
//   Emscripten           emscripten_futex_wait and emscripten_futex_wake
// wait(expected) returns once it has loaded a value other than `expected`
// (with the order the caller names); until then it sleeps in the host,
// which checks the word against `expected` atomically with going to sleep,
// so a change and its notify between the load and the sleep are never
// missed. Like std::atomic::wait it may miss a change that is undone before
// it looks (A to B and back to A), and the host may wake it spuriously;
// both only make it look again. The timed waits measure their deadline on
// IntervalTime (clocks.hpp), as ConditionVariable's do, and report
// timed_out only when that clock has reached the deadline with the word
// still equal to `expected`.
//
// notify_one wakes at least one thread waiting on this word, notify_all
// every one. A notify that finds no waiter does nothing. Every notify
// enters the host (no waiter count is kept here), so a hot path that
// rarely has a waiter keeps its own flag. The word is private to the
// process: the host waits are the process-private forms, and a word in
// memory shared between processes is not supported.
//
// Failure: a host error in a wait or a notify (a bad address, an unknown
// operation) is a defect and fails fast (fail_fast.hpp) with
// internal_failure and the host's code. Early exits: the host calls cannot
// exit early (they are system calls, or wasm imports that do not throw),
// and no STXlib lock is held across them.

#include "stxlib/clocks.hpp"
#include "stxlib/require.hpp"
#include "stxlib/threads.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// C++'s memory orders, without consume (which every compiler treats as
// acquire). A load takes relaxed, acquire or sequential; a store relaxed,
// release or sequential; a read-modify-write any. Another order is a failed
// requirement.
enum class MemoryOrder : u8 {
    relaxed = 0,
    acquire = 1,
    release = 2,
    acquire_release = 3,
    sequential = 4,
};

namespace detail {

// The built-ins' order constants. A constant argument folds to one
// constant, so the switch costs nothing at a call site that names one.
constexpr int atomic_order(const MemoryOrder order) noexcept {
    switch (order) {
    case MemoryOrder::relaxed:
        return __ATOMIC_RELAXED;
    case MemoryOrder::acquire:
        return __ATOMIC_ACQUIRE;
    case MemoryOrder::release:
        return __ATOMIC_RELEASE;
    case MemoryOrder::acquire_release:
        return __ATOMIC_ACQ_REL;
    case MemoryOrder::sequential:
        return __ATOMIC_SEQ_CST;
    }
    return __ATOMIC_SEQ_CST;
}

constexpr bool load_order(const MemoryOrder order) noexcept {
    return order == MemoryOrder::relaxed || order == MemoryOrder::acquire || order == MemoryOrder::sequential;
}

constexpr bool store_order(const MemoryOrder order) noexcept {
    return order == MemoryOrder::relaxed || order == MemoryOrder::release || order == MemoryOrder::sequential;
}

} // namespace detail

// ic: stxlib::AtomicWord: generated from include/stxlib/atomic_word.ic.toml by tools/ic_declarations.py; edit the
// ic: declaration, not these lines. Thread: any, unless a call says otherwise.
class AtomicWord final {
public:
    constexpr AtomicWord() noexcept = default;
    constexpr explicit AtomicWord(const u32 initial) noexcept : value_(initial) {}
    AtomicWord(const AtomicWord&) = delete;
    AtomicWord& operator=(const AtomicWord&) = delete;
    AtomicWord(AtomicWord&&) = delete;
    AtomicWord& operator=(AtomicWord&&) = delete;

    // ic: Returns u32.
    [[nodiscard]] u32 load(const MemoryOrder order = MemoryOrder::sequential) const noexcept {
        STXLIB_REQUIRE(detail::load_order(order), "AtomicWord::load: not a load order");
        const u32 result = __atomic_load_n(&value_, detail::atomic_order(order));
        return result;
    }
    void store(const u32 value, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        STXLIB_REQUIRE(detail::store_order(order), "AtomicWord::store: not a store order");
        __atomic_store_n(&value_, value, detail::atomic_order(order));
    }
    // Stores `value`; returns the value before.
    [[nodiscard]] u32 exchange(const u32 value, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        const u32 result = __atomic_exchange_n(&value_, value, detail::atomic_order(order));
        return result;
    }
    // Stores `desired` if the word holds `expected`, and returns true; else
    // writes the word's value into `expected` and returns false. Never fails
    // spuriously (the strong form). `failure` is a load order no stronger
    // than `success`.
    // ic: Returns bool.
    [[nodiscard]] bool compare_exchange(u32& expected, const u32 desired,
                                        const MemoryOrder success = MemoryOrder::sequential,
                                        const MemoryOrder failure = MemoryOrder::sequential) noexcept {
        STXLIB_REQUIRE(detail::load_order(failure), "AtomicWord::compare_exchange: failure is not a load order");
        const bool result = __atomic_compare_exchange_n(&value_, &expected, desired, false,
                                                        detail::atomic_order(success), detail::atomic_order(failure));
        return result;
    }
    // Each returns the value before. Addition and subtraction wrap modulo
    // 2^32.
    u32 fetch_add(const u32 operand, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        const u32 result = __atomic_fetch_add(&value_, operand, detail::atomic_order(order));
        return result;
    }
    u32 fetch_sub(const u32 operand, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        const u32 result = __atomic_fetch_sub(&value_, operand, detail::atomic_order(order));
        return result;
    }
    u32 fetch_or(const u32 operand, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        const u32 result = __atomic_fetch_or(&value_, operand, detail::atomic_order(order));
        return result;
    }
    u32 fetch_and(const u32 operand, const MemoryOrder order = MemoryOrder::sequential) noexcept {
        const u32 result = __atomic_fetch_and(&value_, operand, detail::atomic_order(order));
        return result;
    }

    // Returns the first value it loads (with `order`, a load order) that is
    // not `expected`.
    // ic: Returns u32.
    u32 wait(u32 expected, MemoryOrder order = MemoryOrder::sequential) const noexcept;
    // As wait, until IntervalTime::now() reaches `deadline`: satisfied when
    // a value other than `expected` was loaded, timed_out otherwise.
    // ic: Returns WaitResult.
    [[nodiscard]] WaitResult wait_until(u32 expected, IntervalTime deadline,
                                        MemoryOrder order = MemoryOrder::sequential) const noexcept;
    // The deadline is now plus `interval`, saturating.
    // ic: Returns WaitResult.
    [[nodiscard]] WaitResult wait_for(u32 expected, Duration interval,
                                      MemoryOrder order = MemoryOrder::sequential) const noexcept;
    // Wake one waiter, or every waiter, of this word. A change is made
    // before its notify; the notify itself orders nothing.
    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    // Aligned to its size, as every host wait requires of the word.
    alignas(4) u32 value_{0};
};

static_assert(sizeof(AtomicWord) == 4 && alignof(AtomicWord) == 4, "AtomicWord is one aligned 32-bit word");

} // namespace stxlib
