// The atomic-wait module for macOS (hosted/atomic_wait_backend.hpp): the
// kernel's user-address locks, __ulock_wait and __ulock_wake.
//
// They are system-library entry points without an SDK header: xnu declares
// them in bsd/sys/ulock.h, which the SDK does not install, and libc++
// declares them itself for std::atomic's wait on Apple hosts (src/atomic.cpp
// at llvmorg-22.1.8, with UL_COMPARE_AND_WAIT64 and ULF_WAKE_ALL), as
// libdispatch and the Swift runtime do. So the declarations below are the
// one place STXlib states a foreign entry's type by hand; they follow xnu's
// bsd/sys/ulock.h and the operation constants its enumeration there:
//   int __ulock_wait(uint32_t operation, void* addr, uint64_t value,
//                    uint32_t timeout);   // timeout in microseconds, 0: none
//   int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
//   UL_COMPARE_AND_WAIT 1 (a 32-bit word), ULF_WAKE_ALL 0x100,
//   ULF_NO_ERRNO 0x01000000 (an error is returned as -errno instead of
//   setting errno).
// (From xnu's published source and libc++'s, not re-read at a pin in this
// batch.) The wait sleeps while *addr == value, checked atomically with
// going to sleep; it returns a non-negative count, or -ETIMEDOUT, -EINTR, or
// -EFAULT when the kernel could not read the word without faulting, which
// libdispatch treats as a spurious return and so does this module. A wake
// with no waiter returns -ENOENT, which is success here; -EINTR is retried.
//
// Available since macOS 10.12. macOS 14.4 added the public
// os_sync_wait_on_address over the same mechanism; STXlib's macOS hosts
// predate it, so the older entry points are used.

#include "hosted/atomic_wait_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cerrno>
#include <cstdint>

extern "C" int __ulock_wait(std::uint32_t operation, void* address, std::uint64_t value, std::uint32_t timeout);
extern "C" int __ulock_wake(std::uint32_t operation, void* address, std::uint64_t wake_value);

namespace stxlib::atomic_wait_backend {

namespace {

constexpr std::uint32_t ul_compare_and_wait = 1;
constexpr std::uint32_t ulf_wake_all = 0x00000100;
constexpr std::uint32_t ulf_no_errno = 0x01000000;

[[noreturn]] void defect(const TextView message, const int error) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.atomic_word",
                          ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)),
                                      .message = message});
    fail_fast(record);
}

// Microseconds of `interval`, rounded up so a wait never ends before it,
// at least 1 (0 means no timeout) and at most 2^32 - 1 (about 71 minutes; a
// longer wait returns early and core waits again).
std::uint32_t microseconds_of(const Duration interval) noexcept {
    const i64 nanoseconds = interval.count_nanoseconds();
    const i64 whole = nanoseconds / 1000;
    const i64 rounded = nanoseconds % 1000 == 0 ? whole : whole + 1;
    constexpr i64 longest = static_cast<i64>(0xffffffffU);
    if (rounded < 1) return 1;
    const std::uint32_t result = static_cast<std::uint32_t>(rounded > longest ? longest : rounded);
    return result;
}

// The kernel only reads the word; the entry points take a non-const address.
void wake(const u32* const word, const std::uint32_t operation) noexcept {
    for (;;) {
        const int result = ::__ulock_wake(operation | ulf_no_errno, const_cast<u32*>(word), 0);
        if (result >= 0 || result == -ENOENT) return;
        if (result != -EINTR) defect("__ulock_wake failed", -result);
    }
}

} // namespace

bool wait(const u32* const word, const u32 expected, const bool timed, const Duration interval) noexcept {
    const std::uint32_t timeout = timed ? microseconds_of(interval) : 0;
    const int result = ::__ulock_wait(ul_compare_and_wait | ulf_no_errno, const_cast<u32*>(word),
                                      static_cast<std::uint64_t>(expected), timeout);
    if (result >= 0) return false;
    if (result == -ETIMEDOUT) return true;
    if (result == -EINTR || result == -EFAULT) return false;
    defect("__ulock_wait failed", -result);
}

void wake_one(const u32* const word) noexcept { wake(word, ul_compare_and_wait); }

void wake_all(const u32* const word) noexcept { wake(word, ul_compare_and_wait | ulf_wake_all); }

} // namespace stxlib::atomic_wait_backend
