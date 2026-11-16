// targets.h: the supervisor's targets layer (C2, appliance/C2_DESIGN.md,
// "Targets", "Programs" and "Updates").
//
// Targets are the registered programs the appliance exists to run
// ([target-<name>] sections, boot_config.h). The registry starts, stops and
// restarts them like services; this layer adds what a target has beyond a
// service:
// - which slot of its program runs (program_slots.h): it answers the
//   system actions' slot_to_run before every start, and moves the slot
//   state on every exit (a trial's early exits and refusals fall back, a
//   trial that runs healthy_after is kept);
// - the update service: per target with an update-url, a check
//   update_first_delay_ms after the supervisor starts and then every
//   update-interval, as one "stxpkg update" job at a time; an installed
//   update goes on trial at once (the target is restarted);
// - the system update service: [system] update-url and update-interval, as
//   "stxpkg system-update <inactive slot>"; when it succeeds the slot is
//   armed (boot_slots_system.h) and the unit reboots. It does not start
//   while a running target is on trial: the reboot would end the trial
//   before it could be kept;
// - the requests the web configuration (sc2) makes through the
//   supervisor's control channel: update now, install a package file,
//   roll back, status.
// The supervisor is the only writer of every program's state file.
//
// Console lines:
//
//   STXSUPERVISOR target <name> slot=<a|b> note=<slot note> [refused_version=<n>]
//   STXSUPERVISOR target <name> job=<update|install> pid=<n> | outcome=<o> next_check_s=<n>
//   STXSUPERVISOR target <name> state unreadable | write-failed errno=<n>
//   STXSUPERVISOR system job=update pid=<n> | outcome=<o> | armed slot=<X> | arm-failed

#ifndef STXLIB_RUNTIME_SUPERVISOR_TARGETS_H
#define STXLIB_RUNTIME_SUPERVISOR_TARGETS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "../package/program_slots.h"
#include "boot_slots.h"
#include "config/boot_config.h"
#include "config/service_registry.h"
#include "config/system_actions.h"
#include "target_log.h"
#include "targets_plan.h"

namespace stxtargets {

enum class RequestResult : std::uint8_t {
    accepted,        // the job started, or the change was made
    unknown_target,
    busy,            // another job runs; ask again later
    nothing_to_do,   // rollback with nothing to roll back to; update without update-url
    failed,          // the job could not be started, or the state not written
};

[[nodiscard]] const char* request_result_name(RequestResult result) noexcept;

// The long operation running or last finished (sc2's operation_state,
// control_protocol.h): one job at a time, so there is at most one.
enum class OperationKind : std::uint8_t {
    none,            // nothing since the supervisor started
    system_update,   // system-update or system-install
    program_install, // install of a staged package, or update from a URL asked for
    program_check,   // an update check (asked for, or the update service's own)
};

struct OperationStatus {
    OperationKind kind;
    // An install asked for (a staged package or image, or a URL), as
    // against a look for something newer: "not newer" and "nothing newer"
    // end an install as a failure and a look as a success
    // (program_answers.h, job_status).
    bool install;
    bool running;
    // waitpid's status when finished; 0 while running.
    int wait_status;
    std::uint64_t started_ms;
    // When it finished; started_ms while running.
    std::uint64_t ended_ms;
    // The target's name for the program kinds, else null. Borrowed from the
    // configuration.
    const char* target;
};

struct TargetStatus {
    std::size_t service_index;
    // Nothing when the program's state file is unreadable or malformed.
    std::optional<stxpackage::StoredState> stored;
    // The slot the running process was started on, if one runs.
    std::optional<stxpackage::Slot> running_slot;
    std::uint32_t version_a;
    std::uint32_t version_b;
    bool job_running;
    // When the update service looks next (monotonic ms), if it will.
    std::optional<std::uint64_t> next_check_ms;
};

class Targets final : public stxconfig::TargetHooks {
public:
    Targets(int console, stxconfig::ServiceRegistry& registry, stxconfig::ServiceActions& actions) noexcept;

    // Reads every target's program state and schedules the update checks.
    // config is borrowed, as the registry borrows it.
    void load(const stxconfig::BootConfig& config, std::optional<stxboot::Slot> running_system_slot,
              std::uint64_t now_ms) noexcept;
    // Starts the targets whose start policy is boot, the foreground target
    // first (after the init section and the device rules).
    void start_boot_targets(std::uint64_t now_ms) noexcept;

    // TargetHooks.
    [[nodiscard]] std::optional<char> slot_to_run(const stxconfig::SpawnRequest& request) noexcept override;
    [[nodiscard]] int prepare_output(std::size_t service_index) noexcept override;
    void output_abandoned(std::size_t service_index) noexcept override;
    void target_started(std::size_t service_index, char slot, int pid) noexcept override;
    void service_event(const stxconfig::ServiceEvent& event) noexcept override;

