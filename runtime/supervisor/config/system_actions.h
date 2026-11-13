// system_actions.h: the init section's actions on FreeBSD (C1 step S2),
// and reading the configuration file.
//
// The only part of runtime/supervisor/config/ that issues system calls.
// Hosted on FreeBSD's libc for C1; it moves to STXlib's own runtime in C2.

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

#include "boot_config.h"
#include "init_executor.h"
#include "service_registry.h"

namespace stxconfig {

// A run that has not exited after run_timeout_ms is killed (SIGKILL); the
// executor then waits at most run_kill_wait_ms for it before reporting the
// time-out and leaving the child to the supervisor's reaper. Anything that
// should run longer is a service (S3), not an init command.
constexpr std::uint32_t run_timeout_ms = 10000;
constexpr std::uint32_t run_kill_wait_ms = 1000;
// How often wait-for and run look again.
constexpr std::uint32_t poll_interval_ms = 10;

// The configuration file's place until step S4 settles the partitions (the
// root location is the owner's decision); provisional.
constexpr const char* config_path = "/etc/stx/appliance.conf";

// ---------------------------------------------------------------------------
// Reading the file.

struct FileRead {
    std::size_t size;
};
struct FileAbsent {
};
struct FileTooLarge {
};
struct FileReadFailed {
    int error_number;
};

class FileOutcome {
public:
    [[nodiscard]] static FileOutcome read(std::size_t size) noexcept;
    [[nodiscard]] static FileOutcome absent() noexcept;
    [[nodiscard]] static FileOutcome too_large() noexcept;
    [[nodiscard]] static FileOutcome read_failed(int error_number) noexcept;

    [[nodiscard]] const FileRead* as_read() const noexcept;
    [[nodiscard]] bool is_absent() const noexcept;
    [[nodiscard]] bool is_too_large() const noexcept;
    [[nodiscard]] const FileReadFailed* as_read_failed() const noexcept;

private:
    using Alternatives = std::variant<FileRead, FileAbsent, FileTooLarge, FileReadFailed>;
    explicit FileOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Reads the whole regular file at path into buffer. A file larger than the
// buffer is refused (too_large) before anything is read; a file that grows
// while it is read is refused too. On anything but read(size), buffer's
// contents are unspecified.
[[nodiscard]] FileOutcome read_config_file(const char* path, std::span<char> buffer) noexcept;

// ---------------------------------------------------------------------------
// The actions.

// Prints one line per step on the console descriptor:
//
//   STXSUPERVISOR init line=<n> <keyword> <outcome>[ status=<n>| errno=<n>] ms=<n>
//
// plus, for a refused mount, the kernel's message:
//
//   STXSUPERVISOR init mount errmsg=<text>
class SystemActions final : public InitActions {
public:
    // start commands go to registry, which spawns through service_actions;
    // both are borrowed for the actions' lifetime.
    SystemActions(int console, ServiceRegistry& registry, ServiceActions& service_actions) noexcept;

    [[nodiscard]] bool is_present(PresenceKind kind, const char* path) noexcept override;
    [[nodiscard]] StepOutcome mount(FileSystemKind file_system, const char* device, const char* directory,
                                    MountAccess access) noexcept override;
    [[nodiscard]] StepOutcome wait_for(PresenceKind kind, const char* path, std::uint32_t timeout_ms) noexcept override;
    [[nodiscard]] StepOutcome start_service(const char* service, const char* const* environment) noexcept override;
    [[nodiscard]] StepOutcome run_program(const char* const* arguments,
                                          const char* const* environment) noexcept override;
    void report(const StepReport& step) noexcept override;

private:
    int console_;
    ServiceRegistry& registry_;
    ServiceActions& service_actions_;
    // When the current step began (the previous report, or construction).
    std::uint64_t step_started_ms_;
};

// Control channels for services whose definition grants control (C2;
// ../control_channels.h implements this). SystemServiceActions asks for a
// channel before it forks and says after the fork whose it is.
class ChannelGrants {
public:
    ChannelGrants() noexcept = default;
    ChannelGrants(const ChannelGrants&) = delete;
    ChannelGrants& operator=(const ChannelGrants&) = delete;

    // A new channel: the descriptor the child is to get as its descriptor
    // control_child_descriptor, or -1 when none can be made (the service
    // then runs without a channel). Close-on-exec in the supervisor.
    [[nodiscard]] virtual int prepare_channel() noexcept = 0;
    // After the fork: the prepared channel belongs to pid.
    virtual void channel_started(int pid) noexcept = 0;
    // The fork failed: the prepared channel is closed.
    virtual void channel_abandoned() noexcept = 0;

protected:
    ~ChannelGrants() = default;
};

// The descriptor a service finds its control channel on.
constexpr int control_child_descriptor = 3;

// Services on FreeBSD. spawn forks; the child starts a new session (so the
// console's job-control signals do not reach it), clears its signal mask,
// changes to the service's directory and executes the program. A service
// whose control access is not none gets its control channel as descriptor
// 3 (control_child_descriptor), or, when no channel could be made, has
// descriptor 3 closed. Console lines, one per event:
//
//   STXSUPERVISOR service <name> started pid=<n> restart_count=<n>
//   STXSUPERVISOR service <name> spawn-failed errno=<n>[ next_in_ms=<n>]
//   STXSUPERVISOR service <name> exited pid=<n> status=<n> ran_ms=<n>[ next_in_ms=<n>]
//   STXSUPERVISOR service <name> stopping pid=<n>
//   STXSUPERVISOR service <name> killed pid=<n>
class SystemServiceActions final : public ServiceActions {
public:
    explicit SystemServiceActions(int console) noexcept;

    [[nodiscard]] SpawnOutcome spawn(const char* const* arguments, const char* const* environment,
                                     const char* directory, ControlAccess control) noexcept override;
    void signal(int pid, ServiceSignal which) noexcept override;
    void report(const ServiceEvent& event) noexcept override;

    // Where channels come from; borrowed, must outlive these actions or be
    // reset to null first. Until set, no service gets a channel.
    void set_channel_grants(ChannelGrants* grants) noexcept;

private:
    int console_;
    ChannelGrants* grants_;
};

// CLOCK_MONOTONIC in milliseconds; 0 if the clock cannot be read.
[[nodiscard]] std::uint64_t monotonic_milliseconds() noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H
