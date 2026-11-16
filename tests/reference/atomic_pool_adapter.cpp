// The reference's C interface (atomic_pool.h) implemented over
// stxlib::AtomicPool, so that tests/thread_pool_conformance.cpp runs one set
// of checks against both the library and atomic_pool.c. Test-only. Ported
// from stx_auxlib's tests/reference/atomic_pool_adapter.cpp at b6e330a:
// STXlib's pool reports OperationStatus (thread_pool.hpp, "Changes from
// stx_auxlib"), mapped back to the reference's codes here, and its tasks'
// functions are noexcept, so each reference task runs through one noexcept
// trampoline over a copy of the reference task the adapter owns.

#include "atomic_pool.h"

#include "stxlib/thread_pool.hpp"

#include <cstdint>
#include <new>
#include <vector>

namespace {

// A batch's tasks: copies of the reference tasks, and the pool's tasks
// that point at them. `reference` is filled before `pooled` borrows its
// elements and is not resized while the pool holds them.
struct CopiedBatch final {
    std::vector<gui_forms_atomic_pool_task_t> reference{};
    std::vector<stxlib::PoolTask> pooled{};
};

void run_reference_task(void* const argument) noexcept {
    const gui_forms_atomic_pool_task_t& task = *static_cast<const gui_forms_atomic_pool_task_t*>(argument);
    task.function(task.argument);
}

// Copies the tasks; false when one has no function (the reference's
// THREADPOOL_INVALID_TASK, refused before any task runs).
bool copy_tasks(const gui_forms_atomic_pool_task_t* const tasks, const uint32_t count, CopiedBatch& output) {
    output.reference.assign(tasks, tasks + count);
    output.pooled.resize(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        if (output.reference[index].function == nullptr) return false;
        output.pooled[index].function = run_reference_task;
        output.pooled[index].argument = &output.reference[index];
    }
    return true;
}

gui_forms_atomic_pool_result_t to_reference(const stxlib::OperationStatus& status) {
    if (status.succeeded()) return THREADPOOL_OK;
    const stxlib::ErrorRecord& record = *status.error();
    if (record.kind() == stxlib::ErrorKind::out_of_memory) return THREADPOOL_ALLOCATION_FAILED;
    if (record.kind() == stxlib::ErrorKind::invalid_argument) {
        if (record.code() == static_cast<stxlib::u32>(stxlib::PoolFailure::task_without_function)) {
            return THREADPOOL_INVALID_TASK;
        }
        return THREADPOOL_INVALID_ARGUMENT;
    }
    return THREADPOOL_SYSTEM_ERROR;
}

} // namespace

struct gui_forms_atomic_pool_t {
    stxlib::AtomicPool pool{};
    // The accepted asynchronous batch, borrowed by the pool from begin until
    // wait. Only the thread that called begin touches it.
    CopiedBatch asynchronous{};
};

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_create_checked(const uint32_t thread_count,
                                                                    gui_forms_atomic_pool_t** const output) {
    if (output == nullptr || thread_count == 0) return THREADPOOL_INVALID_ARGUMENT;
    *output = nullptr;
    stxlib::OperationResult<stxlib::AtomicPool> creation = stxlib::AtomicPool::create(thread_count);
    if (creation.failed()) return to_reference(creation.status());
    gui_forms_atomic_pool_t* const adapter = new (std::nothrow) gui_forms_atomic_pool_t();
    if (adapter == nullptr) return THREADPOOL_ALLOCATION_FAILED;
    (*adapter).pool = static_cast<stxlib::OperationResult<stxlib::AtomicPool>&&>(creation).take_value();
    *output = adapter;
    return THREADPOOL_OK;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_run_checked(gui_forms_atomic_pool_t* const adapter,
                                                                 const gui_forms_atomic_pool_task_t* const tasks,
                                                                 const uint32_t count) {
    if (adapter == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    if (count == 0) return THREADPOOL_OK;
    if (tasks == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    // Per-call storage: concurrent runs on one pool must not share it.
    CopiedBatch copied{};
    if (!copy_tasks(tasks, count, copied)) return THREADPOOL_INVALID_TASK;
    const stxlib::View<stxlib::PoolTask> batch(copied.pooled.data(), copied.pooled.size());
    const stxlib::OperationStatus result = (*adapter).pool.run(batch);
    return to_reference(result);
}

uint32_t gui_forms_atomic_pool_thread_count(const gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return 0;
    const std::uint32_t result = (*adapter).pool.thread_count();
    return result;
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_begin_checked(gui_forms_atomic_pool_t* const adapter,
                                                                   const gui_forms_atomic_pool_task_t* const tasks,
                                                                   const uint32_t count) {
    if (adapter == nullptr || tasks == nullptr || count == 0) return THREADPOOL_INVALID_ARGUMENT;
    // Copied aside first: a batch still in flight keeps its storage until
    // the pool accepts this one. Moving the vectors keeps their elements'
    // addresses, which the pool has borrowed.
    CopiedBatch copied{};
    if (!copy_tasks(tasks, count, copied)) return THREADPOOL_INVALID_TASK;
    const stxlib::View<stxlib::PoolTask> batch(copied.pooled.data(), copied.pooled.size());
    const stxlib::OperationStatus result = (*adapter).pool.begin(batch);
    if (result.succeeded()) (*adapter).asynchronous = static_cast<CopiedBatch&&>(copied);
    return to_reference(result);
}

gui_forms_atomic_pool_result_t gui_forms_atomic_pool_wait_checked(gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return THREADPOOL_INVALID_ARGUMENT;
    const stxlib::OperationStatus result = (*adapter).pool.wait();
    return to_reference(result);
}

int gui_forms_atomic_pool_is_worker(const gui_forms_atomic_pool_t* const adapter) {
    if (adapter == nullptr) return 0;
    if ((*adapter).pool.is_worker()) return 1;
    return 0;
}

void gui_forms_atomic_pool_destroy(gui_forms_atomic_pool_t* const adapter) { delete adapter; }

gui_forms_atomic_pool_t* gui_forms_atomic_pool_create(const uint32_t thread_count) {
    gui_forms_atomic_pool_t* pool = nullptr;
    const gui_forms_atomic_pool_result_t result = gui_forms_atomic_pool_create_checked(thread_count, &pool);
    if (result != THREADPOOL_OK) return nullptr;
    return pool;
}

void gui_forms_atomic_pool_run(gui_forms_atomic_pool_t* const pool, gui_forms_atomic_pool_task_t* const tasks,
                               const uint32_t count) {
    static_cast<void>(gui_forms_atomic_pool_run_checked(pool, tasks, count));
}
