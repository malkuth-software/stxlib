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
//
// macOS frees a module's thread_local storage from a key destructor of
// dyld's own, which runs before this one (state.cpp, end_thread, with the
// citations). The destructor therefore takes the block from the key's
// value, not from the anchor.
//
// The ended mark (ended()). Where the host may free thread_local storage
// before this destructor, the anchor cannot be trusted to say "ended" in
// later rounds, so the destructor sets the key's value to ended_mark and
// sets it again on each round it is called for (POSIX repeats while a
// value is not null, up to PTHREAD_DESTRUCTOR_ITERATIONS rounds). glibc is
// not such a host, and there the mark is never set by the destructor:
// start_thread (glibc release/2.41/master, nptl/pthread_create.c) runs
// __call_tls_dtors, then __nptl_deallocate_tsd, then __libc_thread_freeres,
// and frees the thread's TLS only with its TCB afterwards (__nptl_free_tcb,
// or at join), so the anchor itself says ended through every round. Not
// setting it there matters: a value set from a destructor makes glibc run
// another round, and ThreadSanitizer finishes the thread's state in the
// last round from a key of its own made at start-up (compiler-rt
// release/22.x, lib/tsan/rtl/tsan_interceptors_posix.cpp: thread_finalize
// sets its key again with one less until the value is 1, then calls
// DestroyThreadState; the key is armed in __tsan_thread_start_func with
// GetPthreadDestructorIterations(); not used on Apple, NetBSD or FreeBSD).
// Its key's index is below this one, so a mark set in every round brought
// this destructor, which is instrumented, back after that teardown: the
// SEGV of CI 43726327460 (linux-glibc TSan only). Without the mark this
// destructor runs once, in the round after the value was armed; a thread
// that first touches STXlib from a key destructor in the next-to-last round
// or later still runs it in the last round. Other hosts keep the mark
// (unread: musl, FreeBSD; macOS needs it).

#include "hosted/thread_exit_backend.hpp"

#include "state/state.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#endif

namespace stxlib {

namespace {

// The key, created once: 0 not yet, 1 being created, 2 ready, 3 failed.
constinit std::atomic<int> key_phase{0};
constinit pthread_key_t key{};

// The key's value once the thread's block is released, where the host
// needs it (see the file comment).
constinit char ended_mark = 0;

extern "C" void release_thread_block(void* const value) {
    if (value != &ended_mark) state::detail::end_thread(value);
#if !defined(__GLIBC__)
    static_cast<void>(::pthread_setspecific(key, &ended_mark));
#endif
}

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

u64 os_thread_id() noexcept {
#if defined(__APPLE__)
    __uint64_t thread = 0;
    if (::pthread_threadid_np(nullptr, &thread) != 0) return 0;
    const u64 result = static_cast<u64>(thread);
    return result;
#elif defined(__linux__) && !defined(__EMSCRIPTEN__)
    // SYS_gettid through syscall, which glibc and musl both declare
    // (process_linux.cpp says why not gettid).
    const long thread = ::syscall(SYS_gettid);
    const u64 result = thread > 0 ? static_cast<u64>(thread) : 0;
    return result;
#elif defined(__FreeBSD__)
    const int thread = ::pthread_getthreadid_np();
    const u64 result = thread > 0 ? static_cast<u64>(thread) : 0;
    return result;
#else
    return 0;
#endif
}

bool ended() noexcept {
    if (key_phase.load(std::memory_order_acquire) != 2) return false;
    const bool result = ::pthread_getspecific(key) == &ended_mark;
    return result;
}

void* armed() noexcept {
    if (key_phase.load(std::memory_order_acquire) != 2) return nullptr;
    void* const value = ::pthread_getspecific(key);
    void* const result = value == &ended_mark ? nullptr : value;
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
