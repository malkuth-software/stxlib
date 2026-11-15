// Copyright (c) 2026 Joshuah Rainstar
// SPDX-License-Identifier: MIT

// stxlib::AtomicPool (include/stxlib/thread_pool.hpp), over STXlib's own
// Thread, Mutex and ConditionVariable (threads.hpp), so it is host-free.
// Ported from stx_auxlib's src/hosted/thread_pool.cpp at b6e330a, which
// keeps the reference's (stx_auxlib tests/reference/atomic_pool.c)
// algorithm, memory orders, result codes and lock order; this port keeps
// them too. Where stx_auxlib held std::unique_lock across an unlock and
// relock inside the worker loop, the worker here takes a ScopedLock for each
// locked stretch; the locked stretches and what they check are the same.
// Waits take named predicates (owner decision D11), which state the
// reference's wait conditions.

#include "stxlib/thread_pool.hpp"

#include "stxlib/fail_fast.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"
#include "stxlib/threads.hpp"

#include <atomic>
#include <new>

namespace stxlib {

// Lock order: run_lock, then sleep_lock. run_lock serializes batches and is
// held for a whole batch, across begin and wait for an asynchronous one.
// sleep_lock guards `terminate` and `tasks` and pairs with both conditions.
// Workers hold a pointer to this state, never to the AtomicPool, so moving
// the owner does not disturb them.
struct AtomicPool::State final {
    Mutex sleep_lock{};
    Mutex run_lock{};
    ConditionVariable work_cv{};
    ConditionVariable done_cv{};
    // thread_count Threads in a block of STXlib's allocator; `started` of
    // them constructed.
    Thread* threads{nullptr};
    u32 thread_count{0};
    u32 started{0};
    bool terminate{false};
    // The current batch, borrowed from its caller until run or wait returns.
    const PoolTask* tasks{nullptr};
    // Tickets left to claim. A ticket is a task index plus one; zero or a
    // value above 2^32 - 1 means no ticket (claims overshoot past zero).
    std::atomic<u64> remaining{0};
    // Tasks of the current batch not yet finished.
    std::atomic<u32> pending{0};
    std::atomic<u32> claim_size{1};
    // run_lock is held from begin until wait. Only the thread that called
    // begin reads or writes this.
    bool batch_open{false};
    // The thread that holds the open asynchronous batch (its identity's
    // value), 0 when none. Written by that thread only, after taking
    // run_lock in begin and before releasing it in wait; read by any thread,
    // which finds its own identity there only if it holds the batch.
    std::atomic<u64> batch_owner{0};
};

namespace {

constexpr u64 claims_per_worker_target = 8;
constexpr u64 maximum_claim_size = 256;
constexpr u64 minimum_tasks_per_worker = 64;
constexpr u64 u32_maximum = 0xffffffffULL;

bool valid_ticket(const u64 ticket) noexcept {
    const bool result = ticket != 0 && ticket <= u32_maximum;
    return result;
}

// Wait predicates, called with sleep_lock held.
bool work_or_terminate(const void* const address) noexcept {
    const AtomicPool::State& pool = *static_cast<const AtomicPool::State*>(address);
    const bool result = pool.terminate || valid_ticket(pool.remaining.load(std::memory_order_relaxed));
    return result;
}

bool batch_finished(const void* const address) noexcept {
    const AtomicPool::State& pool = *static_cast<const AtomicPool::State*>(address);
    const bool result = pool.pending.load(std::memory_order_acquire) == 0;
    return result;
}

// A call that would deadlock on run_lock, which this thread holds for its
// open batch: a defect, recorded and failed fast (S2).
void check_not_batch_owner(const AtomicPool::State& pool, const Operation operation) noexcept {
    if (pool.batch_owner.load(std::memory_order_relaxed) != current_thread().value()) return;
    const ErrorRecord record = ErrorRecord::make(
        ErrorKind::invalid_argument, operation,
        ErrorDetail{.code = static_cast<u32>(PoolFailure::batch_still_open),
                    .message = "this thread's asynchronous batch is still open; call wait first"});
    fail_fast(record);
}

ErrorRecord refusal(const PoolFailure failure, const Operation operation) noexcept {
    const ErrorRecord result =
        ErrorRecord::make(ErrorKind::invalid_argument, operation, ErrorDetail{.code = static_cast<u32>(failure)});
    return result;
}

void run_worker(const StopSignal& stop, void* const address) noexcept {
    static_cast<void>(stop); // the pool stops its workers through `terminate`
    AtomicPool::State& pool = *static_cast<AtomicPool::State*>(address);
    for (;;) {
        {
            ScopedLock sleep(pool.sleep_lock);
            pool.work_cv.wait(sleep, work_or_terminate, &pool);
            if (pool.terminate) return;
        }

        const u64 ticket = pool.remaining.fetch_sub(1, std::memory_order_acquire);
        if (!valid_ticket(ticket)) continue;

        const PoolTask* const tasks = pool.tasks;
        const u64 claim_size = pool.claim_size.load(std::memory_order_relaxed);
        const PoolTask& first_task = tasks[ticket - 1];
        first_task.function(first_task.argument);
        u32 local_done = 1;
        // Claim further tickets in blocks of claim_size, highest first, until
        // the batch's tickets run out.
        while (ticket != 1) {
            const u64 claim_end = pool.remaining.fetch_sub(claim_size, std::memory_order_relaxed);
            if (!valid_ticket(claim_end)) break;
            const u64 claimed = claim_end < claim_size ? claim_end : claim_size;
            const u64 claim_start = claim_end - claimed;
            for (u64 claimed_ticket = claim_end; claimed_ticket > claim_start; --claimed_ticket) {
                const PoolTask& task = tasks[claimed_ticket - 1];
                task.function(task.argument);
            }
            local_done = local_done + static_cast<u32>(claimed);
            if (claim_end <= claim_size) break;
        }

        const u32 before = pool.pending.fetch_sub(local_done, std::memory_order_release);
        STXLIB_REQUIRE(before >= local_done, "AtomicPool: more tasks finished than the batch held");
        const ScopedLock sleep(pool.sleep_lock);
        if (before == local_done) pool.done_cv.notify_one();
    }
}

// Sets terminate, wakes every worker and joins the started threads (a
// Thread's destructor joins). The caller holds run_lock (or is creating the
// pool) and not sleep_lock.
void stop_workers(AtomicPool::State& pool) noexcept {
    {
        const ScopedLock sleep(pool.sleep_lock);
        pool.terminate = true;
        pool.work_cv.notify_all();
    }
    for (u32 index = 0; index < pool.started; ++index) pool.threads[index].~Thread();
    pool.started = 0;
}

// Publishes a validated batch of `count` tasks to the workers. The caller
// holds run_lock and sleep_lock; workers wake once sleep_lock is released.
void publish_batch(AtomicPool::State& pool, const PoolTask* const tasks, const u32 count) noexcept {
    pool.tasks = tasks;
    const u64 target_claims = static_cast<u64>(pool.thread_count) * claims_per_worker_target;
    u64 claim_size = (static_cast<u64>(count) + target_claims - 1) / target_claims;
    if (claim_size < 1) claim_size = 1;
    if (claim_size > maximum_claim_size) claim_size = maximum_claim_size;
    pool.pending.store(count, std::memory_order_relaxed);
    pool.claim_size.store(static_cast<u32>(claim_size), std::memory_order_relaxed);
    pool.remaining.store(count, std::memory_order_release);
    pool.work_cv.notify_all();
}

// The checks shared by run and begin, which differ only on empty batches.
Status validate_batch(const View<PoolTask> tasks, const Operation operation) noexcept {
    if (tasks.count() > u32_maximum) return Status::failure(refusal(PoolFailure::too_many_tasks, operation));
    for (usize index = 0; index < tasks.count(); ++index) {
        if (tasks[index].function == nullptr) {
            return Status::failure(refusal(PoolFailure::task_without_function, operation));
        }
    }
    return Status::success();
}

// Stops the workers of `state` and releases it; null does nothing.
void destroy_state(AtomicPool::State* const state) noexcept {
    if (state == nullptr) return;
    AtomicPool::State& owned = *state;
    if (owned.batch_owner.load(std::memory_order_relaxed) != 0) {
        const ErrorRecord record = ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.thread_pool.destroy",
            ErrorDetail{.code = static_cast<u32>(PoolFailure::batch_still_open),
                        .message = "an AtomicPool was destroyed with an asynchronous batch open"});
        fail_fast(record);
    }
    {
        const ScopedLock run(owned.run_lock);
        stop_workers(owned);
    }
    memory::release(owned.threads);
    owned.~State();
    memory::release(state);
}

} // namespace

