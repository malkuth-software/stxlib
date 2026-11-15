#pragma once

// Taking and releasing a process-wide lock of stxlib_state
// (state::SpinLock). Every lock that more than one module must share lives
// in the state as plain data (state.hpp), so it cannot be a std::mutex
// (not trivially destructible, and its implementation is the calling
// module's copy of the C++ runtime) or a host mutex (macOS's
// PTHREAD_MUTEX_INITIALIZER is not all zeros).
//
// The lock is an atomic exchange with a waiting policy that bounds the cost
// of waiting: a waiter first yields its time slice (64 times), then sleeps
// about a millisecond between attempts, so a holder blocked in a host call
// (a console read waiting for input) does not have waiters burning a core.
// As in stx_auxlib's environment lock (src/hosted/environment_lock.hpp at
// b6e330a), from which the yielding part is ported; the sleeping part is
// STXlib's, for the locks held across blocking calls.
//
// Not reentrant: a thread that takes a lock it holds waits forever. Every
// holder in STXlib takes one lock and calls nothing that takes it again.
//
// Process end (the security audit's A-stxlib-allocator-state-01-F2, its
// rule carried to the boundary). On Windows, ExitProcess stops every other
// thread where it stands and then runs the modules' DLL_PROCESS_DETACH code
// (a DLL's atexit tasks and static destructors among it) on the exiting
// thread; a stopped thread may hold one of these locks for ever. So a
// waiter asks state::process_ending before each wait: once it says so, a
// lock that is held is refused (lock returns false, Guard::held is false)
// instead of waited for. A lock that is free is still taken, since then no
// stopped thread was inside it and what it guards is whole. The question
// is asked only when the lock is found held, so an uncontended take costs
// nothing more. Only the exiting thread runs then, so the moment the
// answer turns true needs no ordering. On POSIX hosts and Emscripten the
// answer is always false and no lock is ever refused; children_posix.cpp,
// POSIX only, therefore does not test held(). What each other holder does
// when refused:
// - environment_lock (environment.cpp): read_variable, set_variable and
//   remove_variable fail with interrupted; nothing is read or changed.
// - exit_lock (process.cpp): register_exit_task fails with interrupted;
//   run_exit_tasks stops, and the tasks still in the table do not run.
// - console_output_lock (console.cpp): write_console and
//   write_console_line fail with interrupted; nothing is written or
//   flushed.
// - console_input_lock (console.cpp): read_console_line fails with
//   interrupted; the read-ahead is left as it is.
//
// Lock order (house style §5; audit A, X11). No STXlib spin lock is taken
// under another, so there is no order among them. Where holding the lock is
// the point, these host locks are taken under one:
// - exit_lock (process.cpp): libc's atexit lock, while registering the one
//   handler. The handler takes exit_lock only after registration, when
//   libc's lock is no longer held by this path.
// - environment_lock (environment.cpp, children_posix.cpp): libc's
//   environment lock in getenv and setenv; posix_spawn's own locks; on
//   Apple hosts pipe and fcntl for the capture pipe.
// - console_output_lock (console.cpp): the C library's FILE lock, across
//   the write and fflush.
// - console_input_lock (console.cpp): none; it is held across a blocking
//   read by design.
// - children_lock (children_posix.cpp): none; waitpid with WNOHANG does not
//   block.
// None of those host calls takes an STXlib lock, so no cycle can form.

#include "stxlib/detail/early_exit.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>

namespace stxlib::spin {

// Takes the lock; false only when it is held and the process is ending
// (see "Process end" above), and then it is not taken.
[[nodiscard]] inline bool lock(state::SpinLock& target) noexcept {
    u32 attempts = 0;
    while (target.held.exchange(1, std::memory_order_acquire) != 0) {
        while (target.held.load(std::memory_order_relaxed) != 0) {
            if (state::process_ending(state::layout_key)) return false;
            if (attempts < 64) {
                attempts = attempts + 1;
                host::yield_thread();
            } else {
                host::sleep_briefly();
            }
        }
    }
    return true;
}

inline void unlock(state::SpinLock& target) noexcept { target.held.store(0, std::memory_order_release); }

// Holds a lock for its own lifetime, unless it was refused (held() false:
// see "Process end" above); every holder on a host that can refuse tests
// held() before it touches what the lock guards.
class STXLIB_EARLY_EXIT_LOCK Guard final {
public:
    explicit Guard(state::SpinLock& target) noexcept : target_(target), held_(lock(target)) {}
    ~Guard() {
        if (held_) unlock(target_);
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    [[nodiscard]] bool held() const noexcept { return held_; }

private:
    state::SpinLock& target_;
    bool held_;
};

} // namespace stxlib::spin
