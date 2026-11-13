// stxsupervisor: the appliance supervisor, first form (C1, step S1).
//
// A placeholder for the supervisor's place in the process tree, so that
// PID 1's start, respawn and shutdown can be exercised before the real
// supervisor exists. It writes one marker, stays up reaping any children,
// and stops cleanly on SIGTERM, which is how stxinit's shutdown ends it.
// Later steps give it the configuration and its init section (S2) and the
// services registry (S3); in C2 it moves to STXlib's own runtime and then
// becomes a GUI.Forms application.
//
// stxinit starts it in its own session with the console as standard input,
// output and error, and passes "--restart-count=<n>".
//
// Console lines, matched by appliance/vm/scenario.py:
//
//   STXSUPERVISOR start restart_count=<n|unknown> uptime_ns=<n> cntvct=<n> cntfrq=<n>
//   STXSUPERVISOR stop
//
// Signals stay blocked and are taken with sigwaitinfo, as in stxinit.
// SIGCHLD has a do-nothing handler so that it is not discarded while
// blocked.

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/console_line.h"
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

    stxsupervisor::run();
}
