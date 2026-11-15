// Tests of stxlib::AtomicPool (include/stxlib/thread_pool.hpp). The first
// three groups port stx_auxlib's tests/thread_pool_tests.cpp at b6e330a
// (creation, the empty pool, moving the owner); the rest check the
// scheduling rules the header states, until stx_auxlib's conformance test
// against its C reference (tests/thread_pool_conformance.cpp and
// tests/reference/atomic_pool.c) comes with stxlib::algorithms in batch 3.
// Tested, not proved (proofs/REPORT.md).

#include "stxlib/thread_pool.hpp"
#include "stxlib/threads.hpp"

#include "test_support.hpp"

#include <atomic>
#include <utility>
#include <vector>

namespace {

using stxlib::AtomicPool;
using stxlib::ErrorKind;
using stxlib::PoolFailure;
using stxlib::PoolTask;
using stxlib::Result;
using stxlib::Status;
using stxlib::u32;
using stxlib::usize;
using stxlib::View;

bool refused_with(const Status& status, const PoolFailure failure) {
    if (status.succeeded()) return false;
    const stxlib::ErrorRecord& record = *status.error();
    const bool result = record.kind() == ErrorKind::invalid_argument && record.code() == static_cast<u32>(failure);
    return result;
}

void count_call(void* const address) noexcept {
    std::atomic<u32>& calls = *static_cast<std::atomic<u32>*>(address);
    calls.fetch_add(1, std::memory_order_relaxed);
}

std::vector<PoolTask> counting_tasks(std::atomic<u32>& calls, const usize count) {
    std::vector<PoolTask> tasks(count);
    for (PoolTask& task : tasks) {
        task.function = count_call;
        task.argument = &calls;
    }
    return tasks;
}

View<PoolTask> view_of(const std::vector<PoolTask>& tasks) {
    const View<PoolTask> result(tasks.data(), tasks.size());
    return result;
}

AtomicPool create_or_fail(const u32 thread_count) {
    Result<AtomicPool> created = AtomicPool::create(thread_count);
    CHECK(created.succeeded());
    if (created.failed()) return AtomicPool();
    AtomicPool result = std::move(created).take_value();
    return result;
}

// ---- Ported: creation, the empty pool, moving the owner ------------------------

void creation() {
    const Result<AtomicPool> rejected = AtomicPool::create(0);
    CHECK(rejected.failed());
    CHECK(refused_with(rejected.status(), PoolFailure::zero_threads));

    const AtomicPool pool = create_or_fail(3);
    CHECK(pool.thread_count() == 3);
    CHECK(!pool.is_worker());

    // wasm32: u32_max threads' array is not representable (a Thread holds at
    // least a pointer), so creation is refused before any thread starts
    // instead of wrapping the array's size. On 64-bit targets the product
    // fits and the same call would try to start the threads, so it is not run.
    if constexpr (sizeof(void*) == 4) {
        const Result<AtomicPool> unrepresentable = AtomicPool::create(stxlib::u32_max);
        CHECK(unrepresentable.failed() && (*unrepresentable.error()).kind() == ErrorKind::out_of_memory);
    }
}

void empty_pool() {
    AtomicPool empty{};
    std::atomic<u32> calls{0};
    const std::vector<PoolTask> tasks = counting_tasks(calls, 4);
    CHECK(empty.thread_count() == 0);
    CHECK(!empty.is_worker());
    CHECK(refused_with(empty.run(view_of(tasks)), PoolFailure::empty_pool));
    CHECK(refused_with(empty.begin(view_of(tasks)), PoolFailure::empty_pool));
    CHECK(refused_with(empty.wait(), PoolFailure::empty_pool));
    CHECK(calls.load() == 0);
}

void moving_the_owner() {
    std::atomic<u32> calls{0};
    const std::vector<PoolTask> tasks = counting_tasks(calls, 1000);
    AtomicPool first = create_or_fail(4);
    CHECK(first.run(view_of(tasks)).succeeded());

    AtomicPool second(std::move(first));
    CHECK(first.thread_count() == 0); // NOLINT(bugprone-use-after-move): the moved-from state is specified
    CHECK(refused_with(first.run(view_of(tasks)), PoolFailure::empty_pool));
    CHECK(second.thread_count() == 4);
    CHECK(second.begin(view_of(tasks)).succeeded());
    CHECK(second.wait().succeeded());

    // Assignment destroys the target's own workers before adopting the source's.
    AtomicPool target = create_or_fail(2);
    target = std::move(second);
    CHECK(target.thread_count() == 4);
    CHECK(second.thread_count() == 0); // NOLINT(bugprone-use-after-move)
    CHECK(target.run(view_of(tasks)).succeeded());
    CHECK(calls.load() == 3000);

    // A wait with no batch begun returns without releasing anything.
    CHECK(target.wait().succeeded());
    CHECK(target.run(view_of(tasks)).succeeded());
    CHECK(calls.load() == 4000);
}

// ---- The header's rules --------------------------------------------------------

// Validation happens before any task runs.
void validation() {
    AtomicPool pool = create_or_fail(2);
    std::atomic<u32> calls{0};
    CHECK(pool.run(View<PoolTask>()).succeeded());
    CHECK(refused_with(pool.begin(View<PoolTask>()), PoolFailure::empty_batch));
    std::vector<PoolTask> tasks = counting_tasks(calls, 500);
    tasks[499].function = nullptr;
    CHECK(refused_with(pool.run(view_of(tasks)), PoolFailure::task_without_function));
    CHECK(refused_with(pool.begin(view_of(tasks)), PoolFailure::task_without_function));
    CHECK(calls.load() == 0);
    // The pool still works after refusals (begin left no lock held).
    tasks[499].function = count_call;
    CHECK(pool.run(view_of(tasks)).succeeded());
    CHECK(calls.load() == 500);
}

// Each task of a batch runs exactly once; records where it ran.
struct Slot final {
    std::atomic<u32> runs{0};
    std::atomic<bool> on_worker{false};
    const AtomicPool* pool{nullptr};
    std::atomic<u32>* order{nullptr};
    u32 position{0};
};

void mark_slot(void* const address) noexcept {
    Slot& slot = *static_cast<Slot*>(address);
    slot.runs.fetch_add(1, std::memory_order_relaxed);
    slot.on_worker.store((*slot.pool).is_worker(), std::memory_order_relaxed);
    if (slot.order != nullptr) slot.position = (*slot.order).fetch_add(1, std::memory_order_relaxed);
}

struct Batch final {
    std::vector<Slot> slots;
    std::vector<PoolTask> tasks;
    std::atomic<u32> order{0};

