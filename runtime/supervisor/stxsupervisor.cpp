// stxsupervisor: the appliance supervisor, first form (C1, steps S1-S3, S5).
//
// It writes one marker and registers with the kernel shim (shim_client.h;
// a kernel without the shim leaves it unregistered and otherwise
// unchanged); on the boot's first start (restart count 0) it
// reads the boot configuration and runs its init section once
// (config/boot_config.h, config/init_executor.h; the init section is the
// supervisor's, not PID 1's: coordinator, 2026-11-13, option a); then it
// stays up reaping children and stops cleanly on SIGTERM, which is how
// stxinit's shutdown ends it. A restarted supervisor does not run the init
// section again: its mounts and programs belong to the boot, not to the
// supervisor's life. Services the configuration defines are held by the
// registry (config/service_registry.h): started by the init section's start
// commands, restarted by their policy, stopped on SIGTERM before the
// supervisor stops. In C2 the supervisor moves to STXlib's own runtime and
// then becomes a GUI.Forms application.
//
// Known limit of the first form: a restarted supervisor does not adopt the
// services its predecessor started. They run on (each leads its own
// session), reparented to stxinit, which reaps them; nothing restarts them.
// Adoption, or their termination by the shim, is C2 work.
//
// SIGUSR1 asks the shim to terminate userspace (owner, question 3, option 1;
// shim_client.h). It is the first form's trigger, for tests: in C2 the
// reserved chord, read by the shim below the console, starts the terminate
// and the supervisor's own interface offers it. The supervisor waits for
// the shim (about 2 s at most plus the sync), then reaps the terminated
// services and restarts them by their registration.
//
// Not yet in the first form (C2, with the contention measurements): real-
// time priority and locked memory for the supervisor (C-appliance-design.md
// section 3). Services would inherit the priority class across fork(2) and
// would need it reset before execve(2); locked memory makes fork(2) copy
// the wired pages at once. Both are measured before they are adopted.
//
// A/B root slots and the failsafe path (S4, appliance/disk/LAYOUT.md): the
// supervisor reads which slot the loader booted, takes the failsafe path
// (/etc/stx/failsafe.conf, then the reason in /user/stx/boot.log) when the
// configuration is absent, unreadable or refused or at the third restart,
// and marks the slot good once the good-boot rule holds (60 s after the
// init section, registered, services healthy, network up).
//
// stxinit starts it in its own session with the console as standard input,
// output and error, and passes "--restart-count=<n>".
//
// Console lines, matched by appliance/vm/scenario.py:
//
//   STXSUPERVISOR start restart_count=<n|unknown> uptime_ns=<n> cntvct=<n> cntfrq=<n>
//   STXSUPERVISOR shim registered | absent | refused errno=<n>
//   STXSUPERVISOR boot slot=... | good ... | not-yet-good ... (boot_slots_system.h)
//   STXSUPERVISOR failsafe reason=<config-absent|config-refused|config-unreadable|restart-limit>
//   STXSUPERVISOR config path=<p> sections=<n> settings=<n> commands=<n>
//   STXSUPERVISOR config path=<p> absent | too-large | errno=<n>
//   STXSUPERVISOR config path=<p> refused line=<n> column=<n> error=<name>
//   STXSUPERVISOR init line=<n> <keyword> <outcome> ... ms=<n>   (one per command)
//   STXSUPERVISOR init done commands=<n> skipped=<n> failed=<n> ms=<n>
//   STXSUPERVISOR init not-run restart_count=<n|unknown>
//   STXSUPERVISOR service <name> started | exited | ...   (system_actions.h)
//   STXSUPERVISOR terminate done ... | failed errno=<n> | unavailable   (shim_client.h)
//   STXSUPERVISOR stop services=<stopped|left-running>
//
// Signals stay blocked and are taken with sigwaitinfo, as in stxinit.
// SIGCHLD has a do-nothing handler so that it is not discarded while
// blocked.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/console_line.h"
#include "boot_slots.h"
#include "boot_slots_system.h"
#include "config/boot_config.h"
#include "config/init_executor.h"
#include "config/service_registry.h"
#include "config/system_actions.h"
#include "restart_argument.h"
#include "shim_client.h"

