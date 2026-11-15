// Child processes on POSIX hosts (include/stxlib/children.hpp), over
// posix_spawn. Ported from stx_auxlib's src/hosted/children_posix.cpp at
// b6e330a.
//
// Changes from stx_auxlib:
// - The abandoned-children list is stxlib_state's fixed table under its
//   spin lock (state.hpp), shared by every module; stx_auxlib kept a heap
//   vector behind a mutex in each module.
// - The child state is inline in Child; there is no allocation after the
//   spawn, so no child is ever killed for want of memory.
// - The environment the child receives is read under stxlib_state's
//   environment lock, so set_variable in another thread cannot change it
//   mid-spawn.
// - On Apple hosts the environment comes from _NSGetEnviron(), because a
//   shared library has no direct access to environ (macOS environ(7):
//   "Shared libraries and bundles don't have direct access to environ";
//   read in the man page on macOS 26, not proved).

#include "stxlib/children.hpp"

#include "stxlib/detail/cancellation.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "core/wtf8.hpp"
#include "hosted/children_common.hpp"
#include "state/state.hpp"

#include <cerrno>
#include <csignal>
#include <ctime>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/param.h> // __FreeBSD_version
#endif

// Descriptors past 2 are closed in the child where the C library can say so
// (A-stxlib-hosted-01-F6):
//   - Apple: POSIX_SPAWN_CLOEXEC_DEFAULT, which makes every descriptor the
//     file actions do not name close-on-exec (posix_spawnattr_setflags(3),
//     macOS 10.7 and later); inherit mode names 1 and 2 with
//     posix_spawn_file_actions_addinherit_np.
//   - glibc 2.34 and later, and FreeBSD 13.1 and later:
//     posix_spawn_file_actions_addclosefrom_np(3) from descriptor 3.
//   - elsewhere (musl, which has no version macro to test, older glibc and
//     FreeBSD, Emscripten): only descriptors opened close-on-exec stay out
//     of the child (children.hpp).
#if defined(__APPLE__)
#define STXLIB_SPAWN_CLOEXEC_DEFAULT 1
#else
#define STXLIB_SPAWN_CLOEXEC_DEFAULT 0
#endif
#if (defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))) || \
    (defined(__FreeBSD__) && defined(__FreeBSD_version) && __FreeBSD_version >= 1301000)
#define STXLIB_SPAWN_CLOSEFROM 1
#else
#define STXLIB_SPAWN_CLOSEFROM 0
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace stxlib {

namespace {

char** environment_block() noexcept {
#if defined(__APPLE__)
    char** const result = *_NSGetEnviron();
#else
    char** const result = environ;
#endif
    return result;
}

ErrorRecord record_of_errno(const int error, const Operation operation) noexcept {
    ErrorKind kind = ErrorKind::io_failure;
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        kind = ErrorKind::not_found;
        break;
    case EACCES:
    case EPERM:
    case ENOEXEC:
        kind = ErrorKind::permission_denied;
        break;
    case ENOMEM:
        kind = ErrorKind::out_of_memory;
        break;
    case EAGAIN:
    case EMFILE:
    case ENFILE:
        kind = ErrorKind::limit_exceeded;
        break;
    default:
        break;
    }
    const ErrorRecord result = host_failure::record(kind, operation, HostCode::none,
                                                    ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
    return result;
}

pid_t process_id(const i64 process) noexcept {
    const pid_t result = static_cast<pid_t>(process);
    return result;
}

// Reaps every abandoned child that has ended, without blocking.
void reap_abandoned() noexcept {
    state::ProcessState& process = state::current_process();
    const detail::CancellationHold no_cancel; // waitpid is a cancellation point (audit E, 4.2)
    const spin::Guard lock(process.children_lock);
    usize kept = 0;
    for (usize index = 0; index < process.abandoned_child_count; ++index) {
        const i64 child = process.abandoned_children[index];
        int status = 0;
        const pid_t reaped = ::waitpid(process_id(child), &status, WNOHANG);
        // 0: still running. -1 with ECHILD: reaped elsewhere; drop it.
        if (reaped == 0 || (reaped < 0 && errno == EINTR)) {
            process.abandoned_children[kept] = child;
            kept = kept + 1;
        }
    }
    process.abandoned_child_count = kept;
}

// Lists a running child for later reaping.
void abandon(const i64 child) noexcept {
    reap_abandoned();
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.children_lock);
    if (process.abandoned_child_count == state::abandoned_child_capacity) {
        process.abandoned_children_lost = process.abandoned_children_lost + 1;
        return;
    }
    process.abandoned_children[process.abandoned_child_count] = child;
    process.abandoned_child_count = process.abandoned_child_count + 1;
}

// Owns a posix_spawn_file_actions_t and a posix_spawnattr_t for one start.
class SpawnSetup final {
public:
    SpawnSetup() noexcept = default;
    SpawnSetup(const SpawnSetup&) = delete;
    SpawnSetup& operator=(const SpawnSetup&) = delete;
    ~SpawnSetup() {
        if (actions_ready) (void)posix_spawn_file_actions_destroy(&actions);
        if (attributes_ready) (void)posix_spawnattr_destroy(&attributes);
    }

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actions_ready{false};
    bool attributes_ready{false};
};

// Owns a descriptor for one start; -1 holds none.
class Descriptor final {
public:
    Descriptor() noexcept = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        if (value >= 0) (void)::close(value);
    }

    [[nodiscard]] int release() noexcept {
        const int result = value;
        value = -1;
        return result;
    }

    int value{-1};
};

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
#define STXLIB_HAS_PIPE2 1
#endif

