// The wake channel on Emscripten (include/stxlib/wake.hpp; owner decision
// 2026-11-15): a 32-bit flag in wasm memory and Emscripten's futex. A page
// has no descriptors to poll, so a wait takes no host handles.
//
// Facts at emscripten 6.0.13 (system/include/emscripten/
// threading_primitives.h:190-213): emscripten_futex_wait(address, value,
// milliseconds) waits while the word equals `value`, for at most the
// milliseconds given as a double, and returns 0, -ETIMEDOUT or
// -EWOULDBLOCK (the word already differed), or -ECANCELED; it works on
// every thread, the main browser thread included, where it busy-waits
// (site/source/docs/porting/pthreads.rst:75-117).
// emscripten_futex_wake(address, count) wakes up to `count` waiters.
//
// The flag is a u32 taken from stxlib::memory when the channel is made and
// released when it is closed; both handles hold its address (no address is
// -1, the closed value). wake stores 1 with release order and wakes every
// waiter; drain exchanges 0 with acquire order; wait checks the flag before
// each futex wait, so a wake between the check and the wait makes the wait
// return at once (-EWOULDBLOCK), never sleep past it.
//
// Early exit: the futex calls are wasm code and imports that do not throw;
// the flag is whole before and after each of them.

#include "stxlib/capabilities.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/wake.hpp"

#include "core/host_failure.hpp"

#include <atomic>
#include <cerrno>
#include <climits>
#include <new>

#include <emscripten/threading.h>
#include <emscripten/threading_primitives.h>

namespace stxlib {

namespace {

u32* flag_of(const i64 handle) noexcept {
    u32* const result = reinterpret_cast<u32*>(static_cast<uptr>(handle));
    return result;
}

// Milliseconds for one futex wait: the remaining time, at most a day
// (longer waits wait again).
double wait_milliseconds(const Duration remaining) noexcept {
    const i64 nanoseconds = remaining.count_nanoseconds();
    constexpr i64 day = static_cast<i64>(86400) * 1000000000;
    const i64 bounded = nanoseconds < day ? nanoseconds : day;
    const double result = static_cast<double>(bounded) / 1000000.0;
    return result;
}

} // namespace

OperationResult<WakeChannel> WakeChannel::create() noexcept {
    const Operation operation = "stxlib.wake.create";
    if (!capability_supported(Capability::wake_channel)) {
        return OperationResult<WakeChannel>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                          HostCode::none));
    }
    void* const block = memory::allocate(sizeof(u32), alignof(u32));
    if (block == nullptr) {
        return OperationResult<WakeChannel>::failure(host_failure::record(ErrorKind::out_of_memory, operation,
                                                                          HostCode::none));
    }
    u32* const flag = ::new (block) u32(0);
    const i64 handle = static_cast<i64>(reinterpret_cast<uptr>(flag));
    return OperationResult<WakeChannel>::success(WakeChannel(handle, handle));
}

WakeChannel::~WakeChannel() { close_handles(); }

WakeChannel::WakeChannel(WakeChannel&& other) noexcept
    : read_handle_(other.read_handle_), write_handle_(other.write_handle_) {
    other.read_handle_ = closed;
    other.write_handle_ = closed;
}

WakeChannel& WakeChannel::operator=(WakeChannel&& other) noexcept {
    if (this == &other) return *this;
    close_handles();
    read_handle_ = other.read_handle_;
    write_handle_ = other.write_handle_;
    other.read_handle_ = closed;
    other.write_handle_ = closed;
    return *this;
}

void WakeChannel::wake() const noexcept {
    if (write_handle_ == closed) return;
    u32* const flag = flag_of(write_handle_);
    std::atomic_ref<u32>(*flag).store(1, std::memory_order_release);
    static_cast<void>(emscripten_futex_wake(flag, INT_MAX));
}

bool WakeChannel::drain() noexcept {
    if (read_handle_ == closed) return false;
    const u32 previous = std::atomic_ref<u32>(*flag_of(read_handle_)).exchange(0, std::memory_order_acquire);
    return previous != 0;
}

OperationResult<WakeWait> WakeChannel::wait(const View<i64> handles, const Duration timeout) noexcept {
    const Operation operation = "stxlib.wake.wait";
    if (read_handle_ == closed) {
        return OperationResult<WakeWait>::failure(host_failure::record(ErrorKind::disposed, operation,
                                                                       HostCode::closed_handle));
    }
    if (handles.count() > maximum_wake_wait_handles) {
        return OperationResult<WakeWait>::failure(
            host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_many_handles));
    }
    // A page has no host handles to wait on (wake.hpp).
    if (handles.count() != 0) {
        return OperationResult<WakeWait>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                       HostCode::none));
    }
    u32* const flag = flag_of(read_handle_);
    const IntervalTime deadline = IntervalTime::now().after(timeout);
    for (;;) {
        if (std::atomic_ref<u32>(*flag).load(std::memory_order_acquire) != 0) {
            return OperationResult<WakeWait>::success(WakeWait::woken());
        }
        const Duration remaining = deadline.since(IntervalTime::now());
        if (remaining.count_nanoseconds() <= 0) return OperationResult<WakeWait>::success(WakeWait::timed_out());
        const int waited = emscripten_futex_wait(flag, 0, wait_milliseconds(remaining));
        // 0: woken or spurious; -ETIMEDOUT and -EWOULDBLOCK: the loop's
        // checks decide.
        if (waited != 0 && waited != -ETIMEDOUT && waited != -EWOULDBLOCK) {
            const ForeignCode code = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(-waited));
            return OperationResult<WakeWait>::failure(
                host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, code));
        }
    }
}

void WakeChannel::close_handles() noexcept {
    // Both handles hold the one flag: the channel lets go of it first, then
    // releases it once (thread-local and exit safety, rule 1).
    const i64 held = read_handle_;
    read_handle_ = closed;
    write_handle_ = closed;
    if (held != closed) memory::release(flag_of(held));
}

} // namespace stxlib
