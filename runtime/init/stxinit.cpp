// stxinit: the appliance's PID 1, first form (C1).
//
// Design question 1, option 1 (owner): a minimal PID 1 of ours
// (notes planning/stages/C-appliance-design.md, section 1). This first form
// does three things, in order:
//
//   1. It writes one time-stamp line, the boot marker, on the console.
//   2. It reaps every child it has or inherits (orphans are re-parented to
//      PID 1 by the kernel).
//   3. It stays up.
//
// It starts no supervisor yet, reads no configuration and runs no scripts.
// The kernel starts it through the loader variable init_path
// (sys/kern/init_main.c:705-709, 759-762 at fb9033d2876d), so neither rc(8)
// nor getty(8) runs. The root file system stays read-only as the kernel
// mounted it; nothing here writes to it.
//
// PID 1 must never exit: the kernel panics if it does ("Going nowhere
// without my init!", sys/kern/kern_exit.c:289-291). No path returns from
// main. A failure is written to the console when the console is open, and
// the process stays up either way.
//
// The boot marker, one line, matched by appliance/vm/bootstages.py:
//
//   STXINIT uptime_ns=<decimal> cntvct=<decimal> cntfrq=<decimal>
//
// uptime_ns is clock_gettime(CLOCK_UPTIME) in nanoseconds: the kernel's
// uptime when PID 1 began, measured inside the guest. cntvct and cntfrq are
// the Arm generic timer's virtual count and its frequency in hertz, read
// directly from the CPU (CNTVCT_EL0, CNTFRQ_EL0), independent of the
// kernel's time keeping. If the uptime clock fails, the line says
// "uptime_ns=failed" and the host's boot timing reports the run as
// incomplete.
//
// Hosted on FreeBSD's libc for C1; it moves to STXlib's own runtime in C2.
// Built without the C++ library's compiled part (no exceptions, no RTTI,
// -nostdlib++): it uses language features, fixed-size storage and the
// header-only std::optional.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__aarch64__)
#error "stxinit reads the Arm generic timer; other architectures need their own counter read"
#endif

namespace stxinit {

// ---------------------------------------------------------------------------
// Text: a fixed buffer filled left to right. Every append checks capacity
// first; an append that does not fit writes nothing and marks the text
// truncated, so a truncated line is never mistaken for a complete one.

constexpr std::size_t marker_capacity_bytes = 128;

struct LineText {
    char bytes[marker_capacity_bytes];
    std::size_t length;
    bool truncated;
};

[[nodiscard]] LineText empty_line() noexcept {
    LineText line{};
    line.length = 0;
    line.truncated = false;
    return line;
}

void append_text(LineText& line, const char* text) noexcept {
    std::size_t text_length = 0;
    while (text[text_length] != '\0') {
        text_length += 1;
    }
    if (line.truncated || text_length > marker_capacity_bytes - line.length) {
        line.truncated = true;
        return;
    }
    for (std::size_t index = 0; index < text_length; index += 1) {
        line.bytes[line.length + index] = text[index];
    }
    line.length += text_length;
}

// The largest std::uint64_t, 18446744073709551615, has 20 decimal digits.
constexpr std::size_t uint64_decimal_digits_max = 20;

void append_decimal(LineText& line, std::uint64_t value) noexcept {
    char digits[uint64_decimal_digits_max + 1];
    std::size_t count = 0;
    std::uint64_t remaining = value;
    do {
        digits[uint64_decimal_digits_max - 1 - count] =
            static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    digits[uint64_decimal_digits_max] = '\0';
    append_text(line, &digits[uint64_decimal_digits_max - count]);
}

// ---------------------------------------------------------------------------
// The console: a descriptor for /dev/console, opened once. devfs is mounted
// on /dev by the kernel when it mounts root (sys/kern/vfs_mountroot.c), so
// the device exists before PID 1 starts. PID 1 starts with no descriptors
// open (sys/kern/init_main.c, start_init), hence the explicit open.

constexpr int closed_descriptor = -1;

struct Console {
    int descriptor;
};

[[nodiscard]] Console open_console() noexcept {
    Console console{};
    // O_NOCTTY: the console is an output here, not a controlling terminal.
    console.descriptor = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (console.descriptor < 0) {
        console.descriptor = closed_descriptor;
    }
    return console;
}

// Writes the whole line or as much as the console accepts. A short write is
// continued; an interrupted write is retried; any other failure abandons the
// line (there is nowhere else to report it). The number of attempts is
// bounded by the line length plus the retries for interruption.
void write_line(const Console& console, const LineText& line) noexcept {
    if (console.descriptor == closed_descriptor) {
        return;
    }
    constexpr int interrupted_retries_max = 16;
    int interrupted_retries = 0;
    std::size_t written = 0;
    while (written < line.length) {
        const ssize_t result = write(console.descriptor, &line.bytes[written], line.length - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR && interrupted_retries < interrupted_retries_max) {
            interrupted_retries += 1;
            continue;
        }
        return;
    }
}

void write_message(const Console& console, const char* message) noexcept {
    LineText line = empty_line();
    append_text(line, "STXINIT: ");
    append_text(line, message);
    append_text(line, "\n");
    write_line(console, line);
}

// ---------------------------------------------------------------------------
// The boot marker's measurements.

struct GenericTimer {
    std::uint64_t count;
    std::uint64_t frequency_hertz;
};

// FreeBSD lets user mode read the virtual counter and its frequency
// (CNTKCTL_EL1.EL0VCTEN, set by the generic timer driver,
// sys/arm/arm/generic_timer.c), which the vDSO time functions rely on too.
// The isb orders the counter read after the instructions before it.
[[nodiscard]] GenericTimer read_generic_timer() noexcept {
    GenericTimer timer{};
    std::uint64_t count = 0;
    std::uint64_t frequency = 0;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(count) : : "memory");
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
    timer.count = count;
    timer.frequency_hertz = frequency;
    return timer;
}

// The kernel's uptime in nanoseconds, or nothing when it cannot be read.
using UptimeNanoseconds = std::optional<std::uint64_t>;

constexpr std::uint64_t nanoseconds_per_second = 1000000000u;

[[nodiscard]] UptimeNanoseconds read_uptime() noexcept {
    struct timespec now{};
    if (clock_gettime(CLOCK_UPTIME, &now) != 0) {
        return std::nullopt;
    }
    // A valid timespec has 0 <= tv_nsec < 1e9 and, for an uptime, tv_sec >= 0.
    // tv_sec * 1e9 fits in 64 bits for any uptime below 584 years.
    if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= static_cast<long>(nanoseconds_per_second)) {
        return std::nullopt;
    }
    const std::uint64_t seconds = static_cast<std::uint64_t>(now.tv_sec);
    const std::uint64_t nanoseconds = seconds * nanoseconds_per_second + static_cast<std::uint64_t>(now.tv_nsec);
    const UptimeNanoseconds reading = nanoseconds;
    return reading;
}