AtomicPool::~AtomicPool() {
    destroy_state(state_);
    state_ = nullptr;
}

AtomicPool& AtomicPool::operator=(AtomicPool&& other) noexcept {
    if (this == &other) return *this;
    destroy_state(state_);
    state_ = other.state_;
    other.state_ = nullptr;
    return *this;
}

Result<AtomicPool> AtomicPool::create(const u32 thread_count) noexcept {
    const Operation operation = "stxlib.thread_pool.create";
    if (thread_count == 0) return Result<AtomicPool>::failure(refusal(PoolFailure::zero_threads, operation));
    if (thread_count > maximum_pool_threads) {
        return Result<AtomicPool>::failure(refusal(PoolFailure::too_many_threads, operation));
    }
    static_assert(static_cast<u64>(maximum_pool_threads) * sizeof(Thread) <= usize_max,
                  "the largest thread table's size is representable");
    const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, operation);
    void* const block = memory::allocate(sizeof(State), alignof(State));
    if (block == nullptr) return Result<AtomicPool>::failure(exhausted);
    State* const state = ::new (block) State();
    State& pool = *state;
    pool.threads = static_cast<Thread*>(memory::allocate(sizeof(Thread) * thread_count, alignof(Thread)));
    if (pool.threads == nullptr) {
        pool.~State();
        memory::release(state);
        return Result<AtomicPool>::failure(exhausted);
    }
    pool.thread_count = thread_count;
    for (u32 index = 0; index < thread_count; ++index) {
        Result<Thread> started = Thread::start(run_worker, state);
        if (started.failed()) {
            const ErrorRecord failure = *started.error();
            destroy_state(state);
            return Result<AtomicPool>::failure(failure);
        }
        ::new (static_cast<void*>(pool.threads + index)) Thread(static_cast<Result<Thread>&&>(started).take_value());
        pool.started = index + 1;
    }
    Result<AtomicPool> result = Result<AtomicPool>::success(AtomicPool(state));
    return result;
}

