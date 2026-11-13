#pragma once

// The owner's threadpool_atomic_fast algorithm as an owner type (part of
// Capability::threads). Ported from stx_auxlib's
// include/stx_auxlib/thread_pool.hpp and src/hosted/thread_pool.cpp at
// b6e330a, which keep the C reference's (stx_auxlib
// tests/reference/atomic_pool.c) algorithm, memory orders and lock order.
// Workers claim tickets from one atomic counter in blocks sized to the
// batch; the caller blocks until the batch finishes (run) or until it waits
// (begin and wait).
//
// Ownership: create returns the only owner. Moving transfers the workers and
// leaves the source empty; every call on an empty pool fails with
// invalid_argument and PoolFailure::empty_pool (thread_count() is 0,
// is_worker() false). Destruction stops and joins every worker. It must not
// overlap another call on the pool, and an asynchronous batch must be
// finished by wait first. The pool's state is a block of STXlib's
// allocator, which every module shares (memory.hpp).
//
// Execution: batches are serialized. run returns after every task of the
// batch has finished. A batch of at most thread_count() * 64 tasks, and
// every batch of a one-thread pool, runs on the calling thread in index
// order; larger batches run on the workers only, in unspecified order. begin
// always uses the workers, including for one task; the thread that called
// begin must call wait. Tasks stay borrowed until run or wait returns. A
// task must not call into its own pool.
//
// Reductions (STXlib's floating-point discipline, F5 = 1, 2026-11-13): a
// reduction STXlib spreads over a pool partitions its terms into fixed
// chunks of 4096, never by thread count; each chunk reduces in the fixed
// lane order, and the chunk results combine in chunk-index order on the
// calling thread (src/core/float_reduce.hpp states the shape). The pool's
// unspecified task order and its thread count then change the speed of a
// reduction, never its bits. A consumer's own reductions are its to order.
//
// Validation happens before any task runs: an empty run succeeds; an empty
// begin, a batch of more than 2^32 - 1 tasks, or a task without a function
// is refused and runs nothing.
//
// Changes from stx_auxlib: results are Status and Result (PoolResult's
// invalid_argument and invalid_task become invalid_argument with a
// PoolFailure code; allocation_failed becomes out_of_memory; a thread the
// host refuses at creation is limit_exceeded with the host's code). The
// workers and locks are STXlib's Thread, Mutex and ConditionVariable
// (threads.hpp), so a failed lock is a defect that fails fast, as in
// threads.hpp, where stx_auxlib returned PoolResult::system_error.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// One unit of work. `function` must return normally (it is noexcept: an
// exception on a worker ends the process). `argument` is borrowed until the
// batch that holds the task finishes.
struct PoolTask final {
    void (*function)(void* argument) noexcept {nullptr};
    void* argument{nullptr};
};

// The record's code when the pool refuses a call (kind invalid_argument).
enum class PoolFailure : u32 {
    empty_pool = 1,            // a moved-from or default pool
    zero_threads = 2,          // create(0)
    empty_batch = 3,           // begin with no task
    too_many_tasks = 4,        // more than 2^32 - 1 tasks
    task_without_function = 5, // a task whose function is null
};

// A fixed set of worker threads that run batches of PoolTask.
class AtomicPool final {
public:
    // thread_count must be non-zero.
    [[nodiscard]] static Result<AtomicPool> create(u32 thread_count) noexcept;
    AtomicPool() noexcept = default;
    ~AtomicPool();
    AtomicPool(const AtomicPool&) = delete;
    AtomicPool& operator=(const AtomicPool&) = delete;
    AtomicPool(AtomicPool&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
    // Destroys this pool's workers first, as the destructor does.
    AtomicPool& operator=(AtomicPool&& other) noexcept;

    [[nodiscard]] u32 thread_count() const noexcept;
    [[nodiscard]] Status run(View<PoolTask> tasks) noexcept;
    [[nodiscard]] Status begin(View<PoolTask> tasks) noexcept;
    [[nodiscard]] Status wait() noexcept;
    // True on one of this pool's worker threads.
    [[nodiscard]] bool is_worker() const noexcept;

    // The workers' shared state, defined in src/core/thread_pool.cpp.
    struct State;

private:
    explicit AtomicPool(State* const state) noexcept : state_(state) {}
    State* state_{nullptr};
};

} // namespace stxlib
