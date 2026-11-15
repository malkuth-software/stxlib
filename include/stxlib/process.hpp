#pragma once

// Ending the process (Capability::exit_tasks), replacing exit and atexit.
// Ported from stx_auxlib's include/stx_auxlib/process.hpp at b6e330a.
//
// Settled platform differences:
// - Exit statuses are 0 to 255: POSIX hosts keep only the low eight bits of
//   a wider status, so a wider one would read differently by host.
// - Exit tasks take a context, which atexit's do not.
//
// stx_auxlib's fail() and ensure() are not carried: STXlib's fault path is
// STXLIB_REQUIRE (require.hpp) and fail_fast (fail_fast.hpp), which report
// one line on standard error naming the source location and end the
// process abnormally, and are never compiled out.
//
// Modules: the task table lives in stxlib_state, so a task registered in
// any module runs once. The handler that runs them is registered with the
// C library's atexit by the module that registers the first task; on
// Windows, atexit in a DLL runs when that DLL is unloaded, which for a DLL
// loaded at start-up is during process exit.

#include "stxlib/buffer.hpp"
#include "stxlib/callback.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// Ends the process with `status`: runs the exit tasks (newest first), then
// the C and C++ library's own exit handling (static destructors, atexit),
// which flushes standard streams. Call it only when no other thread can be
// using the objects that static destructors destroy. Emscripten: only a
// build linked with EXIT_RUNTIME ends the runtime (Capability::exit_tasks);
// otherwise, and on a worker thread, exit unwinds to the JavaScript event
// loop without running the exit tasks or the cleanups of the frames it
// leaves, and the page lives on (src/hosted/c_library/c_library.cpp).
[[noreturn]] void exit_process(u8 status) noexcept;

inline constexpr usize maximum_exit_tasks = 32;

// Registers `task` to run when the process exits normally (exit_process, or
// returning from main), newest first. A task must not throw; it may
// register further tasks, which run after it. Its context is borrowed until
// it has run. Failures: invalid_argument for an unbound task;
// limit_exceeded with HostCode::table_full when all maximum_exit_tasks
// places are taken, or the C library refuses the handler.
[[nodiscard]] Status register_exit_task(CallbackTarget<void()> task) noexcept;

// ---- the process's identity, arguments and memory --------------------------
// (A1 platform-rest; the A0 inventory's "executable path and arguments" and
// "task memory report" rows: GUI.Forms' theme_files.cpp, linux_host.cpp and
// bundled_font_memory_report.cpp at 491156eac.)

// The process's identifier as the host numbers it (getpid on POSIX hosts,
// GetCurrentProcessId on Windows; Capability::process_identity). The host
// may reuse it once the process has ended, so it names a process only
// while that process runs: for reports and logs, never as an identity of
// ours (identity.hpp). 0 where Capability::process_identity is false
// (Emscripten, whose getpid answers a constant).
[[nodiscard]] u64 process_id() noexcept;

// One argument, borrowed from the ProcessArguments it came from. Its bytes
// are the host's, carried losslessly (owner decision D13 a, applied to
// arguments): on POSIX hosts the bytes the process was started with; on
// Windows WTF-8 made from the command line's UTF-16 units (an unpaired
// surrogate is carried, path.hpp). No terminating NUL is part of it.
class ArgumentText final {
public:
    constexpr ArgumentText() noexcept = default;

    // The host's bytes. Borrowed (see above).
    [[nodiscard]] constexpr View<char> bytes() const noexcept {
        const View<char> result(bytes_, count_);
        return result;
    }
    [[nodiscard]] constexpr usize count() const noexcept { return count_; }
    // True when the bytes are well-formed UTF-8 (on Windows: no unpaired
    // surrogate).
    [[nodiscard]] constexpr bool is_unicode() const noexcept { return unicode_; }
    // The argument as text. Requirement: is_unicode().
    [[nodiscard]] TextView unicode() const noexcept;

private:
    friend class ProcessArguments;

