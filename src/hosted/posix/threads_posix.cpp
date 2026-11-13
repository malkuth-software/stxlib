// The POSIX thread module (hosted/thread_backend.hpp), for macOS, glibc and
// musl, over pthreads: the calls libc++'s std::thread, std::mutex and
// std::condition_variable make on these hosts (stx_auxlib's
// src/hosted/threads.cpp at b6e330a used those types), made directly so a
// failed start is a result in a build without exceptions.
//
// Survey D11 is open: threads take the host's default attributes (stack
// size and guard), and mutexes the default type, as std::thread and
// std::mutex did (threads.hpp).
//
// Timed waits: on Linux the condition uses CLOCK_MONOTONIC
// (pthread_condattr_setclock, POSIX.1-2008), so a change of the wall clock
// does not move the deadline; macOS has no pthread_condattr_setclock and
// waits with pthread_cond_timedwait_relative_np, a relative interval (the
// call libc++ makes there). Either way core passes a relative interval
// measured on MonotonicTime and decides the timeout by that clock.
//
// processor_count is libc++'s std::thread::hardware_concurrency: sysctl
// hw.ncpu on Apple hosts, sysconf(_SC_NPROCESSORS_ONLN) elsewhere (libc++
// src/thread.cpp; from memory of that file, not re-read).

#include "hosted/thread_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
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

bool thread_start(void* const argument, u64& handle, ForeignCode& failure) noexcept {
    pthread_t thread{};
    const int error = ::pthread_create(&thread, nullptr, thread_entry, argument);
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
#if defined(__APPLE__)
    int names[2] = {CTL_HW, HW_NCPU};
    unsigned int count = 0;
    size_t size = sizeof count;
    if (::sysctl(names, 2, &count, &size, nullptr, 0) != 0) return 0;
    return static_cast<u32>(count);
#else
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (count <= 0) return 0;
    if (static_cast<unsigned long>(count) > 0xffffffffUL) return 0xffffffffU;
    return static_cast<u32>(count);
#endif
}

} // namespace stxlib::thread_backend
