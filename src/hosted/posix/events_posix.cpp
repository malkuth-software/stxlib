// Process events on POSIX hosts (include/stxlib/events.hpp), over sigaction
// and pthread_sigmask, shared by macOS, glibc and musl. Ported from
// stx_auxlib's src/hosted/events_posix.cpp at b6e330a.
//
// The handler only sets bits in one lock-free atomic, which is
// async-signal-safe; handlers are installed without SA_RESTART so a
// blocking read returns EINTR when an event arrives.
//
// Changes from stx_auxlib: the recorded events, the one-watch flag and the
// earlier handlers live in stxlib_state. The handler reaches the recorded
// events through the state accessor, state::process_state, which compares
// one integer and returns a static's address, so it is async-signal-safe;
// this module keeps no namespace-scope state of its own (audit A, H2: the
// pointer it stored before was a per-module copy, AGENTS "state lives in
// stxlib_state").

#include "stxlib/events.hpp"

#include "core/host_failure.hpp"
#include "state/state.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>

#include <pthread.h>
#include <signal.h>

namespace stxlib {

namespace {

static_assert(std::atomic<u8>::is_always_lock_free, "the signal handler needs a lock-free atomic");
static_assert(sizeof(sigset_t) <= event_deferral_state_bytes, "EventDeferral's storage is too small for sigset_t");
static_assert(sizeof(struct sigaction) <= state::saved_signal_action_bytes,
              "the state's storage is too small for struct sigaction");
static_assert(alignof(struct sigaction) <= 16, "the state's storage is not aligned for struct sigaction");

struct EventSignal final {
    ProcessEvent event;
    int signal_number;
};

constexpr EventSignal event_signals[state::watched_signal_count] = {
    {ProcessEvent::interrupt, SIGINT},
    {ProcessEvent::terminate, SIGTERM},
    {ProcessEvent::hangup, SIGHUP},
    {ProcessEvent::terminal_resized, SIGWINCH},
    {ProcessEvent::suspend, SIGTSTP},
};

// The signal handler. It calls only process_state (see the top of this
// file) and a lock-free fetch_or; a layout mismatch, which the module would
// have reported at its first access, records nothing.
void record_event(const int signal_number) {
    state::ProcessState* const process = state::process_state(state::layout_version);
    if (process == nullptr) return;
    for (const EventSignal& mapping : event_signals) {
        if (mapping.signal_number == signal_number) {
            (*process).pending_events.fetch_or(static_cast<u8>(mapping.event), std::memory_order_relaxed);
        }
    }
}

struct sigaction* saved_action(state::ProcessState& process, const usize index) noexcept {
    struct sigaction* const result = reinterpret_cast<struct sigaction*>(process.saved_signal_actions[index]);
    return result;
}

sigset_t signals_of(const EventSet events) noexcept {
    sigset_t set{};
    static_cast<void>(sigemptyset(&set));
    for (const EventSignal& mapping : event_signals) {
        if (events.contains(mapping.event)) static_cast<void>(sigaddset(&set, mapping.signal_number));
    }
    return set;
}

void restore_watched(state::ProcessState& process, const EventSet events) noexcept {
    for (usize index = 0; index < state::watched_signal_count; ++index) {
        if (events.contains(event_signals[index].event)) {
            static_cast<void>(::sigaction(event_signals[index].signal_number, saved_action(process, index), nullptr));
        }
    }
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

void EventWatch::release() noexcept {
    if (events_.empty()) return;
    state::ProcessState& process = state::current_process();
    restore_watched(process, events_);
    process.pending_events.store(0, std::memory_order_relaxed);
    process.event_watch_active.store(false, std::memory_order_release);
    events_ = EventSet();
}

Result<EventWatch> EventWatch::watch(const EventSet events) noexcept {
    if (events.empty()) {
        const ErrorRecord empty =
            host_failure::record(ErrorKind::invalid_argument, "stxlib.events.watch", HostCode::empty_request);
        return Result<EventWatch>::failure(empty);
    }
    state::ProcessState& process = state::current_process();
    if (process.event_watch_active.exchange(true, std::memory_order_acq_rel)) {
        const ErrorRecord busy = host_failure::record(ErrorKind::limit_exceeded, "stxlib.events.watch", HostCode::busy);
        return Result<EventWatch>::failure(busy);
    }
    process.pending_events.store(0, std::memory_order_relaxed);
    EventSet installed{};
    for (usize index = 0; index < state::watched_signal_count; ++index) {
        const EventSignal& mapping = event_signals[index];
        if (!events.contains(mapping.event)) continue;
        struct sigaction action{};
        action.sa_handler = record_event;
        static_cast<void>(sigemptyset(&action.sa_mask));
        action.sa_flags = 0;
        if (::sigaction(mapping.signal_number, &action, saved_action(process, index)) != 0) {
            const int error = errno;
            restore_watched(process, installed);
            process.event_watch_active.store(false, std::memory_order_release);
            const ErrorRecord refused = host_failure::record(
                ErrorKind::foreign_failure, "stxlib.events.watch", HostCode::none,
                ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
            return Result<EventWatch>::failure(refused);
        }
        installed = installed.with(mapping.event);
    }
    EventWatch result;
    result.events_ = events;
    return Result<EventWatch>::success(static_cast<EventWatch&&>(result));
}

EventSet EventWatch::take() noexcept {
    state::ProcessState& process = state::current_process();
    const u8 arrived = process.pending_events.exchange(0, std::memory_order_acquire);
    const EventSet result = EventSet::of_bits(static_cast<u8>(arrived & events_.bits()));
    return result;
}

EventDeferral::EventDeferral(const EventSet events) noexcept {
    const sigset_t blocked = signals_of(events);
    sigset_t previous{};
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous) != 0) return;
    std::memcpy(previous_, &previous, sizeof previous);
    active_ = true;
}

EventDeferral::~EventDeferral() {
    if (!active_) return;
    sigset_t previous{};
    std::memcpy(&previous, previous_, sizeof previous);
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
}

void ignore_broken_pipes() noexcept {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    static_cast<void>(sigemptyset(&action.sa_mask));
    static_cast<void>(::sigaction(SIGPIPE, &action, nullptr));
}

} // namespace stxlib
