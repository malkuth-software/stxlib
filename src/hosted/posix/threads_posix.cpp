// The POSIX thread module (hosted/thread_backend.hpp), for macOS, glibc and
// musl, over pthreads: the calls libc++'s std::thread, std::mutex and
// std::condition_variable make on these hosts (stx_auxlib's
// src/hosted/threads.cpp at b6e330a used those types), made directly so a
// failed start is a result in a build without exceptions.
//
// Owner decision D11 = b (threads.hpp): each thread's attributes name its
// stack size, rounded up to the page size (macOS refuses a size that is not
// a multiple of it, pthread_attr_setstacksize(3)), and a guard of one page
// (pthread_attr_setguardsize, POSIX.1-2008; glibc's default is one page,
// stated here rather than assumed). Mutexes keep the default type: the
// recursion check is core's (threads.cpp, check builds). Man-page
// statements are from the published pages, not re-read in this batch.
//
// Timed waits: on Linux, musl and FreeBSD the condition uses
// CLOCK_MONOTONIC (pthread_condattr_setclock, POSIX.1-2008), so a change of
// the wall clock does not move the deadline; macOS has no
// pthread_condattr_setclock and waits with
// pthread_cond_timedwait_relative_np, a relative interval. Either way core
// passes a relative interval measured on IntervalTime and decides the
// timeout by that clock. This is where STXlib departs from libc++, whose
// std::condition_variable waits on a system_clock deadline with
// pthread_cond_timedwait everywhere but glibc 2.30 and later
// (include/__condition_variable/condition_variable.h:147-165,
// include/__config:650-656, src/condition_variable.cpp:41-60 at
// llvmorg-22.1.8; audit C, C-28), so a wall-clock step moves its waits on
// macOS, musl and FreeBSD and not STXlib's.
//
// processor_count is libc++'s std::thread::hardware_concurrency:
// sysconf(_SC_NPROCESSORS_ONLN) on every POSIX host, Apple included
// (src/thread.cpp:66-75 at llvmorg-22.1.8; audit C, K-2: an earlier comment
// here said sysctl hw.ncpu on Apple hosts, which libc++ does not use).
//
// Emscripten (owner decision 2026-11-15), facts at emscripten 6.0.13:
// threads are Web Workers sharing the wasm memory, started through
// Emscripten's pthreads.
//   - processor_count is emscripten_num_logical_cores
//     (system/include/emscripten/threading.h:29).
//   - thread_pool_limit is the pool's size in a browser and 0 in Node
//     (hosted/web_backend.hpp); other families have no fixed pool.
//   - pthread_create fails with EAGAIN when no pool worker is free under
//     PTHREAD_POOL_SIZE_STRICT=2, or when there is no SharedArrayBuffer
//     (src/lib/libpthread.js:714-716, :792-796): Thread::start reports it.
//   - The guard page is asked for as elsewhere, but wasm has no page
//     protection (page_protection is false; pages_posix.cpp), so it is not
//     enforced: a stack overflow is not caught by a guard there.
//   - Blocking waits on the main browser thread busy-wait
//     (site/source/docs/porting/pthreads.rst:75-117).
//   - Early exit: pthread_exit and cancellation end a thread by unwinding
//     to the JavaScript event loop (system/lib/pthread/pthread_create.c:
//     398-400; musl's pthread_cancel.c:8-15 makes cancellation a
//     pthread_exit), after running the thread's key destructors
//     (pthread_create.c:310-330). STXlib never calls either: a Thread ends
//     by returning from its body.

#include "hosted/thread_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/threading.h>

#include "hosted/web_backend.hpp"
#endif