    Batch(const AtomicPool& pool, const usize count, const bool record_order) : slots(count), tasks(count) {
        for (usize index = 0; index < count; ++index) {
            slots[index].pool = &pool;
            slots[index].order = record_order ? &order : nullptr;
            tasks[index].function = mark_slot;
            tasks[index].argument = &slots[index];
        }
    }

    bool each_once() const {
        bool result = true;
        for (const Slot& slot : slots) result = result && slot.runs.load() == 1;
        return result;
    }

    bool all_on_workers(const bool expected) const {
        bool result = true;
        for (const Slot& slot : slots) result = result && slot.on_worker.load() == expected;
        return result;
    }
};

void caller_path() {
    // At most thread_count() * 64 tasks run on the caller, in index order.
    AtomicPool pool = create_or_fail(4);
    AtomicPool& runner = pool;
    Batch batch(pool, 4 * 64, true);
    CHECK(runner.run(view_of(batch.tasks)).succeeded());
    CHECK(batch.each_once());
    CHECK(batch.all_on_workers(false));
    bool in_order = true;
    for (usize index = 0; index < batch.slots.size(); ++index) {
        in_order = in_order && batch.slots[index].position == static_cast<u32>(index);
    }
    CHECK(in_order);

    // Every batch of a one-thread pool runs on the caller.
    AtomicPool single = create_or_fail(1);
    AtomicPool& single_runner = single;
    Batch large(single, 5000, false);
    CHECK(single_runner.run(view_of(large.tasks)).succeeded());
    CHECK(large.each_once());
    CHECK(large.all_on_workers(false));
}

void worker_path() {
    // Above thread_count() * 64, tasks run on the workers only, each once;
    // counts around the claim sizes (1 to 256 per claim) and the ticket
    // edges.
    AtomicPool pool = create_or_fail(4);
    AtomicPool& runner = pool;
    const usize counts[] = {4 * 64 + 1, 4 * 64 + 31, 1000, 2048, 8191, 8193, 100000};
    for (const usize count : counts) {
        Batch batch(pool, count, false);
        CHECK(runner.run(view_of(batch.tasks)).succeeded());
        CHECK(batch.each_once());
        CHECK(batch.all_on_workers(true));
    }
}

void asynchronous() {
    // begin always uses the workers, even for one task.
    AtomicPool pool = create_or_fail(3);
    AtomicPool& runner = pool;
    const usize counts[] = {1, 2, 3, 191, 192, 193, 50000};
    for (const usize count : counts) {
        Batch batch(pool, count, false);
        CHECK(runner.begin(view_of(batch.tasks)).succeeded());
        CHECK(runner.wait().succeeded());
        CHECK(batch.each_once());
        CHECK(batch.all_on_workers(true));
    }
}

// Batches are serialized: a run from a second thread waits for an open
// asynchronous batch to finish.
struct Contender final {
    AtomicPool* pool{nullptr};
    const std::vector<PoolTask>* tasks{nullptr};
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> succeeded{false};
};

void contend(const stxlib::StopSignal&, void* const address) noexcept {
    Contender& contender = *static_cast<Contender*>(address);
    contender.started.store(true, std::memory_order_release);
    const Status status = (*contender.pool).run(view_of(*contender.tasks));
    contender.succeeded.store(status.succeeded(), std::memory_order_relaxed);
    contender.finished.store(true, std::memory_order_release);
}

struct Gate final {
    std::atomic<bool> open{false};
    std::atomic<u32> passed{0};
};

void wait_at_gate(void* const address) noexcept {
    Gate& gate = *static_cast<Gate*>(address);
    while (!gate.open.load(std::memory_order_acquire)) stxlib::yield_thread();
    gate.passed.fetch_add(1, std::memory_order_relaxed);
}

void serialization() {
    AtomicPool pool = create_or_fail(2);
    Gate gate{};
    const PoolTask gated[] = {PoolTask{wait_at_gate, &gate}};
    CHECK(pool.begin(View<PoolTask>(gated)).succeeded());

    std::atomic<u32> calls{0};
    const std::vector<PoolTask> tasks = counting_tasks(calls, 10);
    Contender contender{};
    contender.pool = &pool;
    contender.tasks = &tasks;
    Result<stxlib::Thread> started = stxlib::Thread::start(contend, &contender);
    CHECK(started.succeeded());
    while (!contender.started.load(std::memory_order_acquire)) stxlib::yield_thread();
    stxlib::sleep_for(stxlib::Duration::milliseconds(20));
    // Sampled: 20 ms is long enough for an unserialized run to finish.
    CHECK(!contender.finished.load(std::memory_order_acquire));
    CHECK(calls.load() == 0);

    gate.open.store(true, std::memory_order_release);
    CHECK(pool.wait().succeeded());
    CHECK(gate.passed.load() == 1);
    if (started.succeeded()) {
        stxlib::Thread thread = std::move(started).take_value();
        CHECK(thread.join() == stxlib::JoinResult::joined);
    }
    CHECK(contender.finished.load(std::memory_order_acquire));
    CHECK(contender.succeeded.load());
    CHECK(calls.load() == 10);
}

} // namespace

int main() {
    creation();
    empty_pool();
    moving_the_owner();
    validation();
    caller_path();
    worker_path();
    asynchronous();
    serialization();
    return stxlib_tests::finish("thread_pool");
}
