#pragma once

// Child processes (Capability::child_processes), replacing fork, exec,
// posix_spawn, waitpid, kill and CreateProcess. Ported from stx_auxlib's
// include/stx_auxlib/children.hpp and src/hosted/children_<host>.cpp at
// b6e330a.
//
// Settled platform differences (stx_auxlib's):
// - A child starts directly from a program and an argument list; no shell
//   ever interprets them, so arguments cannot inject commands. On Windows
//   the arguments are quoted for the C runtime's standard parsing
//   (CommandLineToArgvW rules), so a child that parses its wide command
//   line that way sees exactly these UTF-16 strings. (A child that reads
//   its ANSI command line has them converted to its code page by the host,
//   which can map some characters to '"' or '\'; only the child can avoid
//   that.)
// - Windows refuses a program whose name ends in .bat or .cmd (any case,
//   trailing dots and spaces ignored) with invalid_argument,
//   HostCode::invalid_value: the host would run it through cmd.exe, which
//   reinterprets the arguments. A program that wants cmd.exe names it,
//   and owns the quoting. A ':' in a Windows program name other than after
//   a drive letter is refused the same way.
// - A program without a '/' (POSIX) or given as a bare name (Windows) is
//   searched for in PATH, and only there: on Windows the current directory
//   and the host's other default places are not searched, and PATH entries
//   that are not absolute are skipped.
// - Failure to start (a missing program, no permission) is reported by
//   start, never by a child that exits at once with status 127 (posix_spawn
//   reports a failed exec to the parent on macOS and on glibc 2.24 and
//   later; musl's posix_spawn does too).
// - Every child's standard input is the null device; its output and error
//   are inherited, discarded or captured together through one pipe.
// - No other descriptor or handle of the parent's reaches the child where
//   the host can say so: Windows lists the handles it passes; macOS
//   (POSIX_SPAWN_CLOEXEC_DEFAULT), glibc 2.34 and later and FreeBSD 13.1
//   and later (posix_spawn_file_actions_addclosefrom_np) close every
//   descriptor past 2. Elsewhere (musl, older C libraries) a descriptor
//   the program opened without close-on-exec is inherited; STXlib's own
//   are all close-on-exec.
// - Children start with default handling for SIGPIPE, SIGINT, SIGTERM,
//   SIGHUP, SIGQUIT, SIGTSTP and SIGWINCH and an empty signal mask, whatever
//   the parent ignores or blocks.
// - A child is waited for with a timeout on every host.
// - A Child destroyed without having been waited for is not left a zombie:
//   it is reaped later, without blocking, by the next start or wait.
//
// Changes from stx_auxlib: a Child keeps its host state inline (no heap
// block), so it may be moved to and destroyed in another module; the
// children abandoned before being waited for are listed in stxlib_state, a
// fixed table of 256 shared by every module (a child abandoned while the
// table is full and nothing in it has ended is left until the process ends,
// and counted); a request's strings are PathText (path.hpp): Unicode text
// (a TextView converts implicitly) passes as itself, and a surrogate carries
// a host name that is not valid Unicode losslessly, as a Path carries it
// (D13 a), so a Path's text reaches the child exactly. On a POSIX host the
// surrogates of Path's escape (U+DC80..U+DCFF, and U+DC3A and U+DC5C for ':'
// and '\') each stand for their byte; text with any other surrogate names
// no POSIX string and is refused. On Windows a surrogate is an unpaired
// UTF-16 unit, passed as itself. NUL is refused.
//
// Failures (stx_auxlib's CapabilityStatus as capabilities.hpp maps it):
// invalid_argument with HostCode::empty_request (no program) or
// HostCode::invalid_value (a NUL, or on a POSIX host a surrogate that is not
// a byte's escape); not_found; permission_denied;
// out_of_memory or limit_exceeded (process or descriptor limits);
// limit_exceeded with HostCode::timed_out from wait; invalid_argument with
// HostCode::invalid_state for a Child that is empty, already waited for, or
// not capturing; limit_exceeded with HostCode::too_long from read_output;
// io_failure; the host's code as the foreign code.

#include "stxlib/buffer.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

enum class ChildStreams : u8 {
    inherit = 0, // output and error are the parent's
    discard = 1, // output and error go to the null device
    capture = 2, // output and error go to one pipe that read_output reads
};

