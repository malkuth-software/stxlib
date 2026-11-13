// init_executor.cpp: runs a parsed init section (init_executor.h).

#include "init_executor.h"

#include <cassert>

namespace stxconfig {

// ---------------------------------------------------------------------------
// StepOutcome: factories and queries.

StepOutcome::StepOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

StepOutcome StepOutcome::done() noexcept {
    const StepOutcome outcome(Alternatives(std::in_place_type<StepDone>));
    return outcome;
}

StepOutcome StepOutcome::skipped() noexcept {
    const StepOutcome outcome(Alternatives(std::in_place_type<StepSkipped>));
    return outcome;
}

StepOutcome StepOutcome::exited(int wait_status) noexcept {
    StepExited state{};
    state.wait_status = wait_status;
    const StepOutcome outcome(Alternatives(std::in_place_type<StepExited>, state));
    return outcome;
}

StepOutcome StepOutcome::failed(int error_number) noexcept {
    StepFailed state{};
    state.error_number = error_number;
    const StepOutcome outcome(Alternatives(std::in_place_type<StepFailed>, state));
    return outcome;
}

StepOutcome StepOutcome::timed_out() noexcept {
    const StepOutcome outcome(Alternatives(std::in_place_type<StepTimedOut>));
    return outcome;
}

StepOutcome StepOutcome::unavailable() noexcept {
    const StepOutcome outcome(Alternatives(std::in_place_type<StepUnavailable>));
    return outcome;
}

StepOutcome StepOutcome::environment_full() noexcept {
    const StepOutcome outcome(Alternatives(std::in_place_type<StepEnvironmentFull>));
    return outcome;
}

bool StepOutcome::is_done() const noexcept {
    return std::holds_alternative<StepDone>(alternatives_);
}

bool StepOutcome::is_skipped() const noexcept {
    return std::holds_alternative<StepSkipped>(alternatives_);
}

const StepExited* StepOutcome::as_exited() const noexcept {
    return std::get_if<StepExited>(&alternatives_);
}

const StepFailed* StepOutcome::as_failed() const noexcept {
    return std::get_if<StepFailed>(&alternatives_);
}

bool StepOutcome::is_timed_out() const noexcept {
    return std::holds_alternative<StepTimedOut>(alternatives_);
}

bool StepOutcome::is_unavailable() const noexcept {
    return std::holds_alternative<StepUnavailable>(alternatives_);
}

bool StepOutcome::is_environment_full() const noexcept {
    return std::holds_alternative<StepEnvironmentFull>(alternatives_);
}

namespace {

// waitpid's encoding (FreeBSD sys/wait.h, and POSIX in practice): a normal
// exit has the low 7 bits zero and the exit status in bits 8-15. Spelled
// out here so that this file needs no operating-system header.
[[nodiscard]] bool exited_with_zero(int wait_status) noexcept {
    const unsigned int bits = static_cast<unsigned int>(wait_status);
    return (bits & 0x7fu) == 0u && ((bits >> 8u) & 0xffu) == 0u;
}

}  // namespace

bool StepOutcome::counts_as_failure() const noexcept {
    if (is_done() || is_skipped()) {
        return false;
    }
    const StepExited* const exited = as_exited();
    if (exited != nullptr) {
        return !exited_with_zero((*exited).wait_status);
    }
    return true;
}

const char* StepOutcome::name() const noexcept {
    if (is_done()) {
        return "ok";
    }
    if (is_skipped()) {
        return "skipped";
    }
    if (as_exited() != nullptr) {
        return "exited";
    }
    if (as_failed() != nullptr) {
        return "failed";
    }
    if (is_timed_out()) {
        return "timed-out";
    }
    if (is_unavailable()) {
        return "unavailable";
    }
    if (is_environment_full()) {
        return "environment-full";
    }
    // Fallback: a valueless variant (impossible: every alternative is
    // trivially copyable).
    return "unknown";
}

// ---------------------------------------------------------------------------
// InitWorkspace.

namespace {

[[nodiscard]] std::size_t c_string_length(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length;
}

// Whether entry's name is exactly name (name_length bytes).
[[nodiscard]] bool entry_has_name(const EnvironmentEntry& entry, const char* name, std::size_t name_length) noexcept {
    if (entry.name_length != name_length) {
        return false;
    }
    for (std::size_t index = 0; index < name_length; index += 1) {
        if (entry.text[index] != name[index]) {
            return false;
        }
    }
    return true;
}

// Writes "name=value\0" into entry. Precondition: it fits (checked by the
// caller).
void fill_entry(EnvironmentEntry& entry, const char* name, std::size_t name_length, const char* value,
                std::size_t value_length) noexcept {
    assert(name_length + 1u + value_length + 1u <= environment_entry_bytes);
    std::size_t at = 0;
    for (std::size_t index = 0; index < name_length; index += 1) {
        entry.text[at] = name[index];
        at += 1;
    }
    entry.text[at] = '=';
    at += 1;
    for (std::size_t index = 0; index < value_length; index += 1) {
        entry.text[at] = value[index];
        at += 1;
    }
    entry.text[at] = '\0';
    entry.name_length = name_length;
}

}  // namespace

InitWorkspace::InitWorkspace() noexcept : entries_{}, entry_count_(0), vector_{} {
    reset();
}

void InitWorkspace::reset() noexcept {
    entry_count_ = 0;
    // The base entries always fit: two short names and values.
    const bool path_set = set("PATH", base_environment_path);
    const bool home_set = set("HOME", base_environment_home);
    assert(path_set && home_set);
    (void)path_set;
    (void)home_set;
}

bool InitWorkspace::set(const char* name, const char* value) noexcept {
    const std::size_t name_length = c_string_length(name);
    const std::size_t value_length = c_string_length(value);
    if (name_length == 0u || name_length > name_length_max || value_length > line_length_max) {
        return false;
    }
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        if (entry_has_name(entries_[index], name, name_length)) {
            fill_entry(entries_[index], name, name_length, value, value_length);
            return true;
        }
    }
    if (entry_count_ >= environment_entries_max) {
        return false;
    }
    fill_entry(entries_[entry_count_], name, name_length, value, value_length);
    entry_count_ += 1;
    return true;
}

