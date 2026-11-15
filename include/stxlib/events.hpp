#pragma once

// Process events (Capability::interrupt_events, resize_events and
// suspend_events), replacing signal, sigaction, sigprocmask and
// SetConsoleCtrlHandler. Ported from stx_auxlib's
// include/stx_auxlib/events.hpp at b6e330a.
//
// An EventWatch records events as they arrive; the program collects them
// with take() at a point of its choosing, so no program code runs inside a
// signal handler. While a watch is active, blocking reads and waits may
// return early when an event arrives (POSIX: EINTR), so a loop that reads
// standard input notices events promptly.
//
// Host mapping. POSIX: interrupt SIGINT, terminate SIGTERM, hangup SIGHUP,
// terminal_resized SIGWINCH, suspend SIGTSTP. Windows: interrupt
// CTRL_C_EVENT and CTRL_BREAK_EVENT, terminate CTRL_CLOSE_EVENT and
// CTRL_SHUTDOWN_EVENT, hangup CTRL_LOGOFF_EVENT; resize and suspend are not
// signals there and are unsupported. A Windows close or shutdown still ends
// the process a few seconds after the event, whatever the program does.
//
// Modules: the watch's state (the recorded events, the one-watch flag and
// the earlier handlers) lives in stxlib_state, so a watch made in one
// module may be taken from or destroyed in another. The installed signal
// handler is code of the module that called watch, so that module must stay
// loaded until the watch is destroyed.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

enum class ProcessEvent : u8 {
    interrupt = 1,
    terminate = 2,
    hangup = 4,
    terminal_resized = 8,
    suspend = 16,
};

// A set of ProcessEvent values.
class EventSet final {
public:
    constexpr EventSet() noexcept = default;

    [[nodiscard]] static constexpr EventSet of_bits(const u8 bits) noexcept {
        EventSet result;
        result.bits_ = bits;
        return result;
    }
    [[nodiscard]] constexpr bool contains(const ProcessEvent event) const noexcept {
        return (bits_ & static_cast<u8>(event)) != 0;
    }
    [[nodiscard]] constexpr EventSet with(const ProcessEvent event) const noexcept {
        return of_bits(static_cast<u8>(bits_ | static_cast<u8>(event)));
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr u8 bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool operator==(const EventSet other) const noexcept { return bits_ == other.bits_; }
    [[nodiscard]] constexpr bool operator!=(const EventSet other) const noexcept { return bits_ != other.bits_; }

private:
    u8 bits_{0};
};

// Records the watched events from creation until destruction, replacing
// their default actions (a watched interrupt no longer ends the program),
// and restores the earlier handling when destroyed. One watch at a time in
// the process. Move-only.
class EventWatch final {
public:
    // Failures: invalid_argument with HostCode::empty_request for an empty
    // set; unsupported when `events` includes one the host lacks;
    // limit_exceeded with HostCode::busy while another watch is active;
    // foreign_failure when the host refuses a handler.
    [[nodiscard]] static Result<EventWatch> watch(EventSet events) noexcept;

    EventWatch() noexcept = default;
    ~EventWatch();
    EventWatch(const EventWatch&) = delete;
    EventWatch& operator=(const EventWatch&) = delete;
    EventWatch(EventWatch&& other) noexcept;
    // Ends this watch first, as the destructor does.
    EventWatch& operator=(EventWatch&& other) noexcept;

    // The events that arrived since the last take(), and clears them.
    [[nodiscard]] EventSet take() noexcept;

private:
    void release() noexcept;

    EventSet events_{};
};

// Bytes an EventDeferral keeps for the thread's earlier signal mask
// (sigset_t is at most 128 bytes on the supported POSIX hosts).
inline constexpr usize event_deferral_state_bytes = 128;

// Defers the given events on the calling thread for this object's lifetime
// (a critical section that must not be interrupted); they are delivered
// when it ends. POSIX only; on Windows it has no effect, since console
// events arrive on their own thread. active() says whether the events are
// deferred: false on Windows, and when the host refused the mask
// (pthread_sigmask failing), so a deferral that did nothing is observable.
class EventDeferral final {
public:
    explicit EventDeferral(EventSet events) noexcept;
    ~EventDeferral();
    EventDeferral(const EventDeferral&) = delete;
    EventDeferral& operator=(const EventDeferral&) = delete;
    EventDeferral(EventDeferral&&) = delete;
    EventDeferral& operator=(EventDeferral&&) = delete;

    [[nodiscard]] bool active() const noexcept { return active_; }

private:
    // The thread's earlier mask while active_; all zero bytes otherwise.
    alignas(8) unsigned char previous_[event_deferral_state_bytes]{};
    bool active_{false};
};

// Makes writes to a closed pipe fail instead of ending the process (POSIX:
// SIGPIPE is ignored). Windows has no such signal; there it does nothing.
void ignore_broken_pipes() noexcept;

} // namespace stxlib
