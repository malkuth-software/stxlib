#pragma once

// A wake channel (Capability::wake_channel): a waitable flag that any
// thread, and on POSIX hosts a signal handler, can raise, and that an event
// loop waits on together with the host handles it serves (an X11
// connection, a terminal). Replaces GUI.Forms' self-pipe in the Linux host
// (src/host/linux/application/linux_host.cpp at 491156eac: pipe, fcntl
// O_NONBLOCK and FD_CLOEXEC, poll, read, write; A0 inventory, section 3).
// A1 platform-rest.
//
// POSIX hosts: a pipe whose ends are non-blocking and close-on-exec
// (pipe2 with O_CLOEXEC | O_NONBLOCK on Linux and FreeBSD,
// sys/kern/sys_pipe.c:550-554 at the pin; pipe and fcntl on macOS, which
// has no pipe2); wake writes one byte (write(2) is async-signal-safe,
// POSIX.1-2017 2.4.3; a full pipe already holds a wake, so EAGAIN is
// success); wait polls the read end with the caller's descriptors.
// Windows: a manual-reset event (CreateEventW); wake is SetEvent; wait is
// WaitForMultipleObjects over the event and the caller's handles.
// Emscripten: a 32-bit flag in wasm memory and Emscripten's futex; a page
// has no descriptors, so wait takes no handles there (a non-empty list
// fails with unsupported) and native_handle is the flag's address, which
// no other wait can watch.
//
// Order: drain the channel before doing the work its wakes announce, so a
// wake raised during the work is seen by the next wait, never lost.
//
// Handles are the host's: descriptors on POSIX hosts, HANDLE values on
// Windows, carried as i64 (no platform type crosses this header). A
// channel's own native_handle() may be waited on by another channel.

#include "stxlib/clocks.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// Why a wait returned. One state at a time; ready_handle names the handle
// only in its own state (house style, section 2).
class WakeWait final {
public:
    [[nodiscard]] static constexpr WakeWait woken() noexcept { return WakeWait(Reason::woken, 0); }
    [[nodiscard]] static constexpr WakeWait timed_out() noexcept { return WakeWait(Reason::timed_out, 0); }
    // A POSIX signal arrived during the wait (poll returned EINTR): check
    // the process events (events.hpp), then wait again.
    [[nodiscard]] static constexpr WakeWait interrupted() noexcept { return WakeWait(Reason::interrupted, 0); }
    [[nodiscard]] static constexpr WakeWait handle_ready(const usize index) noexcept {
        return WakeWait(Reason::handle_ready, index);
    }

    [[nodiscard]] constexpr bool is_woken() const noexcept { return reason_ == Reason::woken; }
    [[nodiscard]] constexpr bool is_timed_out() const noexcept { return reason_ == Reason::timed_out; }
    [[nodiscard]] constexpr bool is_interrupted() const noexcept { return reason_ == Reason::interrupted; }
    // The index into the wait's handles of the first ready one, or null in
    // any other state. Borrowed from this value.
    [[nodiscard]] constexpr const usize* ready_handle() const& noexcept {
        if (reason_ != Reason::handle_ready) return nullptr;
        return &index_;
    }
    const usize* ready_handle() const&& = delete;

private:
    enum class Reason : u8 {
        woken = 0,
        timed_out = 1,
        interrupted = 2,
        handle_ready = 3,
    };

    constexpr WakeWait(const Reason wait_reason, const usize ready_index) noexcept
        : reason_(wait_reason), index_(ready_index) {}

    Reason reason_;
    usize index_;
};

// The most handles one wait takes besides the channel's own (Windows'
// MAXIMUM_WAIT_OBJECTS is 64, the channel's event included).
inline constexpr usize maximum_wake_wait_handles = 63;

class WakeChannel final {
public:
    // Failures: limit_exceeded or foreign_failure with the host's code when
    // the host refuses a pipe or an event; unsupported where
    // Capability::wake_channel is false.
    [[nodiscard]] static Result<WakeChannel> create() noexcept;

    WakeChannel() noexcept = default;
    ~WakeChannel();
    WakeChannel(const WakeChannel&) = delete;
    WakeChannel& operator=(const WakeChannel&) = delete;
    WakeChannel(WakeChannel&& other) noexcept;
    // Closes this channel first.
    WakeChannel& operator=(WakeChannel&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return read_handle_ != closed; }

    // Raises the flag. Safe from any thread while the channel is alive, and
    // on POSIX hosts from a signal handler (one write(2), nothing else). Does
    // nothing on a closed channel.
    void wake() const noexcept;

    // Lowers the flag; true when it was raised. Does not block.
    [[nodiscard]] bool drain() noexcept;

    // Waits until the flag is raised, one of `handles` is readable
    // (signalled, on Windows), or `timeout` has passed on IntervalTime; a
    // non-positive timeout polls once. The flag stays raised (drain lowers
    // it); the channel is reported before the handles. Failures:
    // limit_exceeded with HostCode::too_many_handles for more than
    // maximum_wake_wait_handles handles; disposed with HostCode::closed_handle
    // on a closed channel; foreign_failure with the host's code (a handle
    // the host refuses).
    [[nodiscard]] Result<WakeWait> wait(View<i64> handles, Duration timeout) noexcept;

    // The handle another wait can watch: the pipe's read end, or the event.
    // Valid while the channel is open. -1 when closed.
    [[nodiscard]] i64 native_handle() const noexcept { return read_handle_; }

private:
    static constexpr i64 closed = -1;

    WakeChannel(const i64 read_handle, const i64 write_handle) noexcept
        : read_handle_(read_handle), write_handle_(write_handle) {}

    void close_handles() noexcept;

    // POSIX: the pipe's read and write descriptors. Windows: the event in
    // both. -1 when closed.
    i64 read_handle_{closed};
    i64 write_handle_{closed};
};

} // namespace stxlib
