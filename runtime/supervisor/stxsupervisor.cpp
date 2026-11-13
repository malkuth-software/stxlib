// stxsupervisor: the appliance supervisor, first form (C1, steps S1-S2).
//
// It writes one marker; on the boot's first start (restart count 0) it
// reads the boot configuration and runs its init section once
// (config/boot_config.h, config/init_executor.h; the init section is the
// supervisor's, not PID 1's: coordinator, 2026-11-13, option a); then it
// stays up reaping children and stops cleanly on SIGTERM, which is how
// stxinit's shutdown ends it. A restarted supervisor does not run the init
// section again: its mounts and programs belong to the boot, not to the
// supervisor's life. The services registry comes in S3; in C2 the
// supervisor moves to STXlib's own runtime and then becomes a GUI.Forms
// application.
//
// stxinit starts it in its own session with the console as standard input,
// output and error, and passes "--restart-count=<n>".
//
// Console lines, matched by appliance/vm/scenario.py:
//
//   STXSUPERVISOR start restart_count=<n|unknown> uptime_ns=<n> cntvct=<n> cntfrq=<n>
//   STXSUPERVISOR config path=<p> sections=<n> settings=<n> commands=<n>
//   STXSUPERVISOR config path=<p> absent | too-large | errno=<n>
//   STXSUPERVISOR config path=<p> refused line=<n> column=<n> error=<name>
//   STXSUPERVISOR init line=<n> <keyword> <outcome> ... ms=<n>   (one per command)
//   STXSUPERVISOR init done commands=<n> skipped=<n> failed=<n> ms=<n>
//   STXSUPERVISOR init not-run restart_count=<n|unknown>
//   STXSUPERVISOR stop
//
// Signals stay blocked and are taken with sigwaitinfo, as in stxinit.
// SIGCHLD has a do-nothing handler so that it is not discarded while
// blocked.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <span>

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/console_line.h"
#include "config/boot_config.h"
#include "config/init_executor.h"
#include "config/system_actions.h"
#include "restart_argument.h"

namespace stxsupervisor {

using stxruntime::LineText;

extern "C" void note_signal(int signal_number) noexcept {
    (void)signal_number;
}

void reap_children() noexcept {
    for (;;) {
        int status = 0;
        const pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            continue;
        }
        if (child < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// The boot configuration and its init section. The storage is static: the
// file's bytes, the parsed configuration and the executor's environment
// together are about 60 KiB, too much for a stack frame, and the supervisor
// needs exactly one of each.

char config_text[stxconfig::config_size_max];
stxconfig::BootConfig boot_config;
stxconfig::InitWorkspace init_workspace;

void begin_config_line(LineText& line) noexcept {
    stxruntime::append_text(line, "STXSUPERVISOR config path=");
    stxruntime::append_text(line, stxconfig::config_path);
}

// Reads and parses the configuration. True when boot_config holds it.
[[nodiscard]] bool load_config(int console) noexcept {
    const std::span<char> buffer(config_text, sizeof(config_text));
    const stxconfig::FileOutcome file = stxconfig::read_config_file(stxconfig::config_path, buffer);
    LineText line = stxruntime::empty_line();
    begin_config_line(line);
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
        return false;
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
        return false;
    }
    stxruntime::append_text(line, " sections=");
    stxruntime::append_decimal(line, boot_config.section_count());
    stxruntime::append_text(line, " settings=");
    stxruntime::append_decimal(line, boot_config.setting_count());
    stxruntime::append_text(line, " commands=");
    stxruntime::append_decimal(line, boot_config.init_command_count());
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
    return true;
}

void run_init(int console) noexcept {
    const std::uint64_t started_ms = stxconfig::monotonic_milliseconds();
    stxconfig::SystemActions actions(console);
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

// The init section runs on the boot's first supervisor start only.
void boot_once(int console, const RestartCount& restart_count) noexcept {
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
    if (load_config(console)) {
        run_init(console);
    }
}

// ---------------------------------------------------------------------------
// Staying up.

[[noreturn]] void run() noexcept {
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGHUP);
    sigaddset(&blocked, SIGCHLD);
    (void)sigprocmask(SIG_BLOCK, &blocked, nullptr);
    struct sigaction on_child{};
    on_child.sa_handler = note_signal;
    sigemptyset(&on_child.sa_mask);
    on_child.sa_flags = 0;
    (void)sigaction(SIGCHLD, &on_child, nullptr);

    for (;;) {
        siginfo_t information{};
        const int signal_number = sigwaitinfo(&blocked, &information);
        if (signal_number == SIGTERM) {
            LineText line = stxruntime::empty_line();
            stxruntime::append_text(line, "STXSUPERVISOR stop\n");
            stxruntime::write_line(STDOUT_FILENO, line);
            _exit(EXIT_SUCCESS);
        }
        // SIGCHLD: reap. SIGINT, SIGHUP: nothing to do in the first form.
        // A failed wait (EINTR): go round.
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

    // The init section runs before run() blocks signals: the executor waits
    // for each program itself (SIGCHLD keeps its default, which discards the
    // signal but not the child's status). A shutdown during the init section
    // ends the supervisor by SIGTERM's default action, without its stop
    // line; stxinit's shutdown goes on regardless.
    stxsupervisor::boot_once(STDOUT_FILENO, restart_count);
    stxsupervisor::run();
}
