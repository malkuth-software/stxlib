// The atomic-wait module for Linux, glibc and musl
// (hosted/atomic_wait_backend.hpp): the futex system call, made through
// syscall(2) since neither C library wraps it.
//
// futex(2) (the man-pages project's page, from the published page, not
// re-read in this batch):
//   - FUTEX_WAIT_PRIVATE sleeps while *uaddr == val, the comparison and the
//     sleep atomic with respect to FUTEX_WAKE; its timeout is relative and
//     measured on CLOCK_MONOTONIC; it returns 0 when woken, or -1 with
//     EAGAIN (the word already differed), ETIMEDOUT or EINTR, any of which
//     core's loop absorbs.
//   - FUTEX_WAKE_PRIVATE wakes at most val waiters and returns how many.
//   - The _PRIVATE forms (FUTEX_PRIVATE_FLAG, Linux 2.6.22) are for a word
//     no other process maps, which atomic_word.hpp requires.
// libstdc++ and libc++ wait for std::atomic the same way (libc++
// src/atomic.cpp's __libcpp_platform_wait_on_address uses FUTEX_WAIT_PRIVATE
// and FUTEX_WAKE_PRIVATE at llvmorg-22.1.8; not re-read at the pin in this
// batch).
//
// The time argument is a struct timespec of the C library's time_t, which
// STXlib requires to be 64 bits (threads_posix.cpp). A 64-bit host's
// SYS_futex takes that timespec; a 32-bit host with a 64-bit time_t uses
// SYS_futex_time64 (Linux 5.1), whose timespec is 64-bit too.

#include "hosted/atomic_wait_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <climits>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace stxlib::atomic_wait_backend {

namespace {

static_assert(sizeof(time_t) >= 8, "time_t must hold 64-bit seconds");

#if defined(__SIZEOF_LONG__) && __SIZEOF_LONG__ == 4
constexpr long futex_system_call = SYS_futex_time64;
#else
constexpr long futex_system_call = SYS_futex;
#endif

constexpr i64 nanoseconds_per_second = 1000000000;

[[noreturn]] void defect(const TextView message, const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.atomic_word",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = message});
    fail_fast(record);
}

// The futex word is only read by the kernel for a wait and a wake; the
// system call takes a non-const address.
long futex(const u32* const word, const int operation, const u32 value, const timespec* const timeout) noexcept {
    const long result = ::syscall(futex_system_call, const_cast<u32*>(word), operation, value, timeout, nullptr, 0);
    return result;
}

void wake(const u32* const word, const u32 count) noexcept {
    if (futex(word, FUTEX_WAKE_PRIVATE, count, nullptr) < 0) defect("futex FUTEX_WAKE_PRIVATE failed", errno);
}

} // namespace

bool wait(const u32* const word, const u32 expected, const bool timed, const Duration interval) noexcept {
    timespec relative{};
    const timespec* timeout = nullptr;
    if (timed) {
        const i64 nanoseconds = interval.count_nanoseconds();
        relative.tv_sec = static_cast<time_t>(nanoseconds / nanoseconds_per_second);
        relative.tv_nsec = static_cast<long>(nanoseconds % nanoseconds_per_second);
        timeout = &relative;
    }
    if (futex(word, FUTEX_WAIT_PRIVATE, expected, timeout) == 0) return false;
    const int error = errno;
    if (error == ETIMEDOUT) return true;
    if (error == EAGAIN || error == EINTR) return false;
    defect("futex FUTEX_WAIT_PRIVATE failed", error);
}

void wake_one(const u32* const word) noexcept { wake(word, 1); }

void wake_all(const u32* const word) noexcept { wake(word, static_cast<u32>(INT_MAX)); }

} // namespace stxlib::atomic_wait_backend