// Opens the capture pipe with both ends closing on exec (audit A, H5): the
// parent's ends must not leak into this or any other child (the child
// receives the writer through dup2, which clears the flag). With pipe2 the
// flag is set as the pipe is made. Without it (Apple hosts), pipe and the
// two fcntl calls run under the environment lock, which every STXlib spawn
// holds, so no STXlib child can inherit an end; a fork by foreign code in
// another thread between pipe and fcntl still can, and its child then holds
// the writer until it closes it or execs. Yields 0 or the errno.
int open_capture_pipe(Descriptor& reader, Descriptor& writer) noexcept {
    int ends[2] = {-1, -1};
#if defined(STXLIB_HAS_PIPE2)
    if (::pipe2(ends, O_CLOEXEC) != 0) return errno;
    reader.value = ends[0];
    writer.value = ends[1];
    return 0;
#else
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.environment_lock);
    if (::pipe(ends) != 0) return errno;
    reader.value = ends[0];
    writer.value = ends[1];
    if (::fcntl(reader.value, F_SETFD, FD_CLOEXEC) != 0) return errno;
    if (::fcntl(writer.value, F_SETFD, FD_CLOEXEC) != 0) return errno;
    return 0;
#endif
}

// Signals whose disposition the child resets to the default.
constexpr int reset_signals[] = {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGWINCH};