namespace stxsupervisor {

using stxruntime::LineText;

extern "C" void note_signal(int signal_number) noexcept {
    (void)signal_number;
}


// ---------------------------------------------------------------------------
// The boot configuration and its init section. The storage is static: the
// file's bytes, the parsed configuration and the executor's environment
// together are about 60 KiB, too much for a stack frame, and the supervisor
// needs exactly one of each.

char config_text[stxconfig::config_size_max];
stxconfig::BootConfig boot_config;
stxconfig::InitWorkspace init_workspace;
// Borrows boot_config, which is static too.
stxconfig::ServiceRegistry service_registry;
stxconfig::SystemServiceActions service_actions(STDOUT_FILENO);

// This boot, as the loader and the supervisor's own start saw it (S4).
stxboot::BootFacts boot_facts_now{};
bool shim_registered = false;
bool init_done = false;
std::uint64_t init_done_ms = 0;
bool failsafe_taken = false;
bool boot_marked_good = false;
bool boot_not_good_reported = false;
// When the good-boot rule is next evaluated; none when it is not pending.
std::optional<std::uint64_t> good_check_due_ms;
// After the first check at 60 s, how often to look again.
constexpr std::uint64_t good_recheck_ms = 10000;

void begin_config_line(LineText& line, const char* path) noexcept {
    stxruntime::append_text(line, "STXSUPERVISOR config path=");
    stxruntime::append_text(line, path);
}

// Reads and parses a configuration into boot_config.
[[nodiscard]] stxboot::ConfigState load_config(int console, const char* path) noexcept {
    const std::span<char> buffer(config_text, sizeof(config_text));
    const stxconfig::FileOutcome file = stxconfig::read_config_file(path, buffer);
    LineText line = stxruntime::empty_line();
    begin_config_line(line, path);
    const stxconfig::FileRead* const read = file.as_read();
    if (read == nullptr) {
        const stxconfig::FileReadFailed* const failed = file.as_read_failed();
        if (file.is_absent()) {
            stxruntime::append_text(line, " absent\n");
        } else if (file.is_too_large()) {
            stxruntime::append_text(line, " too-large\n");
        } else if (failed != nullptr) {
            stxruntime::append_text(line, " errno=");
            stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>((*failed).error_number)));
            stxruntime::append_text(line, "\n");
        } else {
            // Fallback: a FileOutcome in no known state.
            stxruntime::append_text(line, " unreadable\n");
        }
        stxruntime::write_line(console, line);
        return file.is_absent() ? stxboot::ConfigState::absent : stxboot::ConfigState::unreadable;
    }
    const std::span<const char> text(config_text, (*read).size);
    const stxconfig::ParseOutcome parsed = stxconfig::parse_boot_config(text, boot_config);
    const stxconfig::ParseFailed* const refusal = parsed.as_failure();
    if (refusal != nullptr) {
        stxruntime::append_text(line, " refused line=");
        stxruntime::append_decimal(line, (*refusal).line);
        stxruntime::append_text(line, " column=");
        stxruntime::append_decimal(line, (*refusal).column);
        stxruntime::append_text(line, " error=");
        stxruntime::append_text(line, stxconfig::parse_error_name((*refusal).error));
        stxruntime::append_text(line, "\n");
        stxruntime::write_line(console, line);
        return stxboot::ConfigState::refused;
    }
    stxruntime::append_text(line, " sections=");
    stxruntime::append_decimal(line, boot_config.section_count());
    stxruntime::append_text(line, " settings=");
    stxruntime::append_decimal(line, boot_config.setting_count());
    stxruntime::append_text(line, " commands=");
    stxruntime::append_decimal(line, boot_config.init_command_count());
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
    return stxboot::ConfigState::loaded;
}

