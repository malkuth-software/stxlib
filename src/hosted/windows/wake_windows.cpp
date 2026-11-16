// The wake channel on Windows (include/stxlib/wake.hpp): one manual-reset
// event, waited on with WaitForMultipleObjects. A1 platform-rest. Host
// facts (Microsoft's documentation as published; unconfirmed at a pin):
//   - CreateEventW(nullptr, TRUE, FALSE, nullptr) makes an unnamed,
//     manual-reset event, initially not signalled; SetEvent and ResetEvent
//     raise and lower it, and a wait does not lower it.
//   - WaitForMultipleObjects waits on at most MAXIMUM_WAIT_OBJECTS (64)
//     handles and, with bWaitAll FALSE, returns WAIT_OBJECT_0 plus the
//     lowest index signalled; WAIT_ABANDONED_0 plus an index for an
//     abandoned mutex. The event and maximum_wake_wait_handles (63)
//     handles fill exactly 64, so one call covers every wait.
//   - The wait is not alertable, so it is never interrupted.
// Both handle fields hold the event; it is closed once.

#include "stxlib/capabilities.hpp"
#include "stxlib/wake.hpp"

#include "core/host_failure.hpp"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace {

static_assert(maximum_wake_wait_handles + 1 <= MAXIMUM_WAIT_OBJECTS, "one wait covers the event and the handles");

ForeignCode last_error() noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError()));
    return result;
}

HANDLE handle_of(const i64 value) noexcept {
    const HANDLE result = reinterpret_cast<HANDLE>(static_cast<intptr_t>(value));
    return result;
}

// Milliseconds for one wait: the remaining time rounded up, so a wait never
// ends before its deadline, and at most a day (longer waits wait again;
// never INFINITE).
DWORD wait_milliseconds(const Duration remaining) noexcept {
    const i64 nanoseconds = remaining.count_nanoseconds();
    if (nanoseconds <= 0) return 0;
    constexpr i64 day = 86400000;
    const i64 milliseconds = nanoseconds / 1000000 + (nanoseconds % 1000000 != 0 ? 1 : 0);
    const i64 bounded = milliseconds < day ? milliseconds : day;
    return static_cast<DWORD>(bounded);
}

} // namespace

OperationResult<WakeChannel> WakeChannel::create() noexcept {
    const Operation operation = "stxlib.wake.create";
    if (!capability_supported(Capability::wake_channel)) {
        return OperationResult<WakeChannel>::failure(host_failure::record(ErrorKind::unsupported, operation, HostCode::none));
    }
    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        return OperationResult<WakeChannel>::failure(
            host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, last_error()));
    }
    const i64 value = static_cast<i64>(reinterpret_cast<intptr_t>(event));
    return OperationResult<WakeChannel>::success(WakeChannel(value, value));
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
    static_cast<void>(SetEvent(handle_of(write_handle_)));
}

bool WakeChannel::drain() noexcept {
    if (read_handle_ == closed) return false;
    // A wake between the test and the reset merges with the one observed,
    // as bytes written during a POSIX drain are read with the rest.
    const bool raised = WaitForSingleObject(handle_of(read_handle_), 0) == WAIT_OBJECT_0;
    if (raised) static_cast<void>(ResetEvent(handle_of(read_handle_)));
    return raised;
}

OperationResult<WakeWait> WakeChannel::wait(const View<i64> handles, const Duration timeout) noexcept {
    const Operation operation = "stxlib.wake.wait";
    if (read_handle_ == closed) {
        return OperationResult<WakeWait>::failure(host_failure::record(ErrorKind::disposed, operation, HostCode::closed_handle));
    }
    if (handles.count() > maximum_wake_wait_handles) {
        return OperationResult<WakeWait>::failure(
            host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_many_handles));
    }
    HANDLE watched[maximum_wake_wait_handles + 1]{};
    watched[0] = handle_of(read_handle_);
    for (usize index = 0; index < handles.count(); ++index) watched[index + 1] = handle_of(handles[index]);
    const DWORD count = static_cast<DWORD>(handles.count() + 1);
    const IntervalTime deadline = IntervalTime::now().after(timeout);
    for (;;) {
        const Duration remaining = deadline.since(IntervalTime::now());
        const DWORD outcome = WaitForMultipleObjects(count, watched, FALSE, wait_milliseconds(remaining));
        if (outcome == WAIT_FAILED) {
            return OperationResult<WakeWait>::failure(
                host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, last_error()));
        }
        if (outcome < WAIT_OBJECT_0 + count) {
            const usize index = static_cast<usize>(outcome - WAIT_OBJECT_0);
            if (index == 0) return OperationResult<WakeWait>::success(WakeWait::woken());
            return OperationResult<WakeWait>::success(WakeWait::handle_ready(index - 1));
        }
        if (outcome >= WAIT_ABANDONED_0 && outcome < WAIT_ABANDONED_0 + count) {
            // An abandoned mutex among the handles: ready, and its owner
            // learns of the abandonment when it takes it.
            const usize index = static_cast<usize>(outcome - WAIT_ABANDONED_0);
            if (index == 0) return OperationResult<WakeWait>::success(WakeWait::woken());
            return OperationResult<WakeWait>::success(WakeWait::handle_ready(index - 1));
        }
        if (deadline <= IntervalTime::now()) return OperationResult<WakeWait>::success(WakeWait::timed_out());
    }
}

void WakeChannel::close_handles() noexcept {
    if (read_handle_ != closed) static_cast<void>(CloseHandle(handle_of(read_handle_)));
    read_handle_ = closed;
    write_handle_ = closed;
}

} // namespace stxlib