int prepare(SpawnSetup& setup, const ChildRequest& request, const int output_writer) noexcept {
    int error = posix_spawn_file_actions_init(&setup.actions);
    if (error != 0) return error;
    setup.actions_ready = true;
    error = posix_spawnattr_init(&setup.attributes);
    if (error != 0) return error;
    setup.attributes_ready = true;
    error = posix_spawn_file_actions_addopen(&setup.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (error == 0 && request.streams == ChildStreams::discard) {
        error = posix_spawn_file_actions_addopen(&setup.actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        if (error == 0) error = posix_spawn_file_actions_adddup2(&setup.actions, STDOUT_FILENO, STDERR_FILENO);
    }
    if (error == 0 && request.streams == ChildStreams::capture) {
        error = posix_spawn_file_actions_adddup2(&setup.actions, output_writer, STDOUT_FILENO);
        if (error == 0) error = posix_spawn_file_actions_adddup2(&setup.actions, output_writer, STDERR_FILENO);
    }
#if STXLIB_SPAWN_CLOEXEC_DEFAULT
    // Under POSIX_SPAWN_CLOEXEC_DEFAULT the parent's output and error
    // survive only when named.
    if (error == 0 && request.streams == ChildStreams::inherit) {
        error = posix_spawn_file_actions_addinherit_np(&setup.actions, STDOUT_FILENO);
        if (error == 0) error = posix_spawn_file_actions_addinherit_np(&setup.actions, STDERR_FILENO);
    }
#endif
#if STXLIB_SPAWN_CLOSEFROM
    // After the dup2s above, so the pipe's writer (close-on-exec in any
    // case) has already been copied to 1 and 2.
    if (error == 0) error = posix_spawn_file_actions_addclosefrom_np(&setup.actions, STDERR_FILENO + 1);
#endif
    if (error != 0) return error;
    sigset_t defaults{};
    (void)sigemptyset(&defaults);
    for (const int signal_number : reset_signals) (void)sigaddset(&defaults, signal_number);
    sigset_t empty{};
    (void)sigemptyset(&empty);
    error = posix_spawnattr_setsigdefault(&setup.attributes, &defaults);
    if (error == 0) error = posix_spawnattr_setsigmask(&setup.attributes, &empty);
    if (error != 0) return error;
    short flags = POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK;
#if STXLIB_SPAWN_CLOEXEC_DEFAULT
    flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
#if defined(POSIX_SPAWN_SETSID)
    if (request.detached) flags = static_cast<short>(flags | POSIX_SPAWN_SETSID);
#else
    // Without POSIX_SPAWN_SETSID, a new process group still keeps terminal
    // signals from reaching a detached child.
    if (request.detached) flags = static_cast<short>(flags | POSIX_SPAWN_SETPGROUP);
#endif
    error = posix_spawnattr_setflags(&setup.attributes, flags);
    return error;
}

enum class Appended : u8 { ok, unrepresentable, exhausted };

// Appends the host bytes `text` names (children.hpp): its UTF-8 as itself,
// and each escape (U+DC80..U+DCFF, U+DC3A ':' and U+DC5C '\', the
// surrogates Path's POSIX escape makes) as its byte. Any other surrogate
// names no POSIX string. Unlike Path::posix_name, Unicode text may hold ':'
// and '\' itself (an argument is not a path), so text without a surrogate
// is copied unchanged.
Appended append_host_bytes(const PathText text, Growable<char>& strings) noexcept {
    if (!wtf8::has_surrogate(text.data(), text.count())) {
        if (!strings.append(text.data(), text.count())) return Appended::exhausted;
        return Appended::ok;
    }
    usize index = 0;
    while (index < text.count()) {
        // PathText is well-formed WTF-8, so every sequence has a length.
        const usize length = wtf8::sequence_length(text.data(), text.count(), index);
        const u32 code_point = wtf8::decode(text.data(), index, length);
        if (wtf8::is_surrogate(code_point)) {
            const bool escape = (code_point >= 0xdc80U && code_point <= 0xdcffU) || code_point == 0xdc3aU ||
                                code_point == 0xdc5cU;
            if (!escape) return Appended::unrepresentable;
            if (!strings.push(static_cast<char>(code_point - 0xdc00U))) return Appended::exhausted;
        } else if (!strings.append(text.data() + index, length)) {
            return Appended::exhausted;
        }
        index = index + length;
    }
    return Appended::ok;
}

bool contains_slash(const PathText text) noexcept {
    for (usize index = 0; index < text.count(); ++index) {
        if (text.data()[index] == '/') return true;
    }
    return false;
}

// Sleeps for `interval` (at least 0), retrying interrupted sleeps.
void sleep_for(const Duration interval) noexcept {
    const i64 nanoseconds = interval.count_nanoseconds() < 0 ? 0 : interval.count_nanoseconds();
    struct timespec remaining{};
    remaining.tv_sec = static_cast<time_t>(nanoseconds / 1000000000);
    remaining.tv_nsec = static_cast<long>(nanoseconds % 1000000000);
    while (::nanosleep(&remaining, &remaining) != 0 && errno == EINTR) {
    }
}

} // namespace

void Child::release() noexcept {
    // Lets go of each handle before closing or reaping it (AGENTS.md, rule 1).
    const i64 output = output_;
    output_ = none;
    if (output != none) (void)::close(static_cast<int>(output));
    const i64 process = process_;
    const Phase phase = phase_;
    process_ = none;
    phase_ = Phase::empty;
    if (phase != Phase::running) return;
    // A child that has ended is reaped now; a running one is listed.
    int status = 0;
    if (::waitpid(process_id(process), &status, WNOHANG) != 0) return;
    abandon(process);
}

Result<Child> Child::start(const ChildRequest& request) noexcept {
    const Operation operation = "stxlib.children.start";
    const Status checked = children_detail::check_request(request);
    if (checked.failed()) return Result<Child>::failure(*checked.error());
    reap_abandoned();
    // The program and arguments as NUL-terminated strings in one buffer,
    // then the argv pointers into it (made after the last append, which may
    // move the buffer).
    // Each string is its host bytes (append_host_bytes).
    Growable<char> strings;
    Growable<usize> starts;
    const ErrorRecord exhausted = host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none);
    const ErrorRecord unrepresentable =
        host_failure::record(ErrorKind::invalid_argument, operation, HostCode::invalid_value);
    for (usize index = 0; index <= request.arguments.count(); ++index) {
        const PathText text = index == 0 ? request.program : request.arguments[index - 1];
        if (!starts.push(strings.count())) return Result<Child>::failure(exhausted);
        const Appended appended = append_host_bytes(text, strings);
        if (appended == Appended::unrepresentable) return Result<Child>::failure(unrepresentable);
        if (appended != Appended::ok || !strings.push('\0')) return Result<Child>::failure(exhausted);
    }
    Growable<char*> argv;
    for (usize index = 0; index < starts.count(); ++index) {
        if (!argv.push(strings.data() + starts[index])) return Result<Child>::failure(exhausted);
    }
    if (!argv.push(nullptr)) return Result<Child>::failure(exhausted);

    Descriptor reader;
    Descriptor writer;
    if (request.streams == ChildStreams::capture) {
        const int opened = open_capture_pipe(reader, writer);
        if (opened != 0) return Result<Child>::failure(record_of_errno(opened, operation));
    }
    SpawnSetup setup;
    const int prepared = prepare(setup, request, writer.value);
    if (prepared != 0) return Result<Child>::failure(record_of_errno(prepared, operation));

    pid_t pid = -1;
    int error = 0;
    {
        state::ProcessState& process = state::current_process();
        // POSIX lets posix_spawn be a cancellation point (audit E, 4.2).
        const detail::CancellationHold no_cancel;
        const spin::Guard lock(process.environment_lock);
        char* const* const environment = environment_block();
        if (contains_slash(request.program)) {
            error = posix_spawn(&pid, argv[0], &setup.actions, &setup.attributes, argv.data(), environment);
        } else {
            error = posix_spawnp(&pid, argv[0], &setup.actions, &setup.attributes, argv.data(), environment);
        }
    }
    if (error != 0) return Result<Child>::failure(record_of_errno(error, operation));
    Child child;
    child.process_ = static_cast<i64>(pid);
    child.phase_ = Phase::running;
    child.output_ = request.streams == ChildStreams::capture ? static_cast<i64>(reader.release()) : none;
    Result<Child> result = Result<Child>::success(static_cast<Child&&>(child));
    return result;
}

