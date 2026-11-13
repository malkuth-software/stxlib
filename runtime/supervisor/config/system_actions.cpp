// system_actions.cpp: the init section's actions on FreeBSD
// (system_actions.h).

#include "system_actions.h"

#include <cerrno>

#include <fcntl.h>
#include <signal.h>
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../common/console_line.h"

namespace stxconfig {

// ---------------------------------------------------------------------------
// FileOutcome.

FileOutcome::FileOutcome(const Alternatives& alternatives) noexcept : alternatives_(alternatives) {}

FileOutcome FileOutcome::read(std::size_t size) noexcept {
    FileRead state{};
    state.size = size;
    const FileOutcome outcome(Alternatives(std::in_place_type<FileRead>, state));
    return outcome;
}

FileOutcome FileOutcome::absent() noexcept {
    const FileOutcome outcome(Alternatives(std::in_place_type<FileAbsent>));
    return outcome;
}

FileOutcome FileOutcome::too_large() noexcept {
    const FileOutcome outcome(Alternatives(std::in_place_type<FileTooLarge>));
    return outcome;
}

FileOutcome FileOutcome::read_failed(int error_number) noexcept {
    FileReadFailed state{};
    state.error_number = error_number;
    const FileOutcome outcome(Alternatives(std::in_place_type<FileReadFailed>, state));
    return outcome;
}

const FileRead* FileOutcome::as_read() const noexcept {
    return std::get_if<FileRead>(&alternatives_);
}

bool FileOutcome::is_absent() const noexcept {
    return std::holds_alternative<FileAbsent>(alternatives_);
}

bool FileOutcome::is_too_large() const noexcept {
    return std::holds_alternative<FileTooLarge>(alternatives_);
}

const FileReadFailed* FileOutcome::as_read_failed() const noexcept {
    return std::get_if<FileReadFailed>(&alternatives_);
}

// ---------------------------------------------------------------------------
// Time.

std::uint64_t monotonic_milliseconds() noexcept {
    const stxruntime::ClockNanoseconds now = stxruntime::read_clock(CLOCK_MONOTONIC);
    if (!now.has_value()) {
        return 0;
    }
    return *now / stxruntime::nanoseconds_per_millisecond;
}

namespace {

void sleep_milliseconds(std::uint32_t milliseconds) noexcept {
    struct timespec interval{};
    interval.tv_sec = static_cast<time_t>(milliseconds / 1000u);
    interval.tv_nsec = static_cast<long>(static_cast<std::uint64_t>(milliseconds % 1000u) *
                                         stxruntime::nanoseconds_per_millisecond);
    // An interrupted sleep is simply shorter; every caller bounds its loop
    // by a count, not by the clock.
    (void)nanosleep(&interval, nullptr);
}

// ---------------------------------------------------------------------------
// Reading the file.

// Closes a descriptor on every exit from read_config_file.
class DescriptorOwner {
public:
    explicit DescriptorOwner(int descriptor) noexcept : descriptor_(descriptor) {}
    DescriptorOwner(const DescriptorOwner&) = delete;
    DescriptorOwner& operator=(const DescriptorOwner&) = delete;
    ~DescriptorOwner() {
        if (descriptor_ >= 0) {
            (void)close(descriptor_);
        }
    }
    [[nodiscard]] int descriptor() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

constexpr int interrupted_retries_max = 16;

// read(2) with bounded retries on EINTR. Returns the byte count, 0 at the
// end, or -1 with errno set.
[[nodiscard]] ssize_t read_some(int descriptor, char* destination, std::size_t capacity) noexcept {
    int retries = 0;
    for (;;) {
        const ssize_t result = ::read(descriptor, destination, capacity);
        if (result >= 0 || errno != EINTR || retries >= interrupted_retries_max) {
            return result;
        }
        retries += 1;
    }
}

}  // namespace

FileOutcome read_config_file(const char* path, std::span<char> buffer) noexcept {
    const DescriptorOwner file(open(path, O_RDONLY | O_CLOEXEC));
    if (file.descriptor() < 0) {
        const int error_number = errno;
        if (error_number == ENOENT) {
            return FileOutcome::absent();
        }
        return FileOutcome::read_failed(error_number);
    }
    struct stat status{};
    if (fstat(file.descriptor(), &status) != 0) {
        return FileOutcome::read_failed(errno);
    }
    if (!S_ISREG(status.st_mode)) {
        return FileOutcome::read_failed(EFTYPE);
    }
    if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > buffer.size()) {
        return FileOutcome::too_large();
    }
    std::size_t total = 0;
    // Each successful read adds at least one byte, so the loop ends within
    // buffer.size() + 1 iterations.
    while (total < buffer.size()) {
        const ssize_t got = read_some(file.descriptor(), &buffer[total], buffer.size() - total);
        if (got < 0) {
            return FileOutcome::read_failed(errno);
        }
        if (got == 0) {
            return FileOutcome::read(total);
        }
        total += static_cast<std::size_t>(got);
    }
    // The buffer is full: the file must end here.
    char extra = '\0';
    const ssize_t more = read_some(file.descriptor(), &extra, 1);
    if (more < 0) {
        return FileOutcome::read_failed(errno);
    }
    if (more > 0) {
        return FileOutcome::too_large();
    }
    return FileOutcome::read(total);
}

// ---------------------------------------------------------------------------
// SystemActions.

SystemActions::SystemActions(int console) noexcept : console_(console), step_started_ms_(monotonic_milliseconds()) {}

bool SystemActions::is_present(PresenceKind kind, const char* path) noexcept {
    struct stat status{};
    if (stat(path, &status) != 0) {
        return false;
    }
    switch (kind) {
    case PresenceKind::device:
        return S_ISCHR(status.st_mode);
    case PresenceKind::file:
        return S_ISREG(status.st_mode);
    default:
        break;
    }
    // Fallback: an unknown kind is never present.
    return false;
}

namespace {

constexpr std::size_t mount_message_bytes = 128;

[[nodiscard]] const char* file_system_name(FileSystemKind file_system) noexcept {
    switch (file_system) {
    case FileSystemKind::ufs:
        return "ufs";
    case FileSystemKind::msdosfs:
        return "msdosfs";
    default:
        break;
    }
    // Fallback: no such file system; nmount refuses it.
    return "unknown";
}

[[nodiscard]] std::size_t c_string_bytes(const char* text) noexcept {
    std::size_t length = 0;
    while (text[length] != '\0') {
        length += 1;
    }
    return length + 1u;
}

// nmount(2) takes name and value pairs as iovecs; the strings include their
// NULs. iov_base is not const in the declaration, but nmount only reads
// these (sys/kern/vfs_mount.c, vfs_buildopts copies them in), except the
// errmsg buffer, which it writes.
void set_option(struct iovec* pair, const char* name, const char* value) noexcept {
    pair[0].iov_base = const_cast<char*>(name);
    pair[0].iov_len = c_string_bytes(name);
    pair[1].iov_base = const_cast<char*>(value);
    pair[1].iov_len = c_string_bytes(value);
}

}  // namespace

StepOutcome SystemActions::mount(FileSystemKind file_system, const char* device, const char* directory,
                                 MountAccess access) noexcept {
    char message[mount_message_bytes] = {};
    struct iovec options[8]{};
    set_option(&options[0], "fstype", file_system_name(file_system));
    set_option(&options[2], "fspath", directory);
    set_option(&options[4], "from", device);
    options[6].iov_base = const_cast<char*>("errmsg");
    options[6].iov_len = c_string_bytes("errmsg");
    options[7].iov_base = message;
    options[7].iov_len = sizeof(message);
    // MNT_RDONLY is a 64-bit constant (sys/mount.h); nmount takes the
    // flags as int, and the read-only bit is bit 0.
    const int flags = access == MountAccess::read_only ? static_cast<int>(MNT_RDONLY) : 0;
    if (nmount(options, 8, flags) == 0) {
        return StepOutcome::done();
    }
    const int error_number = errno;
    message[mount_message_bytes - 1u] = '\0';
    if (message[0] != '\0') {
        stxruntime::LineText line = stxruntime::empty_line();
        stxruntime::append_text(line, "STXSUPERVISOR init mount errmsg=");
        stxruntime::append_text(line, message);
        stxruntime::append_text(line, "\n");
        stxruntime::write_line(console_, line);
    }
    return StepOutcome::failed(error_number);
}

StepOutcome SystemActions::wait_for(PresenceKind kind, const char* path, std::uint32_t timeout_ms) noexcept {
    // Bounded by a count of polls, so a stopped or failing clock cannot
    // make the wait endless.
    const std::uint32_t polls_max = timeout_ms / poll_interval_ms + 1u;
    for (std::uint32_t poll = 0; poll <= polls_max; poll += 1) {
        if (is_present(kind, path)) {
            return StepOutcome::done();
        }
        if (poll < polls_max) {
            sleep_milliseconds(poll_interval_ms);
        }
    }
    return StepOutcome::timed_out();
}

StepOutcome SystemActions::start_service(const char* service) noexcept {
    (void)service;
    // The services registry is step S3.
    return StepOutcome::unavailable();
}

namespace {

// In the child between fork and execve: only async-signal-safe calls.
[[noreturn]] void become_program(const char* const* arguments, const char* const* environment) noexcept {
    sigset_t none{};
    sigemptyset(&none);
    (void)sigprocmask(SIG_SETMASK, &none, nullptr);
    // execve's declaration takes char* const[]; it does not modify them.
    (void)execve(arguments[0], const_cast<char* const*>(arguments), const_cast<char* const*>(environment));
    _exit(127);
}

// Polls for one child's exit, at most polls_max + 1 times.
struct ChildWait {
    bool exited;
    int wait_status;
    int error_number;
};

[[nodiscard]] ChildWait wait_for_child(pid_t child, std::uint32_t polls_max) noexcept {
    ChildWait outcome{};
    outcome.exited = false;
    outcome.wait_status = 0;
    outcome.error_number = 0;
    for (std::uint32_t poll = 0; poll <= polls_max; poll += 1) {
        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            outcome.exited = true;
            outcome.wait_status = status;
            return outcome;
        }
        if (result < 0 && errno != EINTR) {
            outcome.error_number = errno;
            return outcome;
        }
        if (poll < polls_max) {
            sleep_milliseconds(poll_interval_ms);
        }
    }
    return outcome;
}

}  // namespace