void run_init(int console) noexcept {
    const std::uint64_t started_ms = stxconfig::monotonic_milliseconds();
    stxconfig::SystemActions actions(console, service_registry, service_actions);
    const stxconfig::InitSummary summary = stxconfig::run_init_section(boot_config, actions, init_workspace);
    const std::uint64_t ended_ms = stxconfig::monotonic_milliseconds();
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR init done commands=");
    stxruntime::append_decimal(line, summary.commands);
    stxruntime::append_text(line, " skipped=");
    stxruntime::append_decimal(line, summary.skipped);
    stxruntime::append_text(line, " failed=");
    stxruntime::append_decimal(line, summary.failed);
    stxruntime::append_text(line, " ms=");
    stxruntime::append_decimal(line, ended_ms >= started_ms ? ended_ms - started_ms : 0);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

[[nodiscard]] std::uint64_t uptime_milliseconds() noexcept {
    const stxruntime::ClockNanoseconds uptime = stxruntime::read_clock(CLOCK_UPTIME);
    return uptime.has_value() ? *uptime / stxruntime::nanoseconds_per_millisecond : 0u;
}

// The failsafe path (appliance/disk/LAYOUT.md): its own init section, then
// the reason in the boot log.
void take_failsafe(int console, stxboot::FailsafeReason reason) noexcept {
    failsafe_taken = true;
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR failsafe reason=");
    stxruntime::append_text(line, stxboot::failsafe_reason_name(reason));
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
    if (load_config(console, stxboot::failsafe_config_path) == stxboot::ConfigState::loaded) {
        run_init(console);
    }
    LineText event = stxruntime::empty_line();
    stxruntime::append_text(event, "failsafe-");
    stxruntime::append_text(event, stxboot::failsafe_reason_name(reason));
    // NUL-terminated for append_boot_log; the event is far shorter than a line.
    const std::size_t end = event.length < stxruntime::line_capacity_bytes ? event.length
                                                                          : stxruntime::line_capacity_bytes - 1u;
    event.bytes[end] = '\0';
    stxboot::append_boot_log(console, boot_facts_now, event.bytes, uptime_milliseconds());
}

// The init section runs on the boot's first supervisor start only; the
// failsafe path instead when the appliance cannot come up.
void boot_once(int console, const RestartCount& restart_count) noexcept {
    stxboot::ConfigState config = stxboot::ConfigState::not_read;
    if (restart_count.has_value() && *restart_count == 0u) {
        config = load_config(console, stxconfig::config_path);
    }
    const stxboot::FailsafeReason reason = stxboot::failsafe_reason(config, restart_count);
    if (reason != stxboot::FailsafeReason::none) {
        take_failsafe(console, reason);
        return;
    }
    if (!restart_count.has_value() || *restart_count != 0u) {
        LineText line = stxruntime::empty_line();
        stxruntime::append_text(line, "STXSUPERVISOR init not-run restart_count=");
        if (restart_count.has_value()) {
            stxruntime::append_decimal(line, *restart_count);
        } else {
            stxruntime::append_text(line, "unknown");
        }
        stxruntime::append_text(line, "\n");
        stxruntime::write_line(console, line);
        return;
    }
    if (config == stxboot::ConfigState::loaded) {
        service_registry.load(boot_config);
        run_init(console);
        init_done = true;
        init_done_ms = stxconfig::monotonic_milliseconds();
        if (boot_facts_now.slot.has_value()) {
            good_check_due_ms = init_done_ms + stxboot::good_boot_settle_ms;
        }
        if (boot_facts_now.fallback) {
            stxboot::append_boot_log(console, boot_facts_now, "fallback", uptime_milliseconds());
        }
    }
}

// The good-boot rule, when due (appliance/disk/LAYOUT.md).
void check_good_boot(int console, std::uint64_t now_ms) noexcept {
    if (!good_check_due_ms.has_value() || now_ms < *good_check_due_ms || boot_marked_good) {
        return;
    }
    std::array<stxboot::ServiceHealth, stxconfig::services_max> health{};
    const std::size_t count = service_registry.service_count();
    for (std::size_t index = 0; index < count && index < health.size(); index += 1) {
        const stxconfig::ServiceState& state = service_registry.state(index);
        stxboot::ServiceHealth item{};
        if (state.as_running() != nullptr) {
            item.state = stxboot::ServiceStateKind::running;
        } else if (state.as_waiting() != nullptr) {
            item.state = stxboot::ServiceStateKind::waiting;
        } else if (state.as_stopping() != nullptr) {
            item.state = stxboot::ServiceStateKind::stopping;
        } else {
            item.state = stxboot::ServiceStateKind::stopped;
        }
        item.restart_never = boot_config.service(index).restart == stxconfig::RestartPolicy::never;
        health[index] = item;
    }
    stxboot::GoodBootInputs inputs{};
    inputs.registered = shim_registered;
    inputs.init_done = init_done;
    inputs.failsafe_taken = failsafe_taken;
    inputs.services_healthy = stxboot::services_healthy(
        std::span<const stxboot::ServiceHealth>(health.data(), count < health.size() ? count : health.size()));
    inputs.network_up = stxboot::network_up();
    inputs.init_done_ms = init_done_ms;
    inputs.now_ms = now_ms;
    if (stxboot::boot_is_good(inputs)) {
        stxboot::mark_boot_good(console, boot_facts_now);
        boot_marked_good = true;
        good_check_due_ms = std::nullopt;
        return;
    }
    if (!boot_not_good_reported) {
        boot_not_good_reported = true;
        LineText line = stxruntime::empty_line();
        stxruntime::append_text(line, "STXSUPERVISOR boot not-yet-good registered=");
        stxruntime::append_text(line, inputs.registered ? "1" : "0");
        stxruntime::append_text(line, inputs.services_healthy ? " services=1" : " services=0");
        stxruntime::append_text(line, inputs.network_up ? " network=1\n" : " network=0\n");
        stxruntime::write_line(console, line);
    }
    good_check_due_ms = now_ms + good_recheck_ms;
}

// ---------------------------------------------------------------------------
// Staying up.

// Reaps every exited child and tells the registry; a child no service owns
// (an init program the executor gave up on, an orphan) is simply reaped.
void reap_children() noexcept {
    // Each successful waitpid removes one zombie; the children are finite,
    // and EINTR retries are bounded.
    int interrupted = 0;
    for (;;) {
        int status = 0;
        const pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            const bool owned = service_registry.child_exited(static_cast<int>(child), status,
                                                             stxconfig::monotonic_milliseconds(), service_actions);
            (void)owned;
            continue;
        }
        if (child < 0 && errno == EINTR && interrupted < 16) {
            interrupted += 1;
            continue;
        }
        return;
    }
}

