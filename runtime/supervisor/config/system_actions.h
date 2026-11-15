// system_actions.h: the init section's actions on FreeBSD (C1 step S2),
// and reading the configuration file.
//
// The only part of runtime/supervisor/config/ that issues system calls.
// Hosted on FreeBSD's libc (the own runtime is after 1.0: owner, D5 and
// D6, 2026-11-15).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H

#include <cstddef>
#include <cstdint>
#include <optional>
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
    // Reads the variable's kind and format from the kernel (the OIDFMT
    // query), lets parse_sysctl_value decide what to write, and writes it.
    // A refusal prints "STXSUPERVISOR init sysctl refused=<reason>" and
    // fails with EINVAL; an unknown name fails with ENOENT.
    [[nodiscard]] StepOutcome set_sysctl(const char* name, const char* value) noexcept override;
    void report(const StepReport& step) noexcept override;

private:
    int console_;
    ServiceRegistry& registry_;
    ServiceActions& service_actions_;
    // When the current step began (the previous report, or construction).
    std::uint64_t step_started_ms_;
};

// The supervisor's target layer as SystemServiceActions sees it
// (../targets.h implements it). Borrowed; set before any target starts.
class TargetHooks {
public:
    TargetHooks() noexcept = default;
    TargetHooks(const TargetHooks&) = delete;
    TargetHooks& operator=(const TargetHooks&) = delete;

    // A target is about to start: the slot ('a' or 'b') of its program to
    // run, or nothing when it has none to run (no program installed, or the
    // store's state cannot be read); the start then fails with ENOENT.
    [[nodiscard]] virtual std::optional<char> slot_to_run(const SpawnRequest& request) noexcept = 0;
    // The target was started on that slot as pid.
    virtual void target_started(std::size_t service_index, char slot, int pid) noexcept = 0;
    // Every event the registry reports, after its console line.
    virtual void service_event(const ServiceEvent& event) noexcept = 0;

protected:
    ~TargetHooks() = default;
};

// The program that starts a target (runtime/package/stxpkg.cpp, "run"): it
// verifies the package in the slot, then executes the file.
constexpr const char* target_launcher_path = "/sbin/stxpkg";
// Launcher words before the target's own: path, "run", program, slot, "--".
constexpr std::size_t launcher_words = 5;

// Services on FreeBSD. spawn forks; the child starts a new session (so the
// console's job-control signals do not reach it), clears its signal mask,
// changes to the service's directory and executes the program. A target is
// different in three ways: its folder is created first if absent (owned
// by the target's identity, mode 0700; its parents below
// target_folder_parent root-owned, mode 0755); the child takes the target's
// uid and gid (setgroups, setgid, setuid) before anything else; and it
// executes the launcher:
//   /sbin/stxpkg run <program> <slot> -- <file> [argument...]
// Console lines, one per event:
//
//   STXSUPERVISOR service <name> started pid=<n> restart_count=<n>
//   STXSUPERVISOR service <name> spawn-failed errno=<n>[ next_in_ms=<n>]
//   STXSUPERVISOR service <name> exited pid=<n> status=<n> ran_ms=<n>[ next_in_ms=<n>]
//   STXSUPERVISOR service <name> stopping pid=<n>
//   STXSUPERVISOR service <name> killed pid=<n>
class SystemServiceActions final : public ServiceActions {
public:
    explicit SystemServiceActions(int console) noexcept;

    [[nodiscard]] SpawnOutcome spawn(const SpawnRequest& request) noexcept override;
    void signal(int pid, ServiceSignal which) noexcept override;
    void report(const ServiceEvent& event) noexcept override;

    // Where targets' slots come from and where events go; borrowed, must
    // outlive these actions or be reset to null first. Until set, every
    // target start fails with ENOENT.
    void set_target_hooks(TargetHooks* hooks) noexcept;

private:
    [[nodiscard]] SpawnOutcome spawn_target(const SpawnRequest& request, const SpawnTarget& target) noexcept;

    int console_;
    TargetHooks* target_hooks_;
};

// CLOCK_MONOTONIC in milliseconds; 0 if the clock cannot be read.
[[nodiscard]] std::uint64_t monotonic_milliseconds() noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_SYSTEM_ACTIONS_H