StepOutcome SystemActions::run_program(const char* const* arguments, const char* const* environment) noexcept {
    // Checked before forking, so a missing program is reported with its
    // errno rather than as exit status 127.
    if (access(arguments[0], X_OK) != 0) {
        return StepOutcome::failed(errno);
    }
    const pid_t child = fork();
    if (child < 0) {
        return StepOutcome::failed(errno);
    }
    if (child == 0) {
        become_program(arguments, environment);
    }
    const ChildWait ran = wait_for_child(child, run_timeout_ms / poll_interval_ms);
    if (ran.exited) {
        return StepOutcome::exited(ran.wait_status);
    }
    if (ran.error_number != 0) {
        return StepOutcome::failed(ran.error_number);
    }
    (void)kill(child, SIGKILL);
    // Reaped here if it dies in time; otherwise the supervisor's reaper
    // collects it later.
    const ChildWait killed = wait_for_child(child, run_kill_wait_ms / poll_interval_ms);
    (void)killed;
    return StepOutcome::timed_out();
}

void SystemActions::report(const StepReport& step) noexcept {
    const std::uint64_t now_ms = monotonic_milliseconds();
    const std::uint64_t elapsed_ms = now_ms >= step_started_ms_ ? now_ms - step_started_ms_ : 0;
    stxruntime::LineText line = stxruntime::empty_line();
    stxruntime::append_text(line, "STXSUPERVISOR init line=");
    stxruntime::append_decimal(line, step.line);
    stxruntime::append_text(line, " ");
    stxruntime::append_text(line, step.keyword);
    stxruntime::append_text(line, " ");
    stxruntime::append_text(line, step.outcome.name());
    const StepExited* const exited = step.outcome.as_exited();
    if (exited != nullptr) {
        stxruntime::append_text(line, " status=");
        stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>((*exited).wait_status)));
    }
    const StepFailed* const failed = step.outcome.as_failed();
    if (failed != nullptr) {
        stxruntime::append_text(line, " errno=");
        stxruntime::append_decimal(line, static_cast<std::uint64_t>(static_cast<unsigned int>((*failed).error_number)));
    }
    stxruntime::append_text(line, " ms=");
    stxruntime::append_decimal(line, elapsed_ms);
    stxruntime::append_text(line, "\n");
    stxruntime::write_line(console_, line);
    step_started_ms_ = monotonic_milliseconds();
}

}  // namespace stxconfig
