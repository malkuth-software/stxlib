// The atomic-wait module for FreeBSD (hosted/atomic_wait_backend.hpp):
// _umtx_op(2), the system call under FreeBSD's own pthread locks.
//
// _umtx_op(2) (the published manual page, not re-read at the pin in this
// batch):
//   - UMTX_OP_WAIT_UINT_PRIVATE sleeps while the unsigned int at obj equals
//     val, checked atomically with going to sleep; a timeout is passed as
//     uaddr = the size of struct _umtx_time and uaddr2 = its address, with
//     _flags 0 (relative) and _clockid CLOCK_MONOTONIC; it returns 0, or -1
//     with ETIMEDOUT or EINTR. A word that already differs returns 0.
//   - UMTX_OP_WAKE_PRIVATE wakes at most val waiters.
//   - The _PRIVATE forms are for a word no other process maps, which
//     atomic_word.hpp requires.
// libc++ waits for std::atomic the same way on FreeBSD (src/atomic.cpp at
// llvmorg-22.1.8, UMTX_OP_WAIT_UINT_PRIVATE and UMTX_OP_WAKE_PRIVATE; not
// re-read at the pin in this batch).

#include "hosted/atomic_wait_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <ctime>

#include <sys/types.h>
#include <sys/umtx.h>

namespace stxlib::atomic_wait_backend {

namespace {

static_assert(sizeof(u32) == sizeof(unsigned int), "UMTX_OP_WAIT_UINT compares an unsigned int");

constexpr i64 nanoseconds_per_second = 1000000000;

[[noreturn]] void defect(const TextView message, const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.atomic_word",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = message});
    fail_fast(record);
}

// The kernel only reads the word; the system call takes a non-const address.
void wake(const u32* const word, const unsigned long count) noexcept {
    if (::_umtx_op(const_cast<u32*>(word), UMTX_OP_WAKE_PRIVATE, count, nullptr, nullptr) != 0) {
        defect("_umtx_op UMTX_OP_WAKE_PRIVATE failed", errno);
    }
}

} // namespace

bool wait(const u32* const word, const u32 expected, const bool timed, const Duration interval) noexcept {
    int result = 0;
    if (timed) {
        const i64 nanoseconds = interval.count_nanoseconds();
        _umtx_time timeout{};
        timeout._timeout.tv_sec = static_cast<time_t>(nanoseconds / nanoseconds_per_second);
        timeout._timeout.tv_nsec = static_cast<long>(nanoseconds % nanoseconds_per_second);
        timeout._flags = 0;
        timeout._clockid = CLOCK_MONOTONIC;
        result = ::_umtx_op(const_cast<u32*>(word), UMTX_OP_WAIT_UINT_PRIVATE, static_cast<unsigned long>(expected),
                            reinterpret_cast<void*>(static_cast<std::uintptr_t>(sizeof timeout)), &timeout);
    } else {
        result = ::_umtx_op(const_cast<u32*>(word), UMTX_OP_WAIT_UINT_PRIVATE, static_cast<unsigned long>(expected),
                            nullptr, nullptr);
    }
    if (result == 0) return false;
    const int error = errno;
    if (error == ETIMEDOUT) return true;
    if (error == EINTR) return false;
    defect("_umtx_op UMTX_OP_WAIT_UINT_PRIVATE failed", error);
}

void wake_one(const u32* const word) noexcept { wake(word, 1); }

void wake_all(const u32* const word) noexcept { wake(word, static_cast<unsigned long>(INT_MAX)); }

} // namespace stxlib::atomic_wait_backend