    // A child exited: true when it was this layer's job.
    [[nodiscard]] bool child_exited(int pid, int wait_status, std::uint64_t now_ms) noexcept;
    // Health checks, pending restarts, due update checks.
    void tick(std::uint64_t now_ms) noexcept;
    [[nodiscard]] std::optional<std::uint64_t> next_due_ms() const noexcept;
    // Whether a system update was installed and armed: the supervisor then
    // asks stxinit to reboot.
    [[nodiscard]] bool reboot_requested() const noexcept;

    // The control channel's requests (sc2, appliance/C2_DESIGN.md, "The
    // supervisor interface").
    [[nodiscard]] RequestResult request_update(const char* name, std::uint64_t now_ms) noexcept;
    [[nodiscard]] RequestResult request_install(const char* name, const char* package_path,
                                                std::uint64_t now_ms) noexcept;
    [[nodiscard]] RequestResult rollback(const char* name, std::uint64_t now_ms) noexcept;
    // An update from a URL the request names (sc2's program_install with an
    // https source): https://, at most url_length_max bytes (stxpkg
    // validates it fully, runtime/net/url.h); nothing_to_do otherwise.
    [[nodiscard]] RequestResult request_update_from(const char* name, const char* url, std::uint64_t now_ms) noexcept;
    // The system image: an update check now ([system] update-url), or the
    // install of a staged system package (sc2's system_update from
    // /user/stx/staging/system.img). Either, when it installs, arms the
    // inactive slot and asks for the reboot, as the update service does.
    [[nodiscard]] RequestResult request_system_update(std::uint64_t now_ms) noexcept;
    [[nodiscard]] RequestResult request_system_install(const char* package_path, std::uint64_t now_ms) noexcept;
    [[nodiscard]] std::optional<TargetStatus> status(const char* name) const noexcept;
    [[nodiscard]] OperationStatus last_operation() const noexcept;
    // The per-target output (K17, target_log.h): the end of name's ring, as
    // TargetLogs::read_end. Nothing for an unknown target.
    [[nodiscard]] std::optional<TargetLogEnd> log_end(const char* name, std::span<char> destination) const noexcept;
    // Reads every target's output that is waiting (non-blocking); the main
    // loop calls it each round.
    void drain_logs() noexcept;
    // The output pipes' read ends, for the main loop's wait (-1 for none).
    [[nodiscard]] std::size_t log_descriptors(std::span<int> out) const noexcept;
    [[nodiscard]] std::size_t target_count() const noexcept;
    // The name of the index-th target (index below target_count).
    [[nodiscard]] const char* target_name(std::size_t index) const noexcept;

private:
    struct Running {
        stxpackage::Slot slot;
        int pid;
        std::uint64_t started_ms;
    };
    struct Entry {
        std::size_t service_index;
        // Nothing when the state file is unreadable or malformed: the
        // target then has no slot to run until an install or a rollback
        // writes a state the supervisor knows.
        std::optional<stxpackage::StoredState> stored;
        std::optional<Running> running;
        bool restart_pending;
        std::optional<std::uint64_t> next_check_ms;
    };
    enum class JobKind : std::uint8_t {
        update,
        install,
        system_update,
    };
    struct Job {
        JobKind kind;
        int pid;
        // The target's entry index (not for system_update).
        std::size_t entry;
    };

    [[nodiscard]] std::optional<std::size_t> entry_of_service(std::size_t service_index) const noexcept;
    [[nodiscard]] std::optional<std::size_t> entry_of_name(const char* name) const noexcept;
    [[nodiscard]] const stxconfig::TargetSettings& settings(const Entry& entry) const noexcept;
    // Whether a running target is on trial (its update not yet kept): the
    // system update waits, since its reboot would end the trial.
    [[nodiscard]] bool trial_running() const noexcept;
    [[nodiscard]] const char* program(const Entry& entry) const noexcept;
    [[nodiscard]] const char* name(const Entry& entry) const noexcept;
    void apply(Entry& entry, const stxpackage::SlotTransition& transition) noexcept;
    [[nodiscard]] bool store(Entry& entry) noexcept;
    [[nodiscard]] bool start_job(JobKind kind, std::size_t entry_index, const char* const* arguments,
                                 OperationKind operation, bool install, std::uint64_t now_ms) noexcept;
    void start_update(std::size_t entry_index, std::uint64_t now_ms) noexcept;
    void start_system_update(std::uint64_t now_ms) noexcept;
    void finish_job(const Job& job, int wait_status, std::uint64_t now_ms) noexcept;

    int console_;
    stxconfig::ServiceRegistry& registry_;
    stxconfig::ServiceActions& actions_;
    const stxconfig::BootConfig* config_;
    Entry entries_[stxconfig::services_max];
    std::size_t entry_count_;
    std::optional<Job> job_;
    std::optional<stxboot::Slot> system_slot_;
    std::optional<std::uint64_t> system_next_check_ms_;
    std::uint32_t system_interval_s_;
    bool reboot_requested_;
    OperationStatus last_operation_;
    TargetLogs logs_;
};

}  // namespace stxtargets

#endif  // STXLIB_RUNTIME_SUPERVISOR_TARGETS_H
