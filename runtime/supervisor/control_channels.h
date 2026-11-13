// control_channels.h: the supervisor's control channels on FreeBSD (C2;
// design dev/notes/planning/stages/C2-web.md, section 4). The pure part,
// what a request may do and what it answers, is control/control_server.h;
// the message format is control/control_protocol.h.
//
// One channel per running service whose definition grants control: an
// AF_UNIX SOCK_SEQPACKET socket pair made just before the service is
// forked (config/system_actions.h, ChannelGrants). The service gets one
// end as its descriptor 3; the supervisor keeps the other, non-blocking
// and close-on-exec, in a fixed table of control_channels_max entries.
//
// The supervisor's main loop waits on one kqueue: the signals it handles
// (EVFILT_SIGNAL; they stay blocked and are taken with sigtimedwait after
// the wait) and every channel (EVFILT_READ). FreeBSD posts the
// EVFILT_SIGNAL note before it looks at the signal mask, so a blocked
// signal still wakes the wait (sys/kern/kern_sig.c at fb9033d2876d,
// tdsendsignal: KNOTE_LOCKED(p->p_klist, NOTE_SIGNAL | sig) precedes the
// ps_sigignore and td_sigmask checks; read 2026-11-13, line to cite).
//
// System calls: kqueue, kevent, socketpair, fcntl, recv, send, close,
// open, fstat, pread. Hosted on FreeBSD's libc for C2's first form.
//
// Console lines:
//   STXSUPERVISOR control queue failed errno=<n>
//   STXSUPERVISOR control channel pid=<n>
//   STXSUPERVISOR control channel unavailable reason=<full|errno=<n>>
//   STXSUPERVISOR control <service|pid=n> <command>[ <target>] status=<s>
//     (for start, stop and restart, and for every refused request)

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONTROL_CHANNELS_H
#define STXLIB_RUNTIME_SUPERVISOR_CONTROL_CHANNELS_H

#include <cstddef>
#include <cstdint>
#include <span>

#include "config/service_registry.h"
#include "config/system_actions.h"
#include "control/control_protocol.h"
#include "control/control_server.h"

namespace stxsupervisor {

constexpr std::size_t control_channels_max = stxconfig::services_max;
// Requests taken from one channel per wake; the rest wait for the next
// (the kqueue's read filter is level-triggered, so they wake it again).
constexpr int control_requests_per_round = 4;
// Signals the loop registers at most.
constexpr std::size_t control_signals_max = 8;

class ControlChannels final : public stxconfig::ChannelGrants {
public:
    explicit ControlChannels(int console) noexcept;
    ~ControlChannels();
    ControlChannels(const ControlChannels&) = delete;
    ControlChannels& operator=(const ControlChannels&) = delete;

    // Makes the kqueue and registers each signal (at most
    // control_signals_max). False when that fails: the supervisor then
    // waits with sigtimedwait as before and grants no channels.
    [[nodiscard]] bool open(std::span<const int> signal_numbers) noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    [[nodiscard]] int prepare_channel() noexcept override;
    void channel_started(int pid) noexcept override;
    void channel_abandoned() noexcept override;

    // Returns when a registered signal arrives, a channel is readable or
    // timeout_ms has passed (whichever is first; an interrupted wait
    // returns early). The caller then takes pending signals and serves.
    void wait(std::uint64_t timeout_ms) noexcept;

    // Serves every open channel: up to control_requests_per_round requests
    // each, one response per request. A channel that ends (end of file), or
    // that fails to receive or to take its response, is closed.
    void serve(stxcontrol::ControlServer& server, const stxcontrol::ControlFacts& facts, std::uint64_t now_ms,
               const stxconfig::ServiceRegistry& registry) noexcept;

    // Closes every channel whose process is no longer a running service.
    void sweep(const stxconfig::ServiceRegistry& registry) noexcept;

private:
    struct Channel {
        int descriptor;
        int pid;
    };

    void close_channel(std::size_t index) noexcept;
    void close_pending() noexcept;
    void report_request(const stxcontrol::ControlHandled& handled, int pid,
                        const stxconfig::ServiceRegistry& registry) noexcept;

    int console_;
    int queue_;
    Channel channels_[control_channels_max];
    std::size_t channel_count_;
    // Between prepare_channel and channel_started or channel_abandoned.
    int pending_parent_;
    int pending_child_;
    // One received packet: a request is 48 bytes; anything larger arrives
    // cut to this size and is refused as malformed.
    std::uint8_t request_[2u * stxcontrol::request_frame_bytes];
    std::uint8_t response_[stxcontrol::response_frame_bytes_max];
};

// The boot log on the user partition (boot_slots_system.h,
// boot_log_path), read for the control server.
class SystemControl final : public stxcontrol::ControlSystem {
public:
    SystemControl() noexcept = default;
    [[nodiscard]] stxcontrol::BootLogRead read_boot_log_end(std::span<char> destination) noexcept override;
};

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CHANNELS_H