Result<ChildEnd> Child::wait(const Duration timeout) noexcept {
    const Operation operation = "stxlib.children.wait";
    if (phase_ != Phase::running) return Result<ChildEnd>::failure(children_detail::invalid_state(operation));
    reap_abandoned();
    const IntervalTime deadline = IntervalTime::now().after(timeout.negative() ? Duration() : timeout);
    Duration pause = Duration::microseconds(200);
    for (;;) {
        int status = 0;
        const pid_t reaped = ::waitpid(process_id(process_), &status, WNOHANG);
        if (reaped < 0) {
            const int error = errno;
            if (error == EINTR) continue;
            // ECHILD: the child was reaped elsewhere (SIGCHLD ignored,
            // SA_NOCLDWAIT, or another waitpid), so its process id may
            // already name another process. The Child leaves the running
            // phase, its end unknown, so kill can never signal that id
            // (A-stxlib-hosted-01-F5).
            if (error == ECHILD) phase_ = Phase::waited;
            return Result<ChildEnd>::failure(record_of_errno(error, operation));
        }
        if (reaped == process_id(process_)) {
            phase_ = Phase::waited;
            if (WIFSIGNALED(status)) {
                return Result<ChildEnd>::success(ChildEnd::signalled(static_cast<u32>(WTERMSIG(status))));
            }
            return Result<ChildEnd>::success(ChildEnd::exited(static_cast<u32>(WEXITSTATUS(status))));
        }
        const IntervalTime now = IntervalTime::now();
        if (deadline <= now) {
            return Result<ChildEnd>::failure(
                host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::timed_out));
        }
        const Duration left = deadline.since(now);
        sleep_for(left < pause ? left : pause);
        if (pause < Duration::milliseconds(10)) pause = pause.plus(pause);
    }
}

Status Child::kill() noexcept {
    const Operation operation = "stxlib.children.kill";
    if (phase_ != Phase::running) return Status::failure(children_detail::invalid_state(operation));
    if (::kill(process_id(process_), SIGKILL) != 0) return Status::failure(record_of_errno(errno, operation));
    return Status::success();
}

Result<Buffer<Byte>> Child::read_output(const usize limit) noexcept {
    const Operation operation = "stxlib.children.read_output";
    if (output_ == none) return Result<Buffer<Byte>>::failure(children_detail::invalid_state(operation));
    const int descriptor = static_cast<int>(output_);
    Growable<Byte> output;
    bool over_limit = false;
    Byte chunk[4096]{};
    int failed_errno = 0;
    bool exhausted = false;
    for (;;) {
        const ssize_t count = ::read(descriptor, chunk, sizeof chunk);
        if (count < 0) {
            if (errno == EINTR) continue;
            failed_errno = errno;
            break;
        }
        if (count == 0) break;
        if (!children_detail::keep_output(chunk, static_cast<usize>(count), limit, output, over_limit)) {
            exhausted = true;
            break;
        }
    }
    output_ = none; // let go before closing (AGENTS.md, rule 1)
    (void)::close(descriptor);
    if (failed_errno != 0) return Result<Buffer<Byte>>::failure(record_of_errno(failed_errno, operation));
    if (exhausted) {
        return Result<Buffer<Byte>>::failure(host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none));
    }
    Result<Buffer<Byte>> result = children_detail::finish_output(output, over_limit, operation);
    return result;
}

} // namespace stxlib
