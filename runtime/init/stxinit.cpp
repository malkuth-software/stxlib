// stxinit: the appliance's PID 1 (C1, second form).
//
// Design question 1, option 1 (owner): a minimal PID 1 of ours that "reads
// no configuration and runs no scripts" (notes planning/stages/
// C-appliance-design.md, section 1). It does four things:
//
//   1. It writes one time-stamp line, the boot marker, on the console.
//   2. It starts the supervisor, /sbin/stxsupervisor, and starts it again
//      whenever it exits, after a bounded back-off. The supervisor learns
//      how many times it has been restarted from its one argument.
//   3. It reaps every child it has or inherits (the kernel re-parents
//      orphans to PID 1).
//   4. It shuts the machine down when the kernel asks: power-off, halt,
//      power cycle or reboot.
//
// The supervisor, not PID 1, reads the configuration and runs its init
// section (coordinator, 2026-11-13). The kernel starts stxinit through the
// loader variable init_path (sys/kern/init_main.c:705-709, 759-762 at
// fb9033d2876d), so neither rc(8) nor getty(8) runs.
//
// PID 1 must never exit: the kernel panics if it does ("Going nowhere
// without my init!", sys/kern/kern_exit.c:289-291). No path returns from
// main. A failure is written to the console when the console is open, and
// the process carries on or stays up.
//
// Console lines, each starting with "STXINIT", matched by
// appliance/vm/bootstages.py and appliance/vm/scenario.py:
//
//   STXINIT uptime_ns=<n> cntvct=<n> cntfrq=<n>       the boot marker
//   STXINIT supervisor started pid=<n> restart_count=<n> shim=<0|1>
//   STXINIT supervisor exited pid=<n> status=<n> ran_ms=<n> next_in_ms=<n>
//   STXINIT shutdown <power-off|halt|power-cycle|reboot>
//   STXINIT: <message>                                a failure
//
// The kernel asks for shutdown by signalling PID 1
// (sys/kern/kern_shutdown.c:313-334, shutdown_nice_task_fn): SIGUSR2 power
// off, SIGWINCH power cycle, SIGUSR1 halt, SIGINT reboot. ACPI's power
// button reaches it as shutdown_nice(RB_POWEROFF) (sys/dev/acpica/acpi.c:3406).
// Shutdown follows design question 3, option 1: SIGTERM to every process
// but PID 1 (kill(-1, ...) spares PID 1, system processes and the caller,
// sys/kern/kern_sig.c:1810-1811), up to 1 s grace, SIGKILL to the rest,
// sync, a bounded wait for the console's output queue to drain (so the
// kernel's shutdown messages do not cut into ours), then reboot(2).
//
// Signals: every signal stxinit acts on stays blocked and is taken with
// sigtimedwait, so none is handled asynchronously and none is lost between
// two waits. Each also has a do-nothing handler, never run, because the
// kernel discards a SIG_DFL signal sent to PID 1 (see run()).
//
// Hosted on FreeBSD's libc for C1; it moves to STXlib's own runtime in C2.
// Built static, without exceptions or RTTI.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../common/console_line.h"

namespace stxinit {

using stxruntime::ClockNanoseconds;
using stxruntime::LineText;

// ---------------------------------------------------------------------------
// Fixed choices. PID 1 reads no configuration, so these are its whole policy.

constexpr char supervisor_path[] = "/sbin/stxsupervisor";

// Back-off before restarting a supervisor that exited after running less
// than stable_run_ms: 100 ms doubled for each such exit in a row, at most
// 10 s. A supervisor that ran stable_run_ms or longer is restarted at once.
constexpr std::uint64_t backoff_first_ms = 100;
constexpr std::uint64_t backoff_max_ms = 10000;
constexpr std::uint64_t stable_run_ms = 30000;
// Quick exits in a row are counted up to this, which keeps the doubling
// shift bounded (100 ms << 16 is far beyond backoff_max_ms).
constexpr std::uint32_t quick_exits_counted_max = 16;

// Shutdown: grace after SIGTERM, then the wait after SIGKILL, polled.
constexpr std::uint64_t termination_grace_ms = 1000;
constexpr std::uint64_t kill_wait_ms = 1000;
constexpr std::uint64_t shutdown_poll_ms = 20;

// The longest single wait when nothing is scheduled. Only bounds one
// sigtimedwait call; the loop waits again.
constexpr std::uint64_t idle_wait_ms = 3600000;

// ---------------------------------------------------------------------------
// The console.

[[nodiscard]] int open_console() noexcept {
    // O_NOCTTY: PID 1 takes no controlling terminal. devfs is mounted on
    // /dev by the kernel with root (sys/kern/vfs_mountroot.c), and PID 1
    // starts with no descriptors open, hence the explicit open.
    const int descriptor = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (descriptor < 0) {
        return -1;
    }
    return descriptor;
}

void write_message(int console, const char* message) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXINIT: ");
    stxruntime::append_text(line, message);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
}