// Designated initializers name the parts: ChildRequest{.program = "ls",
// .arguments = View<PathText>(words)}. The views are read during start only.
struct ChildRequest final {
    // An absolute path, or a bare name searched for in PATH. A Path's text
    // (Path::text()) is passed as the host names it.
    PathText program{};
    // The arguments after the program name; none may contain NUL. Unicode
    // text, or a Path's text (see above).
    View<PathText> arguments{};
    ChildStreams streams{ChildStreams::inherit};
    // Starts the child in its own session (POSIX) or process group without a
    // console (Windows), so that terminal signals and the parent's console do
    // not reach it. For programs that outlive the parent.
    bool detached{false};
};

// How a child ended: an exit status, or (POSIX hosts) the number of the
// signal that ended it. Never both.
class ChildEnd final {
public:
    [[nodiscard]] static ChildEnd exited(const u32 status) noexcept {
        const ChildEnd result(false, status);
        return result;
    }
    [[nodiscard]] static ChildEnd signalled(const u32 signal_number) noexcept {
        const ChildEnd result(true, signal_number);
        return result;
    }

    // The exit status; null when a signal ended the child. Borrowed from
    // this value.
    [[nodiscard]] const u32* exit_status() const& noexcept { return signalled_ ? nullptr : &value_; }
    const u32* exit_status() const&& = delete;
    // The signal number; null when the child exited. Borrowed from this
    // value.
    [[nodiscard]] const u32* signal() const& noexcept { return signalled_ ? &value_ : nullptr; }
    const u32* signal() const&& = delete;

private:
    ChildEnd(const bool by_signal, const u32 value) noexcept : signalled_(by_signal), value_(value) {}

    bool signalled_;
    u32 value_;
};

// One started child process, owned. Move-only; a moved-from Child is empty.
class Child final {
public:
    [[nodiscard]] static Result<Child> start(const ChildRequest& request) noexcept;

    Child() noexcept = default;
    ~Child();
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept;
    // Releases this child first, as the destructor does.
    Child& operator=(Child&& other) noexcept;

    // Whether this owns a started child (running, or ended and waited for).
    [[nodiscard]] bool started() const noexcept { return phase_ != Phase::empty; }

    // Waits until the child ends or `timeout` passes (limit_exceeded with
    // HostCode::timed_out; the child keeps running and can be waited for
    // again). timed_out is reported only once IntervalTime::now() has
    // reached the wait's start plus `timeout` (saturating), on every host;
    // the wait may end later than that, never earlier. A zero or negative
    // timeout polls once. After a successful
    // wait, later calls fail with HostCode::invalid_state. On POSIX hosts,
    // a child reaped elsewhere (SIGCHLD ignored, SA_NOCLDWAIT, or another
    // waitpid) fails the wait with the host's ECHILD, its end unknown, and
    // the Child counts as waited: wait and kill then fail with
    // invalid_state, so its process id, which may name another process by
    // then, is never signalled.
    [[nodiscard]] Result<ChildEnd> wait(Duration timeout) noexcept;
    // Ends the child forcibly (SIGKILL, TerminateProcess). Still wait for it.
    [[nodiscard]] Status kill() noexcept;
    // The child's captured output and error, read until the child closes
    // them. More than `limit` bytes: limit_exceeded with HostCode::too_long,
    // after the rest is drained and discarded. Read before waiting: a child
    // blocked on a full pipe never ends. Once only; HostCode::invalid_state
    // unless started with capture. The output is returned in a Buffer of its
    // own (one read per child, bounded by `limit`; house style §6, audit A,
    // X10), whose block may be up to twice its count (the read's growth).
    [[nodiscard]] Result<Buffer<Byte>> read_output(usize limit) noexcept;

private:
    static constexpr i64 none = -1;

    // The child's state (audit A, X3: one enum in place of a flag beside the
    // process): empty, no process and process_ is none; running, process_
    // holds it; waited, it ended and was waited for, and process_ still
    // holds the Windows handle until release. Only start, wait, release and
    // the moves write it, together with process_.
    enum class Phase : u8 {
        empty = 0,
        running = 1,
        waited = 2,
    };

    void release() noexcept;

    // A POSIX process id or a Windows process HANDLE's value; -1 when empty.
    i64 process_{none};
    // The read end of the captured output pipe (descriptor or HANDLE), or -1;
    // a resource of its own, released by read_output or release.
    i64 output_{none};
    Phase phase_{Phase::empty};
};

} // namespace stxlib
