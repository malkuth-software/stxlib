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
// open, fstat, pread, and the turn files' (store/turn_file_system.h).
// Hosted on FreeBSD's libc for C2's first form.
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
#include "../store/turn_file.h"
#include "control/control_protocol.h"
#include "control/control_server.h"
#include "config/target_ids.h"
#include "targets.h"

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

    // Adds descriptors (the targets' output pipes, target_log.h) to what
    // wait() wakes for, as readable. Adding one already there changes
    // nothing; a descriptor leaves the queue when it is closed. At most
    // control_channels_max per call; the rest are left out.
    void watch(std::span<const int> descriptors) noexcept;

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
    // One received packet: a request is at most request_frame_bytes_max
    // bytes; a larger one arrives cut to this size (SOCK_SEQPACKET
    // discards the rest) and is refused as malformed.
    std::uint8_t request_[stxcontrol::request_frame_bytes_max + 1u];
    std::uint8_t response_[stxcontrol::response_frame_bytes_max];
};

// The control server's system: the boot log on the user partition
// (boot_slots_system.h, boot_log_path), the user configuration's turn
// files (config/user_config.h), the reboot request, and the program and
// system-update commands and each target's output, which the targets layer
// carries out (targets.h; answers in program_answers.h). Until set_targets
// those answer unavailable (the log tail: absent). Large (two turn-file
// buffers): static storage.
class SystemControl final : public stxcontrol::ControlSystem {
public:
    SystemControl() noexcept;
    [[nodiscard]] stxcontrol::BootLogRead read_boot_log_end(std::span<char> destination) noexcept override;
    [[nodiscard]] stxcontrol::UserConfigRead read_user_config(std::span<char> destination) noexcept override;
    [[nodiscard]] stxcontrol::IdsIssued issue_target_ids(const stxconfig::BootConfig& user,
                                                         const stxconfig::BootConfig& factory,
                                                         std::span<const char> text,
                                                         std::span<char> out) noexcept override;
    [[nodiscard]] stxcontrol::Status write_user_config(std::span<const char> text,
                                                       stxcontrol::ConfigWritten& written) noexcept override;
    void request_reboot() noexcept override;
    [[nodiscard]] stxcontrol::ProgramAnswer program_state(const stxcontrol::ServiceName& target) noexcept override;
    [[nodiscard]] stxcontrol::ProgramAnswer program_install(const stxcontrol::ServiceName& target,
                                                            const stxcontrol::InstallSource& source) noexcept override;
    [[nodiscard]] stxcontrol::ProgramAnswer program_rollback(const stxcontrol::ServiceName& target) noexcept override;
    [[nodiscard]] stxcontrol::ProgramAnswer program_check(const stxcontrol::ServiceName& target) noexcept override;
    [[nodiscard]] stxcontrol::Status system_update() noexcept override;
    [[nodiscard]] stxcontrol::OperationRecord operation_state() noexcept override;
    [[nodiscard]] stxcontrol::BootLogRead read_target_log(const stxcontrol::ServiceName& target,
                                                          std::span<char> destination) noexcept override;

    // Whether a reboot was asked for (the main loop reboots).
    [[nodiscard]] bool reboot_requested() const noexcept;
    // The targets layer the program commands go to; borrowed, must outlive
    // this system or be reset to null first.
    void set_targets(stxtargets::Targets* targets) noexcept;
    // At boot, after the init section (/data mounted), before the targets
    // start: adopts the running configuration's target ids into the ledger
    // and removes the folders of deleted targets (config/target_ids.h).
    void boot_target_ids(const stxconfig::BootConfig& running, int console) noexcept;

private:
    // status, with the target's ProgramRecord when it is a target.
    [[nodiscard]] stxcontrol::ProgramAnswer answer_for(const stxcontrol::ServiceName& target,
                                                       stxcontrol::Status status) const noexcept;

    bool reboot_requested_;
    stxtargets::Targets* targets_;
    // The target id ledger and its text (config/target_ids.h).
    stxconfig::TargetIdLedger ledger_;
    char ledger_text_[stxconfig::target_id_ledger_text_max];
    char turn_buffers_[2][stxstore::turn_file_bytes_max + 1u];
};

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONTROL_CHANNELS_H