[[nodiscard]] std::uint64_t monotonic_ms(int console) noexcept {
    const ClockNanoseconds now = stxruntime::read_clock(CLOCK_MONOTONIC);
    if (!now.has_value()) {
        // CLOCK_MONOTONIC cannot fail on FreeBSD with a valid clock id; if it
        // did, time stands still and every back-off ends at once.
        write_message(console, "CLOCK_MONOTONIC failed");
        return 0;
    }
    return *now / stxruntime::nanoseconds_per_millisecond;
}

// ---------------------------------------------------------------------------
// The supervisor's state: running, waiting to be started, or stopped for
// shutdown. Each state holds only its own data.

struct SupervisorRunning {
    pid_t pid;
    std::uint64_t started_ms;
};

struct SupervisorWaiting {
    std::uint64_t start_at_ms;
};

struct SupervisorStopped {
};

class SupervisorState {
public:
    [[nodiscard]] static SupervisorState running(pid_t pid, std::uint64_t started_ms) noexcept {
        SupervisorRunning state{};
        state.pid = pid;
        state.started_ms = started_ms;
        return SupervisorState(state);
    }

    [[nodiscard]] static SupervisorState waiting(std::uint64_t start_at_ms) noexcept {
        SupervisorWaiting state{};
        state.start_at_ms = start_at_ms;
        return SupervisorState(state);
    }

    [[nodiscard]] static SupervisorState stopped() noexcept {
        return SupervisorState(SupervisorStopped{});
    }

    [[nodiscard]] const SupervisorRunning* as_running() const noexcept {
        return std::get_if<SupervisorRunning>(&state_);
    }

    [[nodiscard]] const SupervisorWaiting* as_waiting() const noexcept {
        return std::get_if<SupervisorWaiting>(&state_);
    }

private:
    using State = std::variant<SupervisorRunning, SupervisorWaiting, SupervisorStopped>;

    explicit SupervisorState(SupervisorRunning state) noexcept : state_(state) {}
    explicit SupervisorState(SupervisorWaiting state) noexcept : state_(state) {}
    explicit SupervisorState(SupervisorStopped state) noexcept : state_(state) {}

    State state_;
};

// Restart accounting. restart_count is the number of starts after the first;
// it saturates at its maximum instead of wrapping (it is shown to the
// supervisor, not used as an identity).
struct RestartAccount {
    std::uint64_t restart_count;
    std::uint32_t quick_exits_in_row;
};

[[nodiscard]] std::uint64_t backoff_ms(std::uint32_t quick_exits_in_row) noexcept {
    if (quick_exits_in_row == 0) {
        return 0;
    }
    std::uint64_t delay = backoff_first_ms;
    for (std::uint32_t doubling = 1; doubling < quick_exits_in_row; doubling += 1) {
        delay *= 2u;
        if (delay >= backoff_max_ms) {
            return backoff_max_ms;
        }
    }
    return delay;
}

// ---------------------------------------------------------------------------
// Starting the supervisor.

// The shim's device (appliance/shim/stxshim.h, STXSHIM_DEVICE_PATH) and the
// descriptor the supervisor finds it on (STXSHIM_SUPERVISOR_DESCRIPTOR).
// PID 1 opens the device before each start and hands the open file to that
// child alone: the shim accepts a registration only on a file PID 1 opened,
// once per file, which closes C1's window in which an orphan PID 1 adopted
// could register between a supervisor's death and its restart
// (appliance/C2_DESIGN.md, "Orphan registration"). Spelled here rather than
// included, so that PID 1 needs no kernel header; the supervisor's
// shim_client.cpp includes stxshim.h and checks both values.
constexpr char shim_path[] = "/dev/stxshim";
constexpr int shim_descriptor = 3;
// Where the child parks the shim's file while it sets up descriptors 0-2.
constexpr int shim_parking_min = 10;