u32 AtomicPool::thread_count() const noexcept {
    if (state_ == nullptr) return 0;
    const u32 result = (*state_).thread_count;
    return result;
}

Status AtomicPool::run(const View<PoolTask> tasks) noexcept {
    const Operation operation = "stxlib.thread_pool.run";
    if (state_ == nullptr) return Status::failure(refusal(PoolFailure::empty_pool, operation));
    if (is_worker()) return Status::failure(refusal(PoolFailure::called_from_worker, operation));
    check_not_batch_owner(*state_, operation);
    if (tasks.count() == 0) return Status::success();
    Status validation = validate_batch(tasks, operation);
    if (validation.failed()) return validation;
    State& pool = *state_;
    const u32 count = static_cast<u32>(tasks.count());

    const ScopedLock run_hold(pool.run_lock);
    // Small batches, and every batch of a one-thread pool, run on the caller
    // in index order; waking workers would cost more than the batch.
    const u64 caller_limit = static_cast<u64>(pool.thread_count) * minimum_tasks_per_worker;
    if (pool.thread_count == 1 || static_cast<u64>(count) <= caller_limit) {
        for (u32 index = 0; index < count; ++index) tasks[index].function(tasks[index].argument);
        return Status::success();
    }
    ScopedLock sleep(pool.sleep_lock);
    publish_batch(pool, tasks.data(), count);
    pool.done_cv.wait(sleep, batch_finished, &pool);
    pool.tasks = nullptr;
    return Status::success();
}

Status AtomicPool::begin(const View<PoolTask> tasks) noexcept {
    const Operation operation = "stxlib.thread_pool.begin";
    if (state_ == nullptr) return Status::failure(refusal(PoolFailure::empty_pool, operation));
    if (is_worker()) return Status::failure(refusal(PoolFailure::called_from_worker, operation));
    check_not_batch_owner(*state_, operation);
    if (tasks.count() == 0) return Status::failure(refusal(PoolFailure::empty_batch, operation));
    Status validation = validate_batch(tasks, operation);
    if (validation.failed()) return validation;
    State& pool = *state_;
    const u32 count = static_cast<u32>(tasks.count());

    // Held until wait, which releases it.
    pool.run_lock.lock();
    {
        const ScopedLock sleep(pool.sleep_lock);
        publish_batch(pool, tasks.data(), count);
    }
    pool.batch_open = true;
    pool.batch_owner.store(current_thread().value(), std::memory_order_relaxed);
    return Status::success();
}

Status AtomicPool::wait() noexcept {
    const Operation operation = "stxlib.thread_pool.wait";
    if (state_ == nullptr) return Status::failure(refusal(PoolFailure::empty_pool, operation));
    if (is_worker()) return Status::failure(refusal(PoolFailure::called_from_worker, operation));
    State& pool = *state_;
    {
        ScopedLock sleep(pool.sleep_lock);
        pool.done_cv.wait(sleep, batch_finished, &pool);
        pool.tasks = nullptr;
    }
    // Releases run_lock after sleep_lock, as the reference does. A wait with
    // no batch begun holds no run_lock and releases nothing (the reference
    // would unlock a mutex it does not hold).
    if (pool.batch_open) {
        pool.batch_open = false;
        pool.batch_owner.store(0, std::memory_order_relaxed);
        pool.run_lock.unlock();
    }
    return Status::success();
}

bool AtomicPool::is_worker() const noexcept {
    if (state_ == nullptr) return false;
    const State& pool = *state_;
    const ThreadIdentity caller = current_thread();
    for (u32 index = 0; index < pool.started; ++index) {
        if (pool.threads[index].identity() == caller) return true;
    }
    return false;
}

} // namespace stxlib
