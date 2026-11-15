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

#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>

namespace stxlib::spin {

inline void lock(state::SpinLock& target) noexcept {
    u32 attempts = 0;
    while (target.held.exchange(1, std::memory_order_acquire) != 0) {
        while (target.held.load(std::memory_order_relaxed) != 0) {
            if (attempts < 64) {
                attempts = attempts + 1;
                host::yield_thread();
            } else {
                host::sleep_briefly();
            }
        }
    }
}

inline void unlock(state::SpinLock& target) noexcept { target.held.store(0, std::memory_order_release); }

// Holds a lock for its own lifetime.
class Guard final {
public:
    explicit Guard(state::SpinLock& target) noexcept : target_(target) { lock(target_); }
    ~Guard() { unlock(target_); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    state::SpinLock& target_;
};

} // namespace stxlib::spin