[[nodiscard]] timespec timeout_of(std::uint64_t milliseconds) noexcept {
    struct timespec timeout{};
    timeout.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    timeout.tv_nsec = static_cast<long>((milliseconds % 1000u) * stxruntime::nanoseconds_per_millisecond);
    return timeout;
}

// When nothing is due the loop still wakes hourly, harmlessly.
constexpr std::uint64_t idle_wait_ms = 3600000;
// Stopping: how often to look, and for how long at most (the registry's
// SIGTERM grace, its SIGKILL, and time to reap). Inside stxinit's own 1 s
// grace at shutdown.
constexpr std::uint64_t stop_poll_ms = 10;
constexpr std::uint64_t stop_wait_ms = stxconfig::service_stop_grace_ms + 300u;

// SIGTERM: stop every service, wait for them within stop_wait_ms, then
// stop. A service still running then is left to stxinit's SIGKILL.
[[noreturn]] void stop(const sigset_t& blocked) noexcept {
    service_registry.stop_all(stxconfig::monotonic_milliseconds(), service_actions);
    const std::uint64_t polls_max = stop_wait_ms / stop_poll_ms;
    for (std::uint64_t poll = 0; poll < polls_max && !service_registry.all_stopped(); poll += 1) {
        const timespec timeout = timeout_of(stop_poll_ms);
        siginfo_t information{};
        (void)sigtimedwait(&blocked, &information, &timeout);
        reap_children();
        service_registry.tick(stxconfig::monotonic_milliseconds(), service_actions);
    }
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR stop services=");
    stxruntime::append_text(line, service_registry.all_stopped() ? "stopped\n" : "left-running\n");
    stxruntime::write_line(STDOUT_FILENO, line);
    _exit(EXIT_SUCCESS);
}

