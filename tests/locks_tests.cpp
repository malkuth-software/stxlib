// Tests of UniqueLock, RecursiveMutex, RecursiveLock and MultiLock
// (include/stxlib/threads.hpp; the shield's items 2 and 4, 2026-11-22). The
// misuses that fail fast are must-end cases in death_tests.cpp. Tested, not
// proved (proofs/REPORT.md): every check here is one schedule the host
// chose, and the stress cases are evidence of likely defects only.
//
// Whether a mutex is free is asked from another thread (free_elsewhere): a
// try_lock on a Mutex the caller holds is itself D11's defect.

#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>
#include <utility>
#include <vector>

namespace {

using stxlib::ConditionVariable;
using stxlib::Duration;
using stxlib::JoinResult;
using stxlib::Mutex;
using stxlib::MultiLock;
using stxlib::OperationResult;
using stxlib::RecursiveLock;
using stxlib::RecursiveMutex;
using stxlib::ScopedLock;
using stxlib::StopSignal;
using stxlib::Thread;
using stxlib::UniqueLock;
using stxlib::WaitResult;

Thread start_or_fail(const Thread::Entry entry, void* const context) {
    OperationResult<Thread> started = Thread::start(entry, context);
    CHECK(started.succeeded());
    if (started.failed()) return Thread();
    Thread result = std::move(started).take_value();
    return result;
}

// ---- Asking from another thread ---------------------------------------------------

struct FreeQuery final {
    Mutex* mutex{nullptr};
    RecursiveMutex* recursive{nullptr};
    bool acquired{false}; // written by the thread, read after the join
};

void try_from_thread(const StopSignal&, void* const address) noexcept {
    FreeQuery& query = *static_cast<FreeQuery*>(address);
    if (query.mutex != nullptr) {
        query.acquired = (*query.mutex).try_lock();
        if (query.acquired) (*query.mutex).unlock();
        return;
    }
    query.acquired = (*query.recursive).try_lock();
    if (query.acquired) (*query.recursive).unlock();
}

bool free_elsewhere(Mutex& mutex) {
    FreeQuery query{};
    query.mutex = &mutex;
    Thread thread = start_or_fail(try_from_thread, &query);
    CHECK(thread.join() == JoinResult::joined);
    return query.acquired;
}

bool free_elsewhere(RecursiveMutex& mutex) {
    FreeQuery query{};
    query.recursive = &mutex;
    Thread thread = start_or_fail(try_from_thread, &query);
    CHECK(thread.join() == JoinResult::joined);
    return query.acquired;
}

// Holds a mutex on its own thread until told to let go.
struct Occupier final {
    Mutex* mutex{nullptr};
    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
};

void occupy(const StopSignal&, void* const address) noexcept {
    Occupier& occupier = *static_cast<Occupier*>(address);
    const ScopedLock lock(*occupier.mutex);
    occupier.holding.store(true, std::memory_order_release);
    while (!occupier.release.load(std::memory_order_acquire)) stxlib::yield_thread();
}

// ---- UniqueLock ---------------------------------------------------------------------

void unique_lock_states() {
    UniqueLock empty{};
    CHECK(!empty.owns());
    CHECK(empty.mutex() == nullptr);
    CHECK(empty.release() == nullptr);

    Mutex mutex{};
    {
        UniqueLock lock(mutex);
        CHECK(lock.owns());
        CHECK(lock.mutex() == &mutex);
        CHECK(!free_elsewhere(mutex));
        lock.unlock();
        CHECK(!lock.owns());
        CHECK(lock.mutex() == &mutex);
        CHECK(free_elsewhere(mutex));
        lock.lock();
        CHECK(lock.owns());
        CHECK(!free_elsewhere(mutex));
    }
    // The destructor unlocked it.
    CHECK(free_elsewhere(mutex));

    // Deferred: named, not held; try_lock takes it.
    {
        UniqueLock lock = UniqueLock::deferred(mutex);
        CHECK(!lock.owns());
        CHECK(free_elsewhere(mutex));
        CHECK(lock.try_lock());
        CHECK(lock.owns());
        CHECK(!free_elsewhere(mutex));
    }
    CHECK(free_elsewhere(mutex));

    // try_lock fails while another thread holds the mutex, and leaves the
    // lock unlocked.
    {
        Occupier occupier{};
        occupier.mutex = &mutex;
        Thread thread = start_or_fail(occupy, &occupier);
        while (!occupier.holding.load(std::memory_order_acquire)) stxlib::yield_thread();
        UniqueLock lock = UniqueLock::deferred(mutex);
        CHECK(!lock.try_lock());
        CHECK(!lock.owns());
        occupier.release.store(true, std::memory_order_release);
        CHECK(thread.join() == JoinResult::joined);
        CHECK(lock.try_lock());
        lock.unlock();
    }

    // Release gives the held mutex to the caller without unlocking it.
    {
        UniqueLock lock(mutex);
        Mutex* const released = lock.release();
        CHECK(released == &mutex);
        CHECK(!lock.owns());
        CHECK(lock.mutex() == nullptr);
        CHECK(!free_elsewhere(mutex));
        (*released).unlock();
    }
    CHECK(free_elsewhere(mutex));

    // Moving transfers the mutex and its state; the source is empty.
    {
        UniqueLock first(mutex);
        UniqueLock second(std::move(first));
        CHECK(!first.owns() && first.mutex() == nullptr); // NOLINT(bugprone-use-after-move): specified
        CHECK(second.owns() && second.mutex() == &mutex);
        CHECK(!free_elsewhere(mutex));
    }
    CHECK(free_elsewhere(mutex));

    // Move assignment unlocks the target's own mutex first.
    Mutex other{};
    {
        UniqueLock target(other);
        UniqueLock source(mutex);
        target = std::move(source);
        CHECK(free_elsewhere(other));
        CHECK(!free_elsewhere(mutex));
        CHECK(target.owns() && target.mutex() == &mutex);
        CHECK(!source.owns() && source.mutex() == nullptr); // NOLINT(bugprone-use-after-move): specified
    }
    CHECK(free_elsewhere(mutex));
}

// A queue of one value, handed over under a UniqueLock and a condition.
struct Mailbox final {
    Mutex mutex{};
    ConditionVariable changed{};
    int value{0}; // guarded by mutex
    bool full{false};
};

bool mailbox_full(const void* const address) noexcept {
    const bool result = (*static_cast<const Mailbox*>(address)).full;
    return result;
}

bool mailbox_empty(const void* const address) noexcept {
    const bool result = !(*static_cast<const Mailbox*>(address)).full;
    return result;
}

constexpr int mailbox_rounds = 2000;

void post_values(const StopSignal&, void* const address) noexcept {
    Mailbox& box = *static_cast<Mailbox*>(address);
    for (int round = 1; round <= mailbox_rounds; ++round) {
        UniqueLock lock(box.mutex);
        box.changed.wait(lock, mailbox_empty, &box);
        box.value = round;
        box.full = true;
        lock.unlock();
        box.changed.notify_all();
    }
}

void unique_lock_waits() {
    Mailbox box{};
    {
        UniqueLock lock(box.mutex);
        const WaitResult waited = box.changed.wait_for(lock, mailbox_full, &box, Duration::milliseconds(10));
        CHECK(waited == WaitResult::timed_out);
        CHECK(lock.owns());
    }
    Thread poster = start_or_fail(post_values, &box);
    int expected = 1;
    bool in_order = true;
    for (int round = 1; round <= mailbox_rounds; ++round) {
        UniqueLock lock(box.mutex);
        const WaitResult waited = box.changed.wait_for(lock, mailbox_full, &box, Duration::seconds(30));
        CHECK(waited == WaitResult::satisfied);
        in_order = in_order && box.value == expected;
        expected = expected + 1;
        box.full = false;
        lock.unlock();
        box.changed.notify_all();
    }
    CHECK(in_order);
    CHECK(poster.join() == JoinResult::joined);
}

struct HandOver final {
    Mutex mutex{};
    stxlib::u64 value{0}; // guarded by mutex
};

constexpr int stress_steps = 20000;
constexpr int stress_threads = 8;

// Each step takes the lock twice through one UniqueLock (a hand-over-hand
// section), so the count tells a lost exclusion from a lost unlock.
void hand_over(const StopSignal&, void* const address) noexcept {
    HandOver& shared = *static_cast<HandOver*>(address);
    UniqueLock lock = UniqueLock::deferred(shared.mutex);
    for (int step = 0; step < stress_steps; ++step) {
        lock.lock();
        shared.value = shared.value + 1;
        lock.unlock();
        if (!lock.try_lock()) lock.lock();
        shared.value = shared.value + 1;
        lock.unlock();
    }
}

void unique_lock_stress() {
    HandOver shared{};
    {
        std::vector<Thread> threads{};
        for (int index = 0; index < stress_threads; ++index) threads.push_back(start_or_fail(hand_over, &shared));
    }
    CHECK(shared.value == static_cast<stxlib::u64>(stress_threads) * stress_steps * 2);
}

// ---- RecursiveMutex ---------------------------------------------------------------

void take_nested(RecursiveMutex& mutex, const int depth) {
    const RecursiveLock lock(mutex);
    CHECK(mutex.held_by_current_thread());
    if (depth > 1) take_nested(mutex, depth - 1);
    CHECK(mutex.held_by_current_thread());
}

void recursive_mutex() {
    RecursiveMutex mutex{};
    CHECK(!mutex.held_by_current_thread());
    take_nested(mutex, 5);
    CHECK(!mutex.held_by_current_thread());
    CHECK(free_elsewhere(mutex));

    mutex.lock();
    mutex.lock();
    CHECK(mutex.try_lock()); // the holder's try succeeds at once
    CHECK(!free_elsewhere(mutex));
    mutex.unlock();
    mutex.unlock();
    CHECK(mutex.held_by_current_thread());
    CHECK(!free_elsewhere(mutex));
    mutex.unlock();
    CHECK(!mutex.held_by_current_thread());
    CHECK(free_elsewhere(mutex));
    CHECK(mutex.try_lock());
    mutex.unlock();
}

struct RecursiveCounter final {
    RecursiveMutex mutex{};
    stxlib::u64 value{0}; // guarded by mutex
    std::atomic<int> next_depth{0};
};

void count_nested(RecursiveCounter& counter, const int depth) {
    const RecursiveLock lock(counter.mutex);
    counter.value = counter.value + 1;
    if (depth > 1) count_nested(counter, depth - 1);
}

// Each thread nests 1 to 4 levels deep and counts once per level.
void recursive_counting(const StopSignal&, void* const address) noexcept {
    RecursiveCounter& counter = *static_cast<RecursiveCounter*>(address);
    const int depth = counter.next_depth.fetch_add(1, std::memory_order_relaxed) % 4 + 1;
    for (int step = 0; step < stress_steps; ++step) count_nested(counter, depth);
}

void recursive_stress() {
    RecursiveCounter counter{};
    {
        std::vector<Thread> threads{};
        for (int index = 0; index < stress_threads; ++index) {
            threads.push_back(start_or_fail(recursive_counting, &counter));
        }
    }
    // Depths 1, 2, 3, 4, 1, 2, 3, 4 for eight threads: 20 levels per step.
    CHECK(counter.value == static_cast<stxlib::u64>(stress_steps) * 20);
    CHECK(free_elsewhere(counter.mutex));
}

// ---- MultiLock ---------------------------------------------------------------------

void multi_lock_holds() {
    Mutex first{};
    Mutex second{};
    Mutex third{};
    {
        const MultiLock<Mutex, 3> all(third, first, second);
        CHECK(!free_elsewhere(first));
        CHECK(!free_elsewhere(second));
        CHECK(!free_elsewhere(third));
    }
    CHECK(free_elsewhere(first));
    CHECK(free_elsewhere(second));
    CHECK(free_elsewhere(third));

    // One RecursiveMutex named twice is taken twice and released twice.
    RecursiveMutex recursive{};
    RecursiveMutex other{};
    {
        const MultiLock<RecursiveMutex, 3> twice(recursive, other, recursive);
        CHECK(recursive.held_by_current_thread());
        CHECK(other.held_by_current_thread());
        CHECK(!free_elsewhere(recursive));
    }
    CHECK(!recursive.held_by_current_thread());
    CHECK(free_elsewhere(recursive));
    CHECK(free_elsewhere(other));

    // Re-entry: a MultiLock over a RecursiveMutex the thread already holds.
    {
        const RecursiveLock outer(other);
        {
            const MultiLock<RecursiveMutex, 2> inner(recursive, other);
            CHECK(recursive.held_by_current_thread());
        }
        CHECK(other.held_by_current_thread());
        CHECK(!recursive.held_by_current_thread());
    }
    CHECK(free_elsewhere(other));
}

// Two accounts moved between by threads that name them in opposite orders:
// with ordering by address, this cannot deadlock, and the total is kept.
struct Accounts final {
    Mutex left_mutex{};
    Mutex right_mutex{};
    stxlib::i64 left{1000000}; // guarded by left_mutex
    stxlib::i64 right{0};      // guarded by right_mutex
    RecursiveMutex recursive[4]{};
    stxlib::u64 recursive_count{0}; // guarded by all four of recursive
    std::atomic<int> next{0};
};

void move_between(const StopSignal&, void* const address) noexcept {
    Accounts& accounts = *static_cast<Accounts*>(address);
    const int index = accounts.next.fetch_add(1, std::memory_order_relaxed);
    for (int step = 0; step < stress_steps; ++step) {
        if (index % 2 == 0) {
            const MultiLock<Mutex, 2> both(accounts.left_mutex, accounts.right_mutex);
            accounts.left = accounts.left - 1;
            accounts.right = accounts.right + 1;
        } else {
            const MultiLock<Mutex, 2> both(accounts.right_mutex, accounts.left_mutex);
            accounts.right = accounts.right - 1;
            accounts.left = accounts.left + 1;
        }
        // Four recursive mutexes in a rotating order.
        RecursiveMutex* const ring = accounts.recursive;
        const int turn = (index + step) % 4;
        const MultiLock<RecursiveMutex, 4> four(ring[turn], ring[(turn + 1) % 4], ring[(turn + 2) % 4],
                                                ring[(turn + 3) % 4]);
        accounts.recursive_count = accounts.recursive_count + 1;
    }
}

void multi_lock_stress() {
    Accounts accounts{};
    {
        std::vector<Thread> threads{};
        for (int index = 0; index < stress_threads; ++index) {
            threads.push_back(start_or_fail(move_between, &accounts));
        }
    }
    CHECK(accounts.left + accounts.right == 1000000);
    // Four threads moved each way, the same number of steps.
    CHECK(accounts.left == 1000000 && accounts.right == 0);
    CHECK(accounts.recursive_count == static_cast<stxlib::u64>(stress_threads) * stress_steps);
}

} // namespace

int main() {
    unique_lock_states();
    unique_lock_waits();
    unique_lock_stress();
    recursive_mutex();
    recursive_stress();
    multi_lock_holds();
    multi_lock_stress();
    return stxlib_tests::finish("locks");
}
