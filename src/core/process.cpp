// The exit tasks (include/stxlib/process.hpp),
// ported from stx_auxlib's src/hosted/process.cpp at b6e330a. The exit
// tasks run from one handler the C library's atexit receives once, when
// the first task is registered.
//
// Changes from stx_auxlib: the task table and its lock live in
// stxlib_state, so every module registers into one table; tasks are
// CallbackTargets; fail() and ensure() are not carried (STXLIB_REQUIRE and
// fail_fast replace them).

#include "stxlib/process.hpp"

#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

namespace stxlib {

namespace {

static_assert(maximum_exit_tasks == state::exit_task_capacity, "the exit task table and its limit agree");

// Runs the tasks newest first. Each is taken off the stack under the lock
// and run outside it, so a task may register more.
void run_exit_tasks() {
    state::ProcessState& process = state::current_process();
    for (;;) {
        state::ExitTaskEntry next{};
        {
            const spin::Guard lock(process.exit_lock);
            // Refused at the process's end (spin_lock.hpp): the tasks left
            // in the table do not run.
            if (!lock.held()) return;
            if (process.exit_task_count == 0) return;
            process.exit_task_count = process.exit_task_count - 1;
            next = process.exit_tasks[process.exit_task_count];
        }
        next.function(next.context);
    }
}

} // namespace

Status register_exit_task(const CallbackTarget<void()> task) noexcept {
    if (!task.bound()) {
        return host_failure::failed(ErrorKind::invalid_argument, "stxlib.process.register_exit_task",
                                    HostCode::empty_request);
    }
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.exit_lock);
    if (!lock.held()) {
        return host_failure::failed(ErrorKind::interrupted, "stxlib.process.register_exit_task", HostCode::none);
    }
    if (process.exit_task_count == maximum_exit_tasks) {
        return host_failure::failed(ErrorKind::limit_exceeded, "stxlib.process.register_exit_task",
                                    HostCode::table_full);
    }
    if (!process.exit_handler_registered) {
        if (!host::register_at_exit(run_exit_tasks)) {
            return host_failure::failed(ErrorKind::limit_exceeded, "stxlib.process.register_exit_task",
                                        HostCode::table_full);
        }
        process.exit_handler_registered = true;
    }
    process.exit_tasks[process.exit_task_count] = state::ExitTaskEntry{task.function(), task.context()};
    process.exit_task_count = process.exit_task_count + 1;
    return Status::success();
}

} // namespace stxlib