// Opens the shim for the next supervisor, close-on-exec in PID 1 (only the
// child's copy at shim_descriptor survives its execve). -1 when the kernel
// has no shim or the open failed; the supervisor then starts without one
// and reports it (shim_client.h).
[[nodiscard]] int open_shim_for_supervisor() noexcept {
    int descriptor = -1;
    for (int attempt = 0; attempt < 16; attempt += 1) {
        descriptor = open(shim_path, O_RDWR | O_CLOEXEC);
        if (descriptor >= 0 || errno != EINTR) {
            break;
        }
    }
    return descriptor;
}

// The child's side of the fork: a new session, the console as standard input,
// output and error, the shim's file (if any) at shim_descriptor, no blocked
// signals, then the supervisor. Never returns.
[[noreturn]] void become_supervisor(char* const argument_vector[], int shim) noexcept {
    (void)setsid();
    // The shim's file first goes above every descriptor the console setup
    // can touch, so that neither dup2 below overwrites the other.
    int parked = -1;
    if (shim >= 0) {
        parked = fcntl(shim, F_DUPFD_CLOEXEC, shim_parking_min);
        if (parked != shim) {
            (void)close(shim);
        }
    }
    const int console = open("/dev/console", O_RDWR | O_NOCTTY);
    if (console >= 0) {
        (void)dup2(console, STDIN_FILENO);
        (void)dup2(console, STDOUT_FILENO);
        (void)dup2(console, STDERR_FILENO);
        if (console > STDERR_FILENO) {
            (void)close(console);
        }
    }
    // dup2 gives the new descriptor no close-on-exec flag, so it survives
    // the execve; the parked copy keeps its flag and is closed.
    if (parked >= 0) {
        (void)dup2(parked, shim_descriptor);
        (void)close(parked);
    }
    sigset_t none{};
    sigemptyset(&none);
    (void)sigprocmask(SIG_SETMASK, &none, nullptr);

    char path_variable[] = "PATH=/sbin:/bin:/usr/sbin:/usr/bin";
    char home_variable[] = "HOME=/";
    char* const environment[] = {path_variable, home_variable, nullptr};
    (void)execve(supervisor_path, argument_vector, environment);

    write_message(STDERR_FILENO, "execve /sbin/stxsupervisor failed");
    _exit(127);
}

// Forks and starts the supervisor. Returns its state: running, or waiting for
// a retry when fork failed.
[[nodiscard]] SupervisorState start_supervisor(int console, const RestartAccount& account) noexcept {
    // Prepared before the fork, so the child only copies and calls.
    LineText argument = stxruntime::empty_line();
    stxruntime::append_text(argument, "--restart-count=");
    stxruntime::append_decimal(argument, account.restart_count);
    char argument_text[stxruntime::line_capacity_bytes + 1];
    stxruntime::copy_line_to_c_string(argument, argument_text);
    char path_text[sizeof(supervisor_path)];
    for (std::size_t index = 0; index < sizeof(supervisor_path); index += 1) {
        path_text[index] = supervisor_path[index];
    }
    char* const argument_vector[] = {path_text, argument_text, nullptr};

    const std::uint64_t now_ms = monotonic_ms(console);
    const int shim = open_shim_for_supervisor();
    const pid_t child = fork();
    if (child == 0) {
        become_supervisor(argument_vector, shim);
    }
    // PID 1 keeps no copy: the child's is the only one, so no later child
    // of PID 1 (an adopted orphan included) can hold this file.
    if (shim >= 0) {
        (void)close(shim);
    }
    if (child < 0) {
        write_message(console, "fork failed; retrying the supervisor after the longest back-off");
        return SupervisorState::waiting(now_ms + backoff_max_ms);
    }

    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXINIT supervisor started pid=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(child));
    stxruntime::append_text(line, " restart_count=");
    stxruntime::append_decimal(line, account.restart_count);
    stxruntime::append_text(line, shim >= 0 ? " shim=1\n" : " shim=0\n");
    stxruntime::write_line(console, line);
    return SupervisorState::running(child, now_ms);
}

