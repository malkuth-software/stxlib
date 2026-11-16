// Process events on Windows (include/stxlib/events.hpp), over
// SetConsoleCtrlHandler. Ported from stx_auxlib's
// src/hosted/events_windows.cpp at b6e330a. The handler runs on a thread
// the system creates and only reads and sets bits in atomics. Resize and
// suspend are not console control events, so watching them is unsupported.
//
// Change from stx_auxlib: the recorded and watched events and the
// one-watch flag live in stxlib_state; the handler reaches them through a
// pointer this module stores before installing it.

#include "stxlib/events.hpp"

#include "core/host_failure.hpp"
#include "state/state.hpp"

#include <atomic>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace {

constexpr u8 supported_bits = static_cast<u8>(static_cast<u8>(ProcessEvent::interrupt) |
                                              static_cast<u8>(ProcessEvent::terminate) |
                                              static_cast<u8>(ProcessEvent::hangup));

std::atomic<state::ProcessState*> handler_state{nullptr};

ProcessEvent event_of(const DWORD control) noexcept {
    switch (control) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
        return ProcessEvent::interrupt;
    case CTRL_LOGOFF_EVENT:
        return ProcessEvent::hangup;
    default:
        break;
    }
    return ProcessEvent::terminate;
}

// How long the handler holds a close, logoff or shutdown event: under the
// 5,000 ms the system allows for each (Microsoft's HandlerRoutine page,
// "Timeouts": CTRL_CLOSE_EVENT SPI_GETHUNGAPPTIMEOUT, 5000 ms; logoff and
// shutdown SPI_GETWAITTOKILLTIMEOUT, 5000 ms; read 2026-11-15).
constexpr ULONGLONG ending_hold_milliseconds = 4500;
constexpr DWORD ending_poll_milliseconds = 10;

// Records a watched event and reports it handled; an unwatched one falls
// through to the next handler (by default, ending the process).
//
// For CTRL_CLOSE_EVENT, CTRL_LOGOFF_EVENT and CTRL_SHUTDOWN_EVENT the
// system terminates the process as soon as a handler returns TRUE (the same
// page, Remarks: "Return TRUE. In this case, no other handler functions are
// called and the system terminates the process"); the time to clean up runs
// only while the handler has not returned. So for those three the handler
// waits, polling, until the watch is released (the program has finished
// with it) or the hold above passes, then returns TRUE. A program that ends
// itself meanwhile ends this thread with it. Interrupts return at once:
// they have no timeout and do not end the process.
BOOL WINAPI record_event(const DWORD control) {
    state::ProcessState* const process = handler_state.load(std::memory_order_acquire);
    if (process == nullptr) return FALSE;
    const u8 bit = static_cast<u8>(event_of(control));
    if (((*process).watched_events.load(std::memory_order_acquire) & bit) == 0) return FALSE;
    (*process).pending_events.fetch_or(bit, std::memory_order_relaxed);
    const bool ends_process = control == CTRL_CLOSE_EVENT || control == CTRL_LOGOFF_EVENT || control == CTRL_SHUTDOWN_EVENT;
    if (!ends_process) return TRUE;
    const ULONGLONG started = GetTickCount64();
    while (((*process).watched_events.load(std::memory_order_acquire) & bit) != 0 &&
           GetTickCount64() - started < ending_hold_milliseconds) {
        Sleep(ending_poll_milliseconds);
    }
    return TRUE;
}

} // namespace

EventWatch::~EventWatch() { release(); }

EventWatch::EventWatch(EventWatch&& other) noexcept : events_(other.events_) { other.events_ = EventSet(); }

EventWatch& EventWatch::operator=(EventWatch&& other) noexcept {
    if (this == &other) return *this;
    release();
    events_ = other.events_;
    other.events_ = EventSet();
    return *this;
}

// The handler was installed by the module that made the watch; removing it
// from another module's copy names that module's function, which is not
// the one installed. Clearing the watched set makes any installed copy fall
// through, so the earlier handling is back either way.
void EventWatch::release() noexcept {
    if (events_.empty()) return;
    state::ProcessState& process = state::current_process();
    static_cast<void>(SetConsoleCtrlHandler(record_event, FALSE));
    process.watched_events.store(0, std::memory_order_release);
    process.pending_events.store(0, std::memory_order_relaxed);
    process.event_watch_active.store(false, std::memory_order_release);
    events_ = EventSet();
}

OperationResult<EventWatch> EventWatch::watch(const EventSet events) noexcept {
    if (events.empty()) {
        const ErrorRecord empty =
            host_failure::record(ErrorKind::invalid_argument, "stxlib.events.watch", HostCode::empty_request);
        return OperationResult<EventWatch>::failure(empty);
    }
    if ((events.bits() & static_cast<u8>(~supported_bits)) != 0) {
        const ErrorRecord unsupported =
            host_failure::record(ErrorKind::unsupported, "stxlib.events.watch", HostCode::none);
        return OperationResult<EventWatch>::failure(unsupported);
    }
    state::ProcessState& process = state::current_process();
    if (process.event_watch_active.exchange(true, std::memory_order_acq_rel)) {
        const ErrorRecord busy = host_failure::record(ErrorKind::limit_exceeded, "stxlib.events.watch", HostCode::busy);
        return OperationResult<EventWatch>::failure(busy);
    }
    process.pending_events.store(0, std::memory_order_relaxed);
    process.watched_events.store(events.bits(), std::memory_order_release);
    handler_state.store(&process, std::memory_order_release);
    if (!SetConsoleCtrlHandler(record_event, TRUE)) {
        const ForeignCode foreign = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError()));
        process.watched_events.store(0, std::memory_order_release);
        process.event_watch_active.store(false, std::memory_order_release);
        const ErrorRecord refused =
            host_failure::record(ErrorKind::foreign_failure, "stxlib.events.watch", HostCode::none, foreign);
        return OperationResult<EventWatch>::failure(refused);
    }
    EventWatch result;
    result.events_ = events;
    return OperationResult<EventWatch>::success(static_cast<EventWatch&&>(result));
}

EventSet EventWatch::take() noexcept {
    state::ProcessState& process = state::current_process();
    const u8 arrived = process.pending_events.exchange(0, std::memory_order_acquire);
    const EventSet result = EventSet::of_bits(static_cast<u8>(arrived & events_.bits()));
    return result;
}

// Console events arrive on their own thread, so deferring them on the
// calling thread has nothing to do.
EventDeferral::EventDeferral(const EventSet) noexcept {}
EventDeferral::~EventDeferral() {}

void ignore_broken_pipes() noexcept {}

} // namespace stxlib
