// The wake channel on POSIX hosts (include/stxlib/wake.hpp): a
// non-blocking, close-on-exec pipe and poll, shared by macOS, glibc, musl
// and FreeBSD; the shape of GUI.Forms' self-pipe (linux_host.cpp at
// 491156eac). A1 platform-rest.
//
// pipe2 with O_CLOEXEC | O_NONBLOCK sets both flags atomically, so no
// child started between the calls inherits the pipe: Linux (glibc 2.9,
// musl) and FreeBSD (sys/kern/sys_pipe.c:550-554 at the pin fb9033d2876d,
// which accepts exactly O_CLOEXEC, O_CLOFORK and O_NONBLOCK). macOS has no
// pipe2, so it takes pipe and then fcntl: a child started by another
// thread in between could inherit the descriptors (unconfirmed for
// posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT, which children_posix.cpp
// does not set).

#include "stxlib/capabilities.hpp"
#include "stxlib/wake.hpp"

#include "core/host_failure.hpp"

#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace stxlib {

namespace {

ForeignCode errno_code(const int error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    return result;
}

int descriptor(const i64 handle) noexcept { return static_cast<int>(handle); }

bool open_pipe(int (&ends)[2]) noexcept {
#if defined(__APPLE__)
    if (::pipe(ends) != 0) return false;
    for (const int end : ends) {
        const int status = ::fcntl(end, F_GETFL);
        if (status < 0 || ::fcntl(end, F_SETFL, status | O_NONBLOCK) != 0 || ::fcntl(end, F_SETFD, FD_CLOEXEC) != 0) {
            const int error = errno;
            static_cast<void>(::close(ends[0]));
            static_cast<void>(::close(ends[1]));
            errno = error;
            return false;
        }
    }
    return true;
#else
    const bool result = ::pipe2(ends, O_CLOEXEC | O_NONBLOCK) == 0;
    return result;
#endif
}

// Milliseconds for one poll: the remaining time rounded up, so a wait never
// ends before its deadline, and at most a day (longer waits poll again).
int poll_milliseconds(const Duration remaining) noexcept {
    const i64 nanoseconds = remaining.count_nanoseconds();
    if (nanoseconds <= 0) return 0;
    constexpr i64 day = 86400000;
    const i64 milliseconds = nanoseconds / 1000000 + (nanoseconds % 1000000 != 0 ? 1 : 0);
    const i64 bounded = milliseconds < day ? milliseconds : day;
    return static_cast<int>(bounded);
}

} // namespace

OperationResult<WakeChannel> WakeChannel::create() noexcept {
    const Operation operation = "stxlib.wake.create";
    if (!capability_supported(Capability::wake_channel)) {
        return OperationResult<WakeChannel>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                          HostCode::none));
    }
    int ends[2] = {-1, -1};
    if (!open_pipe(ends)) {
        const int error = errno;
        const ErrorKind kind = error == EMFILE || error == ENFILE ? ErrorKind::limit_exceeded : ErrorKind::foreign_failure;
        return OperationResult<WakeChannel>::failure(host_failure::record(kind, operation, HostCode::none,
                                                                          errno_code(error)));
    }
    return OperationResult<WakeChannel>::success(WakeChannel(static_cast<i64>(ends[0]), static_cast<i64>(ends[1])));
}

WakeChannel::~WakeChannel() { close_handles(); }

WakeChannel::WakeChannel(WakeChannel&& other) noexcept
    : read_handle_(other.read_handle_), write_handle_(other.write_handle_) {
    other.read_handle_ = closed;
    other.write_handle_ = closed;
}

WakeChannel& WakeChannel::operator=(WakeChannel&& other) noexcept {
    if (this == &other) return *this;
    close_handles();
    read_handle_ = other.read_handle_;
    write_handle_ = other.write_handle_;
    other.read_handle_ = closed;
    other.write_handle_ = closed;
    return *this;
}

void WakeChannel::wake() const noexcept {
    if (write_handle_ == closed) return;
    // Async-signal-safe: one write(2). EAGAIN means the pipe is full, so a
    // wake is already pending; EINTR leaves it to the next wake. errno is
    // restored, since a signal handler may interrupt code that reads it.
    const int saved = errno;
    const Byte one = 1;
    static_cast<void>(::write(descriptor(write_handle_), &one, 1));
    errno = saved;
}

bool WakeChannel::drain() noexcept {
    if (read_handle_ == closed) return false;
    bool raised = false;
    Byte bytes[64]{};
    for (;;) {
        const ssize_t got = ::read(descriptor(read_handle_), bytes, sizeof bytes);
        if (got > 0) {
            raised = true;
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        // 0 cannot happen while the write end is open; EAGAIN: empty.
        return raised;
    }
}

OperationResult<WakeWait> WakeChannel::wait(const View<i64> handles, const Duration timeout) noexcept {
    const Operation operation = "stxlib.wake.wait";
    if (read_handle_ == closed) {
        return OperationResult<WakeWait>::failure(host_failure::record(ErrorKind::disposed, operation,
                                                                       HostCode::closed_handle));
    }
    if (handles.count() > maximum_wake_wait_handles) {
        return OperationResult<WakeWait>::failure(
            host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::too_many_handles));
    }
    pollfd watched[maximum_wake_wait_handles + 1]{};
    watched[0].fd = descriptor(read_handle_);
    watched[0].events = POLLIN;
    for (usize index = 0; index < handles.count(); ++index) {
        watched[index + 1].fd = descriptor(handles[index]);
        watched[index + 1].events = POLLIN;
    }
    const nfds_t count = static_cast<nfds_t>(handles.count() + 1);
    const IntervalTime deadline = IntervalTime::now().after(timeout);
    for (;;) {
        const Duration remaining = deadline.since(IntervalTime::now());
        const int ready = ::poll(watched, count, poll_milliseconds(remaining));
        if (ready < 0) {
            const int error = errno;
            if (error == EINTR) return OperationResult<WakeWait>::success(WakeWait::interrupted());
            return OperationResult<WakeWait>::failure(
                host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, errno_code(error)));
        }
        if (ready > 0) {
            // POLLHUP and POLLERR count as readable: a read then reports
            // the end or the error to the handle's owner.
            constexpr short readable = POLLIN | POLLHUP | POLLERR;
            if ((watched[0].revents & readable) != 0) return OperationResult<WakeWait>::success(WakeWait::woken());
            for (usize index = 0; index < handles.count(); ++index) {
                if ((watched[index + 1].revents & POLLNVAL) != 0) {
                    return OperationResult<WakeWait>::failure(host_failure::record(ErrorKind::foreign_failure,
                                                                                   operation, HostCode::none,
                                                                                   errno_code(EBADF)));
                }
                if ((watched[index + 1].revents & readable) != 0) {
                    // macOS's poll reports a descriptor listed twice in one
                    // entry only (the last), so the first entry with the
                    // same handle is named: the first ready one.
                    usize first = 0;
                    while (handles[first] != handles[index]) ++first;
                    return OperationResult<WakeWait>::success(WakeWait::handle_ready(first));
                }
            }
        }
        if (deadline <= IntervalTime::now()) return OperationResult<WakeWait>::success(WakeWait::timed_out());
    }
}

void WakeChannel::close_handles() noexcept {
    // close is not retried on EINTR: Linux, macOS and FreeBSD release the
    // descriptor even then (file_posix.cpp's rule).
    if (read_handle_ != closed) static_cast<void>(::close(descriptor(read_handle_)));
    if (write_handle_ != closed) static_cast<void>(::close(descriptor(write_handle_)));
    read_handle_ = closed;
    write_handle_ = closed;
}

} // namespace stxlib