    constexpr ArgumentText(const char* const argument_bytes, const usize argument_count,
                           const bool argument_unicode) noexcept
        : bytes_(argument_bytes), count_(argument_count), unicode_(argument_unicode) {}

    const char* bytes_{nullptr};
    usize count_{0};
    bool unicode_{false};
};

namespace detail {

// One argument of a ProcessArguments: text_[offset, offset + count).
struct ArgumentRecord final {
    usize offset{0};
    usize count{0};
    bool unicode{false};
};

class ProcessArgumentsBuilder;

} // namespace detail

// The process's arguments, the program's own name first (argv[0]: as the
// starter gave it, not necessarily a path; use current_executable,
// desktop.hpp, for the file). Owns its text; move-only.
class ProcessArguments final {
public:
    ProcessArguments() noexcept = default;
    ProcessArguments(ProcessArguments&&) noexcept = default;
    ProcessArguments& operator=(ProcessArguments&&) noexcept = default;
    ProcessArguments(const ProcessArguments&) = delete;
    ProcessArguments& operator=(const ProcessArguments&) = delete;
    ~ProcessArguments() = default;

    [[nodiscard]] usize count() const noexcept { return records_.count(); }
    // Borrowed from this object. Requirement: index < count().
    [[nodiscard]] ArgumentText argument(usize index) const& noexcept;
    ArgumentText argument(usize index) const&& = delete;

private:
    friend class detail::ProcessArgumentsBuilder;

    Buffer<char> text_;
    Buffer<detail::ArgumentRecord> records_;
};

// Bounds the arguments' bytes process_arguments reads by default, above
// every host's own bound: FreeBSD's ARG_MAX is 512 KiB on 64-bit
// architectures (sys/sys/syslimits.h:45 at the pin); macOS's is 1 MiB and
// Linux allows a quarter of the stack limit (both unconfirmed at a pin);
// Windows' command line is at most 32,767 UTF-16 units.
inline constexpr usize default_argument_limit = 4194304; // 4 MiB

// The arguments, read from the host each call (Capability::process_arguments):
//   - macOS: the C runtime's own argc and argv (_NSGetArgc, _NSGetArgv);
//   - Linux (glibc, musl): /proc/self/cmdline, the kernel's view of the
//     argument area, so a program that rewrote its argv (a process title)
//     reads what it wrote;
//   - FreeBSD: sysctl kern.proc.args for this process (sys/kern/kern_proc.c
//     at the pin, sysctl_kern_proc_args), the kernel's copy, or the
//     argument area when the kernel kept none;
//   - Windows: GetCommandLineW, split by the rules the C runtime uses for
//     main's argv (src/core/command_line.hpp states them), so argv and
//     these agree.
// Failures: limit_exceeded with HostCode::too_long when the arguments hold
// more than `limit` bytes (refused before copying); out_of_memory;
// io_failure (with the host's code) when the host cannot be read;
// unsupported where Capability::process_arguments is false.
[[nodiscard]] Result<ProcessArguments> process_arguments(usize limit = default_argument_limit) noexcept;

// The process's resident memory, in bytes (Capability::memory_report).
struct ProcessMemory final {
    // Resident now: macOS task_info's resident_size, Linux /proc/self/statm,
    // FreeBSD kinfo_proc's ki_rssize, Windows' working set, Emscripten's
    // wasm memory size (which only grows, so it is also the peak).
    u64 resident_bytes{0};
    // The largest resident size the host has recorded, and at least
    // resident_bytes: hosts sample their peak (FreeBSD updates ru_maxrss
    // at statistics-clock ticks, sys/kern/kern_clock.c:742-744 at the pin),
    // so the host's own peak can lag the current size.
    u64 peak_resident_bytes{0};
};

// Failures: io_failure or foreign_failure with the host's code; unsupported
// where Capability::memory_report is false.
[[nodiscard]] Result<ProcessMemory> process_memory() noexcept;

} // namespace stxlib
