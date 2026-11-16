// init_executor.h: runs a parsed init section (boot_config.h) once, in
// order (C1 step S2).
//
// Pure code like the parser: the executor decides, an InitActions object
// acts. The supervisor passes SystemActions (system_actions.h, FreeBSD
// system calls); host tests pass a recorder that scripts each action's
// outcome.
//
// Termination: the executor makes one pass over the commands, n steps for n
// commands, with no jumps, retries or loops other than bounded table
// searches. Each step makes at most two calls on InitActions (a presence
// test, then the action), and each InitActions call is bounded in time by
// its implementation's own limit (SystemActions: wait-for by its timeout,
// at most wait_ms_max; run by run_timeout_ms plus the kill wait).
//
// Semantics:
// - A guarded command whose path is absent is skipped.
// - A failed command is reported and the next one runs: one bad line does
//   not stop the boot. The summary counts failures.
// - set records NAME=VALUE in the workspace's environment, replacing an
//   earlier value of NAME; each later run gets the whole environment. The
//   environment starts with PATH and HOME (base_environment below).
// - start asks the services registry (service_registry.h) to start a
//   service the file defines, with the environment as it is at that line.
// - sysctl sets a kernel variable: the system side reads the variable's
//   kind and writes the value sysctl_value.h makes of the text (C2).

#ifndef STXLIB_RUNTIME_SUPERVISOR_CONFIG_INIT_EXECUTOR_H
#define STXLIB_RUNTIME_SUPERVISOR_CONFIG_INIT_EXECUTOR_H

#include <cstddef>
#include <cstdint>
#include <variant>

#include "boot_config.h"

namespace stxconfig {

// ---------------------------------------------------------------------------
// Outcomes.

struct StepDone {
};
struct StepSkipped {
};
// A run's program ended: status as waitpid reports it.
struct StepExited {
    int wait_status;
};
struct StepFailed {
    int error_number;
};
struct StepTimedOut {
};
// The action has no implementation (kept for actions added before their
// system side; none at present).
struct StepUnavailable {
};
// set found the environment full (environment_entries_max names).
struct StepEnvironmentFull {
};

class StepOutcome {
public:
    [[nodiscard]] static StepOutcome done() noexcept;
    [[nodiscard]] static StepOutcome skipped() noexcept;
    [[nodiscard]] static StepOutcome exited(int wait_status) noexcept;
    [[nodiscard]] static StepOutcome failed(int error_number) noexcept;
    [[nodiscard]] static StepOutcome timed_out() noexcept;
    [[nodiscard]] static StepOutcome unavailable() noexcept;
    [[nodiscard]] static StepOutcome environment_full() noexcept;

    [[nodiscard]] bool is_done() const noexcept;
    [[nodiscard]] bool is_skipped() const noexcept;
    [[nodiscard]] const StepExited* as_exited() const noexcept;
    [[nodiscard]] const StepFailed* as_failed() const noexcept;
    [[nodiscard]] bool is_timed_out() const noexcept;
    [[nodiscard]] bool is_unavailable() const noexcept;
    [[nodiscard]] bool is_environment_full() const noexcept;

    // Whether the step counts as a failure in the summary: anything but
    // done, skipped, or a program that exited normally with status 0.
    [[nodiscard]] bool counts_as_failure() const noexcept;

    // The outcome's name as printed ("ok", "skipped", "exited", "failed",
    // "timed-out", "unavailable", "environment-full").
    [[nodiscard]] const char* name() const noexcept;

private:
    using Alternatives = std::variant<StepDone, StepSkipped, StepExited, StepFailed, StepTimedOut, StepUnavailable,
                                      StepEnvironmentFull>;
    explicit StepOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

struct StepReport {
    std::uint32_t line;
    // The command's keyword ("mount", ...), a string literal.
    const char* keyword;
    StepOutcome outcome;
};

// ---------------------------------------------------------------------------
// What the executor asks of the system. Every string argument is
// NUL-terminated and borrowed for the call only.

class InitActions {
public:
    InitActions() noexcept = default;
    InitActions(const InitActions&) = delete;
    InitActions& operator=(const InitActions&) = delete;

    [[nodiscard]] virtual bool is_present(PresenceKind kind, const char* path) noexcept = 0;
    [[nodiscard]] virtual StepOutcome mount(FileSystemKind file_system, const char* device, const char* directory,
                                            MountAccess access) noexcept = 0;
    [[nodiscard]] virtual StepOutcome wait_for(PresenceKind kind, const char* path, std::uint32_t timeout_ms) noexcept = 0;
    // Starts the named service with the init section's environment so far
    // (a null-terminated vector). Returns done (started, or already
    // running) or failed.
    [[nodiscard]] virtual StepOutcome start_service(const char* service,
                                                    const char* const* environment) noexcept = 0;
    // arguments and environment are null-terminated vectors; arguments[0]
    // is the program's absolute path. Returns exited, failed (it could not
    // be started) or timed_out.
    [[nodiscard]] virtual StepOutcome run_program(const char* const* arguments,
                                                  const char* const* environment) noexcept = 0;
    // Sets the kernel variable name to value (sysctl_value.h says how the
    // text is read). Returns done or failed.
    [[nodiscard]] virtual StepOutcome set_sysctl(const char* name, const char* value) noexcept = 0;
    // Called once per step, after it, in order.
    virtual void report(const StepReport& step) noexcept = 0;

protected:
    // Not deleted through this interface: the owner holds the concrete type.
    ~InitActions() = default;
};

// ---------------------------------------------------------------------------
// The executor's own storage: the environment that set builds. The caller
// owns one (about 10 KiB) and passes it to run_init_section.

constexpr std::size_t environment_entries_max = 32;
// "NAME=VALUE" and its NUL: a name of at most name_length_max bytes and a
// value no longer than a line.
constexpr std::size_t environment_entry_bytes = name_length_max + 1u + line_length_max + 1u;

struct EnvironmentEntry {
    char text[environment_entry_bytes];
    // Bytes before the '='.
    std::size_t name_length;
};

class InitWorkspace {
public:
    InitWorkspace() noexcept;
    InitWorkspace(const InitWorkspace&) = delete;
    InitWorkspace& operator=(const InitWorkspace&) = delete;

    // Back to the base environment only.
    void reset() noexcept;

    // Sets name to value, replacing an earlier value. False, with nothing
    // changed, when the name is new and the table is full, or when the
    // entry would not fit (impossible for parsed names and values; checked
    // anyway).
    [[nodiscard]] bool set(const char* name, const char* value) noexcept;

    [[nodiscard]] std::size_t entry_count() const noexcept;
    // A null-terminated vector of the entries' texts, valid until the next
    // set or reset.
    [[nodiscard]] const char* const* environment() noexcept;

private:
    EnvironmentEntry entries_[environment_entries_max];
    std::size_t entry_count_;
    const char* vector_[environment_entries_max + 1u];
};

// The environment every run starts from.
constexpr const char* base_environment_path = "/sbin:/bin:/usr/sbin:/usr/bin";
constexpr const char* base_environment_home = "/";

struct InitSummary {
    std::uint32_t commands;
    std::uint32_t skipped;
    std::uint32_t failed;
};

// Runs config's init section once, in order. workspace is reset first.
// Reports every step through actions.report.
[[nodiscard]] InitSummary run_init_section(const BootConfig& config, InitActions& actions,
                                           InitWorkspace& workspace) noexcept;

}  // namespace stxconfig

#endif  // STXLIB_RUNTIME_SUPERVISOR_CONFIG_INIT_EXECUTOR_H