// The supervisor has exited: account for it, report it, and schedule the
// next start.
[[nodiscard]] SupervisorState schedule_restart(int console, const SupervisorRunning& ended, int status,
                                               RestartAccount& account) noexcept {
    const std::uint64_t now_ms = monotonic_ms(console);
    const std::uint64_t ran_ms = now_ms >= ended.started_ms ? now_ms - ended.started_ms : 0;
    if (ran_ms >= stable_run_ms) {
        account.quick_exits_in_row = 0;
    } else if (account.quick_exits_in_row < quick_exits_counted_max) {
        account.quick_exits_in_row += 1;
    }
    if (account.restart_count < UINT64_MAX) {
        account.restart_count += 1;
    }
    const std::uint64_t delay_ms = backoff_ms(account.quick_exits_in_row);

    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXINIT supervisor exited pid=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(ended.pid));
    stxruntime::append_text(line, " status=");
    stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>(status)));
    stxruntime::append_text(line, " ran_ms=");
    stxruntime::append_decimal(line, ran_ms);
    stxruntime::append_text(line, " next_in_ms=");
    stxruntime::append_decimal(line, delay_ms);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);
    return SupervisorState::waiting(now_ms + delay_ms);
}

// ---------------------------------------------------------------------------
// Reaping.

// Reaps every child that has exited, without blocking. If the supervisor is
// among them, the state moves to waiting. Ends when no exited child remains,
// there are no children, or waitpid fails otherwise; each pass reaps one
// child, so the loop ends.
void reap_children(int console, SupervisorState& supervisor, RestartAccount& account) noexcept {
    for (;;) {
        int status = 0;
        const pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            const SupervisorRunning* running = supervisor.as_running();
            if (running != nullptr && (*running).pid == child) {
                const SupervisorRunning ended = *running;
                supervisor = schedule_restart(console, ended, status, account);
            }
            continue;
        }
        if (child < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Shutdown.

struct ShutdownKind {
    int howto;
    const char* name;
};

// The signal the kernel sends for each kind (sys/kern/kern_shutdown.c:321-332).
[[nodiscard]] std::optional<ShutdownKind> shutdown_kind_for(int signal_number) noexcept {
    ShutdownKind kind{};
    switch (signal_number) {
    case SIGUSR2:
        kind.howto = RB_POWEROFF;
        kind.name = "power-off";
        return kind;
    case SIGWINCH:
        kind.howto = RB_POWERCYCLE;
        kind.name = "power-cycle";
        return kind;
    case SIGUSR1:
        kind.howto = RB_HALT;
        kind.name = "halt";
        return kind;
    case SIGINT:
        kind.howto = RB_AUTOBOOT;
        kind.name = "reboot";
        return kind;
    default:
        break;
    }
    return std::nullopt;
}

void sleep_ms(std::uint64_t milliseconds) noexcept {
    struct timespec interval{};
    interval.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    interval.tv_nsec = static_cast<long>((milliseconds % 1000u) * stxruntime::nanoseconds_per_millisecond);
    (void)nanosleep(&interval, nullptr);
}

// Reaps until no child remains or the time runs out, polling.
void reap_until_none(int console, std::uint64_t limit_ms) noexcept {
    const std::uint64_t deadline_ms = monotonic_ms(console) + limit_ms;
    for (;;) {
        int status = 0;
        const pid_t child = waitpid(-1, &status, WNOHANG);
        if (child > 0) {
            continue;
        }
        if (child < 0 && errno == EINTR) {
            continue;
        }
        if (child < 0) {
            return;  // ECHILD: no children left.
        }
        if (monotonic_ms(console) >= deadline_ms) {
            return;
        }
        sleep_ms(shutdown_poll_ms);
    }
}

[[noreturn]] void stay_up_forever(const sigset_t& blocked) noexcept {
    for (;;) {
        siginfo_t information{};
        (void)sigwaitinfo(&blocked, &information);
    }
}

// Waits until the console's output queue is empty, at most
// console_drain_polls_max * console_drain_poll_ms. Userland console writes
// return once queued in the tty; reboot(2)'s own messages are printed by the
// kernel directly, so without this wait they interleave with the last
// lines of stxinit and the supervisor (C1 S1 run 4: "STXINIT shutdown
// power-oWaiting (max 60 seconds) for system process `vnlru' ...").
constexpr int console_drain_polls_max = 100;
constexpr long console_drain_poll_ms = 5;

void wait_console_drained(int console) noexcept {
    if (console < 0) {
        return;
    }
    for (int poll = 0; poll < console_drain_polls_max; poll += 1) {
        int queued = 0;
        if (ioctl(console, TIOCOUTQ, &queued) != 0 || queued <= 0) {
            return;
        }
        struct timespec interval{};
        interval.tv_sec = 0;
        interval.tv_nsec = console_drain_poll_ms * static_cast<long>(stxruntime::nanoseconds_per_millisecond);
        (void)nanosleep(&interval, nullptr);
    }
}

[[noreturn]] void shut_down(int console, const ShutdownKind& kind, const sigset_t& blocked) noexcept {
    LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXINIT shutdown ");
    stxruntime::append_text(line, kind.name);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console, line);

    (void)kill(-1, SIGTERM);
    reap_until_none(console, termination_grace_ms);
    (void)kill(-1, SIGKILL);
    reap_until_none(console, kill_wait_ms);
    sync();
    wait_console_drained(console);
    (void)reboot(kind.howto);

    // reboot(2) returned: it failed. Nothing else is left running; stay up.
    write_message(console, "reboot(2) failed; staying up");
    stay_up_forever(blocked);
}

// ---------------------------------------------------------------------------
// The main loop.

extern "C" void note_signal(int signal_number) noexcept {
    (void)signal_number;
}

[[noreturn]] void run(int console) noexcept {
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigaddset(&blocked, SIGUSR2);
    sigaddset(&blocked, SIGWINCH);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGINT);
    if (sigprocmask(SIG_BLOCK, &blocked, nullptr) != 0) {
        write_message(console, "sigprocmask failed");
    }
    // Every waited signal needs a handler, even though none is ever run (the
    // signals stay blocked and sigtimedwait takes them). For PID 1 the
    // kernel discards a signal whose action is SIG_DFL, under sigtimedwait
    // too: issignal() returns SIGSTATUS_IGNORE for p_pid <= 1
    // (sys/kern/kern_sig.c:3257-3271, "Don't take default actions on system
    // processes"). C1 S1 run 3 showed it: the power button reached
    // acpi_button and shutdown_nice sent SIGUSR2, and stxinit never saw it.
    // SIGCHLD and SIGWINCH, ignored by default, would be discarded anyway.
    struct sigaction on_signal{};
    on_signal.sa_handler = note_signal;
    sigemptyset(&on_signal.sa_mask);
    on_signal.sa_flags = 0;
    const int waited[] = {SIGCHLD, SIGUSR2, SIGWINCH, SIGUSR1, SIGINT};
    for (const int signal_number : waited) {
        if (sigaction(signal_number, &on_signal, nullptr) != 0) {
            write_message(console, "sigaction failed");
        }
    }

    RestartAccount account{};
    account.restart_count = 0;
    account.quick_exits_in_row = 0;
    SupervisorState supervisor = SupervisorState::waiting(0);

    for (;;) {
        std::uint64_t wait_ms = idle_wait_ms;
        const SupervisorWaiting* waiting = supervisor.as_waiting();
        if (waiting != nullptr) {
            const std::uint64_t now_ms = monotonic_ms(console);
            if (now_ms >= (*waiting).start_at_ms) {
                supervisor = start_supervisor(console, account);
                continue;
            }
            wait_ms = (*waiting).start_at_ms - now_ms;
        }

        struct timespec timeout{};
        timeout.tv_sec = static_cast<time_t>(wait_ms / 1000u);
        timeout.tv_nsec = static_cast<long>((wait_ms % 1000u) * stxruntime::nanoseconds_per_millisecond);
        siginfo_t information{};
        const int signal_number = sigtimedwait(&blocked, &information, &timeout);
        if (signal_number < 0) {
            // EAGAIN: the wait ran out (a restart may be due). EINTR: try again.
            continue;
        }
        const std::optional<ShutdownKind> kind = shutdown_kind_for(signal_number);
        if (kind.has_value()) {
            supervisor = SupervisorState::stopped();
            shut_down(console, *kind, blocked);
        }
        // SIGCHLD; also any other, harmlessly: reap and go round.
        reap_children(console, supervisor, account);
    }
}

}  // namespace stxinit

int main() {
    // Measure first, before anything else costs time.
    const stxruntime::GenericTimer timer = stxruntime::read_generic_timer();
    const stxruntime::ClockNanoseconds uptime = stxruntime::read_clock(CLOCK_UPTIME);

    const int console = stxinit::open_console();
    stxruntime::LineText marker = stxruntime::empty_line();
    stxruntime::append_text(marker, "STXINIT");
    stxruntime::append_boot_readings(marker, uptime, timer);
    stxruntime::append_text(marker, "\n");
    stxruntime::write_line(console, marker);

    stxinit::run(console);
}
