// Tests of AtomicWord and its waits (include/stxlib/atomic_word.hpp; the
// shield's item 1, 2026-11-22), over this host's primitive (futex,
// __ulock, _umtx_op, WaitOnAddress or Emscripten's futex). The order misuse
// that fails is a must-end case in death_tests.cpp. Tested, not proved
// (proofs/REPORT.md): the stress cases are evidence of likely defects (a
// lost wake hangs the test until ctest's timeout) only.

#include "stxlib/atomic_word.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>
#include <utility>
#include <vector>

namespace {

using stxlib::AtomicWord;
using stxlib::Duration;
using stxlib::IntervalTime;
using stxlib::JoinResult;
using stxlib::MemoryOrder;
using stxlib::OperationResult;
using stxlib::StopSignal;
using stxlib::Thread;
using stxlib::u32;
using stxlib::WaitResult;

Thread start_or_fail(const Thread::Entry entry, void* const context) {
    OperationResult<Thread> started = Thread::start(entry, context);
    CHECK(started.succeeded());
    if (started.failed()) return Thread();
    Thread result = std::move(started).take_value();
    return result;
}

// ---- Operations --------------------------------------------------------------------

void operations() {
    AtomicWord word{};
    CHECK(word.load() == 0);
    AtomicWord seven(7);
    CHECK(seven.load(MemoryOrder::relaxed) == 7);

    word.store(5, MemoryOrder::release);
    CHECK(word.load(MemoryOrder::acquire) == 5);
    CHECK(word.exchange(9) == 5);
    CHECK(word.load() == 9);

    u32 expected = 3;
    CHECK(!word.compare_exchange(expected, 4));
    CHECK(expected == 9); // the failure wrote the value back
    CHECK(word.compare_exchange(expected, 4, MemoryOrder::acquire_release, MemoryOrder::acquire));
    CHECK(word.load() == 4);

    CHECK(word.fetch_add(6) == 4);
    CHECK(word.fetch_sub(3, MemoryOrder::relaxed) == 10);
    CHECK(word.fetch_or(0x10) == 7);
    CHECK(word.fetch_and(0x13) == 0x17);
    CHECK(word.load() == 0x13);

    // Addition and subtraction wrap modulo 2^32.
    word.store(0xffffffffU);
    CHECK(word.fetch_add(1) == 0xffffffffU);
    CHECK(word.load() == 0);
    CHECK(word.fetch_sub(1) == 0);
    CHECK(word.load() == 0xffffffffU);
}

// ---- Waits without a waker ------------------------------------------------------------

void waits_without_waker() {
    AtomicWord word(5);
    // A word that already differs returns at once, with its value.
    CHECK(word.wait(4) == 5);
    CHECK(word.wait(4, MemoryOrder::acquire) == 5);
    CHECK(word.wait_for(4, Duration::seconds(30)) == WaitResult::satisfied);
    // ... even with a deadline already past.
    CHECK(word.wait_until(4, IntervalTime::now().after(Duration::seconds(-1))) == WaitResult::satisfied);

    // An unchanged word times out, no earlier than the deadline.
    const IntervalTime before = IntervalTime::now();
    CHECK(word.wait_for(5, Duration::milliseconds(20)) == WaitResult::timed_out);
    CHECK(IntervalTime::now().since(before) >= Duration::milliseconds(20));
    const IntervalTime quick = IntervalTime::now();
    CHECK(word.wait_for(5, Duration::seconds(-5)) == WaitResult::timed_out);
    CHECK(word.wait_for(5, Duration::nanoseconds(stxlib::i64_min)) == WaitResult::timed_out);
    CHECK(word.wait_until(5, quick) == WaitResult::timed_out);
    CHECK(IntervalTime::now().since(quick) < Duration::seconds(1));

    // A notify with no waiter does nothing.
    word.notify_one();
    word.notify_all();
    CHECK(word.load() == 5);
}

// ---- One waiter, one waker ------------------------------------------------------------

struct Waiter final {
    AtomicWord word{};
    std::atomic<bool> waiting{false};
    u32 seen{0}; // written by the waiter, read after the join
};

void wait_for_change(const StopSignal&, void* const address) noexcept {
    Waiter& waiter = *static_cast<Waiter*>(address);
    waiter.waiting.store(true, std::memory_order_release);
    waiter.seen = waiter.word.wait(0, MemoryOrder::acquire);
}

void one_waiter() {
    Waiter waiter{};
    Thread thread = start_or_fail(wait_for_change, &waiter);
    while (!waiter.waiting.load(std::memory_order_acquire)) stxlib::yield_thread();
    // Most likely asleep in the host by now; either way the store is seen.
    stxlib::sleep_for(Duration::milliseconds(10));
    waiter.word.store(1, MemoryOrder::release);
    waiter.word.notify_one();
    CHECK(thread.join() == JoinResult::joined);
    CHECK(waiter.seen == 1);
}

// A timed waiter that a change ends before its deadline.
struct TimedWaiter final {
    AtomicWord word{};
    std::atomic<bool> waiting{false};
    WaitResult result{WaitResult::timed_out};
};

void wait_with_deadline(const StopSignal&, void* const address) noexcept {
    TimedWaiter& waiter = *static_cast<TimedWaiter*>(address);
    waiter.waiting.store(true, std::memory_order_release);
    waiter.result = waiter.word.wait_for(0, Duration::seconds(30), MemoryOrder::acquire);
}

void timed_waiter_woken() {
    TimedWaiter waiter{};
    const IntervalTime before = IntervalTime::now();
    Thread thread = start_or_fail(wait_with_deadline, &waiter);
    while (!waiter.waiting.load(std::memory_order_acquire)) stxlib::yield_thread();
    stxlib::sleep_for(Duration::milliseconds(10));
    waiter.word.store(2, MemoryOrder::release);
    waiter.word.notify_all();
    CHECK(thread.join() == JoinResult::joined);
    CHECK(waiter.result == WaitResult::satisfied);
    CHECK(IntervalTime::now().since(before) < Duration::seconds(30));
}

// ---- Many waiters ----------------------------------------------------------------------

constexpr int waiter_count = 8;

struct Crowd final {
    AtomicWord gate{};
    AtomicWord waiting{};
    std::atomic<int> passed{0};
};

void wait_at_gate(const StopSignal&, void* const address) noexcept {
    Crowd& crowd = *static_cast<Crowd*>(address);
    crowd.waiting.fetch_add(1);
    crowd.waiting.notify_all();
    if (crowd.gate.wait(0, MemoryOrder::acquire) == 1) crowd.passed.fetch_add(1, std::memory_order_relaxed);
}

void many_waiters() {
    Crowd crowd{};
    std::vector<Thread> threads{};
    for (int index = 0; index < waiter_count; ++index) threads.push_back(start_or_fail(wait_at_gate, &crowd));
    // Wait on a word for the others to arrive: the counting side of a wait.
    u32 arrived = crowd.waiting.load();
    while (arrived != static_cast<u32>(waiter_count)) arrived = crowd.waiting.wait(arrived);
    crowd.gate.store(1, MemoryOrder::release);
    crowd.gate.notify_all();
    for (Thread& thread : threads) CHECK(thread.join() == JoinResult::joined);
    CHECK(crowd.passed.load() == waiter_count);
}

// ---- Stress ----------------------------------------------------------------------------

constexpr u32 ping_pong_rounds = 20000;

// Two threads take turns: thread `side` (0 or 1) moves the word from an
// even (side 0) or odd (side 1) value to the next, waking the other. A lost
// wake leaves both asleep.
struct PingPong final {
    AtomicWord turn{};
    std::atomic<u32> next_side{0};
};

void take_turns(const StopSignal&, void* const address) noexcept {
    PingPong& game = *static_cast<PingPong*>(address);
    const u32 side = game.next_side.fetch_add(1, std::memory_order_relaxed);
    for (u32 round = 0; round < ping_pong_rounds; ++round) {
        u32 value = game.turn.load(MemoryOrder::acquire);
        while (value % 2 != side) value = game.turn.wait(value, MemoryOrder::acquire);
        game.turn.store(value + 1, MemoryOrder::release);
        game.turn.notify_one();
    }
}

void ping_pong() {
    PingPong game{};
    {
        Thread first = start_or_fail(take_turns, &game);
        Thread second = start_or_fail(take_turns, &game);
    }
    CHECK(game.turn.load() == 2 * ping_pong_rounds);
}

// Rounds of a generation gate: the leader opens generation g, every worker
// sees it and acknowledges, and the leader waits for all acknowledgements
// before opening g + 1. Both directions wait on words.
constexpr u32 gate_rounds = 2000;
constexpr u32 gate_workers = 4;

struct Generations final {
    AtomicWord generation{};
    AtomicWord acknowledged{};
};

void follow_generations(const StopSignal&, void* const address) noexcept {
    Generations& gates = *static_cast<Generations*>(address);
    u32 seen = 0;
    for (u32 round = 1; round <= gate_rounds; ++round) {
        while (seen < round) seen = gates.generation.wait(seen, MemoryOrder::acquire);
        gates.acknowledged.fetch_add(1, MemoryOrder::acquire_release);
        gates.acknowledged.notify_all();
    }
}

void generation_gate() {
    Generations gates{};
    std::vector<Thread> threads{};
    for (u32 index = 0; index < gate_workers; ++index) threads.push_back(start_or_fail(follow_generations, &gates));
    bool every_round_whole = true;
    for (u32 round = 1; round <= gate_rounds; ++round) {
        gates.generation.store(round, MemoryOrder::release);
        gates.generation.notify_all();
        const u32 target = round * gate_workers;
        u32 count = gates.acknowledged.load(MemoryOrder::acquire);
        while (count < target) count = gates.acknowledged.wait(count, MemoryOrder::acquire);
        every_round_whole = every_round_whole && count == target;
    }
    for (Thread& thread : threads) CHECK(thread.join() == JoinResult::joined);
    CHECK(every_round_whole);
    CHECK(gates.acknowledged.load() == gate_rounds * gate_workers);
}

} // namespace

int main() {
    operations();
    waits_without_waker();
    one_waiter();
    timed_waiter_woken();
    many_waiters();
    ping_pong();
    generation_gate();
    return stxlib_tests::finish("atomic_word");
}
