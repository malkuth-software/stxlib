// AtomicWord's waits (include/stxlib/atomic_word.hpp) over the family's
// atomic-wait module (hosted/atomic_wait_backend.hpp). The loads and
// read-modify-writes are inline in the header.
//
// Each wait loads the word with the caller's order and returns the first
// value that differs from `expected`; otherwise it sleeps in the host, which
// re-checks the word against `expected` before sleeping, so a store and its
// notify made between this load and the sleep wake it (the host returns at
// once) rather than being lost. The loop absorbs spurious and interrupted
// host returns. Timed waits pass the host the interval left on
// IntervalTime, so the deadline means the same on every host, and report
// timed_out by that clock alone, after a load found the word unchanged.

#include "stxlib/atomic_word.hpp"

#include "hosted/atomic_wait_backend.hpp"

namespace stxlib {

namespace {

void require_load_order(const MemoryOrder order) noexcept {
    STXLIB_REQUIRE(detail::load_order(order), "AtomicWord: a wait's order is not a load order");
}

} // namespace

u32 AtomicWord::wait(const u32 expected, const MemoryOrder order) const noexcept {
    require_load_order(order);
    for (;;) {
        const u32 observed = load(order);
        if (observed != expected) return observed;
        static_cast<void>(atomic_wait_backend::wait(&value_, expected, false, Duration()));
    }
}

WaitResult AtomicWord::wait_until(const u32 expected, const IntervalTime deadline, const MemoryOrder order) const noexcept {
    require_load_order(order);
    for (;;) {
        if (load(order) != expected) return WaitResult::satisfied;
        const IntervalTime now = IntervalTime::now();
        if (!(now < deadline)) return WaitResult::timed_out;
        static_cast<void>(atomic_wait_backend::wait(&value_, expected, true, deadline.since(now)));
    }
}

WaitResult AtomicWord::wait_for(const u32 expected, const Duration interval, const MemoryOrder order) const noexcept {
    // IntervalTime::after saturates (clocks.hpp).
    const WaitResult result = wait_until(expected, IntervalTime::now().after(interval), order);
    return result;
}

void AtomicWord::notify_one() noexcept { atomic_wait_backend::wake_one(&value_); }

void AtomicWord::notify_all() noexcept { atomic_wait_backend::wake_all(&value_); }

} // namespace stxlib