// SIGUSR1: the shim terminates userspace; the supervisor waits, then the
// caller reaps and the registry restarts services by their policy.
void terminate(const ShimRegistration& shim) noexcept {
    const ShimRegistered* const registered = shim.as_registered();
    if (registered == nullptr) {
        report_terminate_unavailable(STDOUT_FILENO);
        return;
    }
    const TerminateOutcome outcome = terminate_userspace(*registered);
    report_terminate(STDOUT_FILENO, outcome);
}

[[noreturn]] void run(const ShimRegistration& shim) noexcept {
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGUSR1);
    (void)sigprocmask(SIG_BLOCK, &blocked, nullptr);
    struct sigaction on_child{};
    on_child.sa_handler = note_signal;
    sigemptyset(&on_child.sa_mask);
    on_child.sa_flags = 0;
    (void)sigaction(SIGCHLD, &on_child, nullptr);

    // Children that exited before signals were blocked (during the init
    // section) sent no SIGCHLD that is still pending: reap them now.
    reap_children();

    for (;;) {
        const std::uint64_t now_ms = stxconfig::monotonic_milliseconds();
        service_registry.tick(now_ms, service_actions);
        check_good_boot(STDOUT_FILENO, now_ms);
        std::uint64_t wait_ms = idle_wait_ms;
        const std::optional<std::uint64_t> due_ms = service_registry.next_due_ms();
        if (due_ms.has_value()) {
            wait_ms = *due_ms > now_ms ? *due_ms - now_ms : 0u;
        }
        if (good_check_due_ms.has_value()) {
            const std::uint64_t good_wait_ms = *good_check_due_ms > now_ms ? *good_check_due_ms - now_ms : 0u;
            wait_ms = good_wait_ms < wait_ms ? good_wait_ms : wait_ms;
        }
        const timespec timeout = timeout_of(wait_ms);
        siginfo_t information{};
        const int signal_number = sigtimedwait(&blocked, &information, &timeout);
        if (signal_number == SIGTERM) {
            stop(blocked);
        }
        if (signal_number == SIGUSR1) {
            terminate(shim);
        }
        // SIGCHLD: reap. SIGINT, SIGHUP: nothing to do in the first form.
        // A time-out (EAGAIN) or EINTR: go round; tick does what is due.
        reap_children();
    }
}

}  // namespace stxsupervisor

int main(int argument_count, char** argument_vector) {
    const stxruntime::GenericTimer timer = stxruntime::read_generic_timer();
    const stxruntime::ClockNanoseconds uptime = stxruntime::read_clock(CLOCK_UPTIME);

    const char* argument = argument_count >= 2 ? argument_vector[1] : nullptr;
    const stxsupervisor::RestartCount restart_count = stxsupervisor::parse_restart_count(argument);

    stxruntime::LineText marker = stxruntime::empty_line();
    stxruntime::append_text(marker, "STXSUPERVISOR start restart_count=");
    if (restart_count.has_value()) {
        stxruntime::append_decimal(marker, *restart_count);
    } else {
        stxruntime::append_text(marker, "unknown");
    }
    stxruntime::append_boot_readings(marker, uptime, timer);
    stxruntime::append_text(marker, "\n");
    stxruntime::write_line(STDOUT_FILENO, marker);

    // SIGUSR1 (terminate) stays pending until run() takes it; the programs
    // and services the supervisor starts unblock every signal before
    // execve (system_actions.cpp).
    sigset_t terminate_request{};
    sigemptyset(&terminate_request);
    sigaddset(&terminate_request, SIGUSR1);
    (void)sigprocmask(SIG_BLOCK, &terminate_request, nullptr);

    const stxsupervisor::ShimRegistration shim = stxsupervisor::register_with_shim();
    stxsupervisor::report_registration(STDOUT_FILENO, shim);
    stxsupervisor::shim_registered = shim.as_registered() != nullptr;
    stxsupervisor::boot_facts_now = stxboot::read_boot_facts(STDOUT_FILENO);

    // The init section runs before run() blocks signals: the executor waits
    // for each program itself (SIGCHLD keeps its default, which discards the
    // signal but not the child's status). A shutdown during the init section
    // ends the supervisor by SIGTERM's default action, without its stop
    // line; stxinit's shutdown goes on regardless.
    stxsupervisor::boot_once(STDOUT_FILENO, restart_count);
    stxsupervisor::run(shim);
}
