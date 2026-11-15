// The POSIX thread-exit hook (hosted/thread_exit_backend.hpp), compiled into
// stxlib_state: one pthread key whose destructor releases the thread's
// block, and the layout refusal's write and abort.
//
// POSIX runs a key's destructor at the exit (pthread_exit, or a return from
// the start routine) of every thread whose value for the key is not null,
// whoever created the thread; the value is set to null before the
// destructor is called. glibc (release/2.39, nptl/pthread_create.c,
// start_thread) calls __call_tls_dtors (C++ thread_local destructors) and
// then __nptl_deallocate_tsd (key destructors, ascending key index, up to
// PTHREAD_DESTRUCTOR_ITERATIONS rounds; nptl/nptl_deallocate_tsd.c); macOS
// libpthread (src/pthread_tsd.c, _pthread_tsd_cleanup_new) visits dynamic,
// then shared-cache, then static keys, clearing each before its destructor.
// Read from source, 2026-11-15; the order relative to a consumer's
// thread_local destructors on macOS is confirmed by
// tests/thread_records_tests.cpp. Process exit runs no key destructor, so
// termination releases nothing (rule 1 as reworded).

#include "hosted/thread_exit_backend.hpp"

#include "state/state.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <unistd.h>

namespace stxlib {

namespace {

// The key, created once: 0 not yet, 1 being created, 2 ready, 3 failed.
constinit std::atomic<int> key_phase{0};
constinit pthread_key_t key{};

extern "C" void release_thread_block(void* const block) { state::detail::end_thread(block); }

bool key_ready() noexcept {
    int phase = key_phase.load(std::memory_order_acquire);
    if (phase == 2) return true;
    if (phase == 3) return false;
    int expected = 0;
    if (key_phase.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        const int error = ::pthread_key_create(&key, release_thread_block);
        key_phase.store(error == 0 ? 2 : 3, std::memory_order_release);
        return error == 0;
    }
    do {
        phase = key_phase.load(std::memory_order_acquire);
    } while (phase == 1);
    return phase == 2;
}

} // namespace

namespace thread_exit_backend {

bool arm(void* const block) noexcept {
    if (!key_ready()) return false;
    const bool result = ::pthread_setspecific(key, block) == 0;
    return result;
}

} // namespace thread_exit_backend

namespace state {

void refuse_layout(const char* const message) noexcept {
    // write is a cancellation point: with cancellation on, it could unwind
    // the thread out of here instead of ending the process (audit E, 4.9).
    int previous = 0;
    static_cast<void>(::pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
    const usize count = std::strlen(message);
    usize written = 0;
    while (written < count) {
        const ssize_t result = ::write(STDERR_FILENO, message + written, count - written);
        if (result <= 0) break;
        written += static_cast<usize>(result);
    }
    std::abort();
}

} // namespace state

} // namespace stxlib