[[nodiscard]] LineText boot_marker(const UptimeNanoseconds& uptime, const GenericTimer& timer) noexcept {
    LineText line = empty_line();
    append_text(line, "STXINIT uptime_ns=");
    if (uptime.has_value()) {
        append_decimal(line, *uptime);
    } else {
        append_text(line, "failed");
    }
    append_text(line, " cntvct=");
    append_decimal(line, timer.count);
    append_text(line, " cntfrq=");
    append_decimal(line, timer.frequency_hertz);
    append_text(line, "\n");
    return line;
}

// ---------------------------------------------------------------------------
// Staying up and reaping.
//
// SIGCHLD stays blocked except inside sigsuspend, so a child's exit between
// the reaping pass and the wait cannot be lost: the pending signal ends the
// next sigsuspend at once. The handler does nothing; its only purpose is to
// make SIGCHLD end sigsuspend (an ignored signal would not).

extern "C" void note_child_exit(int signal_number) noexcept {
    (void)signal_number;
}

// Collects every child that has exited, without blocking. Returns when no
// exited child remains (0), there are no children (ECHILD), or waitpid fails
// otherwise. Each pass reaps one zombie, so the loop ends.
void reap_exited_children() noexcept {
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

[[noreturn]] void stay_up(const Console& console) noexcept {
    sigset_t blocked{};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    sigset_t waiting{};
    if (sigprocmask(SIG_BLOCK, &blocked, &waiting) != 0) {
        write_message(console, "sigprocmask failed; children may stay zombies");
    }
    sigdelset(&waiting, SIGCHLD);

    struct sigaction on_child{};
    on_child.sa_handler = note_child_exit;
    sigemptyset(&on_child.sa_mask);
    on_child.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &on_child, nullptr) != 0) {
        write_message(console, "sigaction(SIGCHLD) failed; children may stay zombies");
    }

    for (;;) {
        reap_exited_children();
        // Returns only when a caught signal arrives (always -1, EINTR). With
        // no children and no signals, PID 1 sleeps here for ever.
        (void)sigsuspend(&waiting);
    }
}

}  // namespace stxinit

int main() {
    // Measure first, before anything else costs time.
    const stxinit::GenericTimer timer = stxinit::read_generic_timer();
    const stxinit::UptimeNanoseconds uptime = stxinit::read_uptime();

    const stxinit::Console console = stxinit::open_console();
    const stxinit::LineText marker = stxinit::boot_marker(uptime, timer);
    stxinit::write_line(console, marker);

    stxinit::stay_up(console);
}