std::size_t InitWorkspace::entry_count() const noexcept {
    return entry_count_;
}

const char* const* InitWorkspace::environment() noexcept {
    for (std::size_t index = 0; index < entry_count_; index += 1) {
        vector_[index] = entries_[index].text;
    }
    vector_[entry_count_] = nullptr;
    return vector_;
}

// ---------------------------------------------------------------------------
// The executor.

namespace {

[[nodiscard]] StepOutcome perform_run(const BootConfig& config, const RunCommand& command, InitActions& actions,
                                      InitWorkspace& workspace) noexcept {
    // argument_count is 1..run_arguments_max (the parser's guarantee).
    assert(command.argument_count >= 1u && command.argument_count <= run_arguments_max);
    const char* arguments[run_arguments_max + 1u];
    for (std::uint32_t index = 0; index < command.argument_count; index += 1) {
        arguments[index] = config.text(command.arguments[index]);
    }
    arguments[command.argument_count] = nullptr;
    return actions.run_program(arguments, workspace.environment());
}

[[nodiscard]] StepOutcome perform_action(const BootConfig& config, const CommandAction& action, InitActions& actions,
                                         InitWorkspace& workspace) noexcept {
    const MountCommand* const mount = action.as_mount();
    if (mount != nullptr) {
        return actions.mount((*mount).file_system, config.text((*mount).device), config.text((*mount).directory),
                             (*mount).access);
    }
    const WaitForCommand* const wait_for = action.as_wait_for();
    if (wait_for != nullptr) {
        return actions.wait_for((*wait_for).kind, config.text((*wait_for).path), (*wait_for).timeout_ms);
    }
    const SetCommand* const set = action.as_set();
    if (set != nullptr) {
        const bool stored = workspace.set(config.text((*set).name), config.text((*set).value));
        return stored ? StepOutcome::done() : StepOutcome::environment_full();
    }
    const StartCommand* const start = action.as_start();
    if (start != nullptr) {
        return actions.start_service(config.text((*start).service), workspace.environment());
    }
    const RunCommand* const run = action.as_run();
    if (run != nullptr) {
        return perform_run(config, *run, actions, workspace);
    }
    // Fallback: an action of no known kind is reported as unavailable and
    // the init section goes on.
    return StepOutcome::unavailable();
}

}  // namespace

InitSummary run_init_section(const BootConfig& config, InitActions& actions, InitWorkspace& workspace) noexcept {
    workspace.reset();
    InitSummary summary{};
    summary.commands = 0;
    summary.skipped = 0;
    summary.failed = 0;
    const std::size_t count = config.init_command_count();
    for (std::size_t index = 0; index < count; index += 1) {
        const InitCommand& command = config.init_command(index);
        bool runs = true;
        if (command.guard.has_value()) {
            runs = actions.is_present((*command.guard).kind, config.text((*command.guard).path));
        }
        const StepOutcome outcome =
            runs ? perform_action(config, command.action, actions, workspace) : StepOutcome::skipped();
        summary.commands += 1;
        if (outcome.is_skipped()) {
            summary.skipped += 1;
        }
        if (outcome.counts_as_failure()) {
            summary.failed += 1;
        }
        const StepReport report{command.line, command.action.keyword(), outcome};
        actions.report(report);
    }
    return summary;
}

}  // namespace stxconfig