namespace stxlib::thread_backend {

namespace {

static_assert(sizeof(pthread_mutex_t) <= storage_bytes && alignof(pthread_mutex_t) <= storage_alignment,
              "the public Mutex storage is too small for pthread_mutex_t");
static_assert(sizeof(pthread_cond_t) <= storage_bytes && alignof(pthread_cond_t) <= storage_alignment,
              "the public ConditionVariable storage is too small for pthread_cond_t");
static_assert(sizeof(pthread_t) <= sizeof(u64), "a thread handle does not fit a u64");
static_assert(sizeof(time_t) >= 8, "time_t must hold 64-bit seconds");

// A host error a correct program cannot see: fail fast with the errno.
[[noreturn]] void defect(const TextView message, const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.threads",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = message});
    fail_fast(record);
}

pthread_mutex_t* mutex_of(unsigned char* const storage) noexcept {
    pthread_mutex_t* const result = std::launder(reinterpret_cast<pthread_mutex_t*>(storage));
    return result;
}

pthread_cond_t* condition_of(unsigned char* const storage) noexcept {
    pthread_cond_t* const result = std::launder(reinterpret_cast<pthread_cond_t*>(storage));
    return result;
}

void* thread_entry(void* const argument) {
    run_started_thread(argument);
    return nullptr;
}

constexpr i64 nanoseconds_per_second = 1000000000;

timespec timespec_of(const i64 nanoseconds) noexcept {
    timespec result{};
    result.tv_sec = static_cast<time_t>(nanoseconds / nanoseconds_per_second);
    result.tv_nsec = static_cast<long>(nanoseconds % nanoseconds_per_second);
    return result;
}

} // namespace

void mutex_create(unsigned char* const storage) noexcept {
    pthread_mutex_t* const mutex = ::new (static_cast<void*>(storage)) pthread_mutex_t;
    const int error = ::pthread_mutex_init(mutex, nullptr);
    if (error != 0) defect("pthread_mutex_init failed", error);
}

void mutex_destroy(unsigned char* const storage) noexcept {
    // EBUSY (a held mutex) is a defect too; the storage is then abandoned.
    const int error = ::pthread_mutex_destroy(mutex_of(storage));
    if (error != 0) defect("pthread_mutex_destroy failed (is the mutex held?)", error);
}

void mutex_lock(unsigned char* const storage) noexcept {
    const int error = ::pthread_mutex_lock(mutex_of(storage));
    if (error != 0) defect("pthread_mutex_lock failed", error);
}

bool mutex_try_lock(unsigned char* const storage) noexcept {
    const int error = ::pthread_mutex_trylock(mutex_of(storage));
    if (error == 0) return true;
    if (error == EBUSY) return false;
    defect("pthread_mutex_trylock failed", error);
}

void mutex_unlock(unsigned char* const storage) noexcept {
    const int error = ::pthread_mutex_unlock(mutex_of(storage));
    if (error != 0) defect("pthread_mutex_unlock failed", error);
}

void condition_create(unsigned char* const storage) noexcept {
    pthread_cond_t* const condition = ::new (static_cast<void*>(storage)) pthread_cond_t;
#if defined(__APPLE__)
    const int error = ::pthread_cond_init(condition, nullptr);
#else
    pthread_condattr_t attributes{};
    int error = ::pthread_condattr_init(&attributes);
    if (error != 0) defect("pthread_condattr_init failed", error);
    error = ::pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (error == 0) error = ::pthread_cond_init(condition, &attributes);
    (void)::pthread_condattr_destroy(&attributes);
#endif
    if (error != 0) defect("pthread_cond_init failed", error);
}

void condition_destroy(unsigned char* const storage) noexcept {
    const int error = ::pthread_cond_destroy(condition_of(storage));
    if (error != 0) defect("pthread_cond_destroy failed (is a thread waiting?)", error);
}

void condition_wait(unsigned char* const condition, unsigned char* const mutex) noexcept {
    const int error = ::pthread_cond_wait(condition_of(condition), mutex_of(mutex));
    if (error != 0) defect("pthread_cond_wait failed", error);
}

bool condition_wait_for(unsigned char* const condition, unsigned char* const mutex, const Duration interval) noexcept {
#if defined(__APPLE__)
    const timespec relative = timespec_of(interval.count_nanoseconds());
    const int error = ::pthread_cond_timedwait_relative_np(condition_of(condition), mutex_of(mutex), &relative);
#else
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) defect("clock_gettime(CLOCK_MONOTONIC) failed", errno);
    const i64 now_nanoseconds =
        detail::saturating_add(detail::saturating_scale(static_cast<i64>(now.tv_sec), nanoseconds_per_second),
                               static_cast<i64>(now.tv_nsec));
    const timespec deadline = timespec_of(detail::saturating_add(now_nanoseconds, interval.count_nanoseconds()));
    const int error = ::pthread_cond_timedwait(condition_of(condition), mutex_of(mutex), &deadline);
#endif
    if (error == 0) return false;
    if (error == ETIMEDOUT) return true;
    defect("pthread_cond_timedwait failed", error);
}

void condition_notify_one(unsigned char* const condition) noexcept {
    const int error = ::pthread_cond_signal(condition_of(condition));
    if (error != 0) defect("pthread_cond_signal failed", error);
}

void condition_notify_all(unsigned char* const condition) noexcept {
    const int error = ::pthread_cond_broadcast(condition_of(condition));
    if (error != 0) defect("pthread_cond_broadcast failed", error);
}

bool thread_start(void* const argument, const usize stack_bytes, u64& handle, ForeignCode& failure) noexcept {
    const long reported_page = ::sysconf(_SC_PAGESIZE);
    const usize page = reported_page > 0 ? static_cast<usize>(reported_page) : 4096;
    // stack_bytes is at most 1 GiB (threads.hpp), so the round-up fits.
    const usize stack = (stack_bytes + page - 1) / page * page;
    pthread_attr_t attributes{};
    int error = ::pthread_attr_init(&attributes);
    if (error != 0) {
        failure = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
        return false;
    }
    error = ::pthread_attr_setstacksize(&attributes, stack);
    if (error == 0) error = ::pthread_attr_setguardsize(&attributes, page);
    pthread_t thread{};
    if (error == 0) error = ::pthread_create(&thread, &attributes, thread_entry, argument);
    (void)::pthread_attr_destroy(&attributes);
    if (error != 0) {
        failure = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
        return false;
    }
    handle = 0;
    std::memcpy(&handle, &thread, sizeof thread);
    return true;
}

void thread_join(const u64 handle) noexcept {
    pthread_t thread{};
    std::memcpy(&thread, &handle, sizeof thread);
    const int error = ::pthread_join(thread, nullptr);
    if (error != 0) defect("pthread_join failed", error);
}

void sleep_for(const Duration interval) noexcept {
    timespec remaining = timespec_of(interval.count_nanoseconds());
    while (::nanosleep(&remaining, &remaining) != 0) {
        if (errno != EINTR) return;
    }
}

u32 processor_count() noexcept {
#if defined(__EMSCRIPTEN__)
    const long count = static_cast<long>(emscripten_num_logical_cores());
#else
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
#endif
    if (count <= 0) return 0;
    if (static_cast<unsigned long>(count) > 0xffffffffUL) return 0xffffffffU;
    return static_cast<u32>(count);
}

u32 thread_pool_limit() noexcept {
#if defined(__EMSCRIPTEN__)
    const u32 result = web_backend::thread_pool_limit();
    return result;
#else
    return 0;
#endif
}

} // namespace stxlib::thread_backend
