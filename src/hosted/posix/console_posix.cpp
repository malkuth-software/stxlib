// The POSIX console backend (hosted/console_backend.hpp) over descriptors
// 0, 1 and 2, and the host's standard-error writer, thread yield and brief
// sleep (host.hpp), shared by macOS, glibc and musl. Ported from
// stx_auxlib's src/hosted/console_posix.cpp at b6e330a.

#include "hosted/console_backend.hpp"
#include "hosted/host.hpp"

#include <cerrno>
#include <cstddef>
#include <ctime>

#include <pthread.h>
#include <sched.h>
#include <sys/uio.h>
#include <unistd.h>

namespace stxlib {

namespace host {

void write_standard_error(const char* const bytes, const usize count) noexcept {
    usize written = 0;
    while (written < count) {
        const ssize_t result = ::write(STDERR_FILENO, bytes + written, count - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (result == 0) return;
        written += static_cast<usize>(result);
    }
}

void hold_cancellation() noexcept {
    int previous = 0;
    static_cast<void>(::pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous));
}

void yield_thread() noexcept { static_cast<void>(::sched_yield()); }

void sleep_briefly() noexcept {
    struct timespec interval{};
    interval.tv_sec = 0;
    interval.tv_nsec = 1000000;
    static_cast<void>(::nanosleep(&interval, nullptr));
}

} // namespace host

namespace console_backend {

namespace {

int descriptor(const ConsoleStream stream) noexcept {
    if (stream == ConsoleStream::error) return STDERR_FILENO;
    return STDOUT_FILENO;
}

Report report_errno(const int error) noexcept {
    const Outcome outcome = (error == EPIPE || error == EBADF) ? Outcome::closed : Outcome::io_error;
    const Report result{outcome, ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error))};
    return result;
}

} // namespace

Report write(const ConsoleStream stream, const char* const bytes, const usize count) noexcept {
    const int fd = descriptor(stream);
    usize written = 0;
    while (written < count) {
        const ssize_t result = ::write(fd, bytes + written, count - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return report_errno(errno);
        }
        if (result == 0) return Report{Outcome::io_error, ForeignCode::none()};
        written += static_cast<usize>(result);
    }
    return Report{};
}

// One writev for the text and its newline; a partial write finishes with
// plain writes of what is left.
Report write_line(const ConsoleStream stream, const char* const bytes, const usize count) noexcept {
    const int fd = descriptor(stream);
    char newline = '\n';
    iovec pieces[2]{};
    pieces[0].iov_base = const_cast<char*>(bytes);
    pieces[0].iov_len = count;
    pieces[1].iov_base = &newline;
    pieces[1].iov_len = 1;
    ssize_t result = ::writev(fd, pieces, 2);
    while (result < 0 && errno == EINTR) result = ::writev(fd, pieces, 2);
    if (result < 0) return report_errno(errno);
    const usize written = static_cast<usize>(result);
    if (written == count + 1) return Report{};
    if (written < count) {
        const Report rest = write(stream, bytes + written, count - written);
        if (rest.outcome != Outcome::ok) return rest;
    }
    const Report last = write(stream, "\n", 1);
    return last;
}

bool is_terminal(const ConsoleStream stream) noexcept {
    const bool result = ::isatty(descriptor(stream)) == 1;
    return result;
}

Report read(char* const buffer, const usize capacity, usize& transferred) noexcept {
    transferred = 0;
    for (;;) {
        const ssize_t result = ::read(STDIN_FILENO, buffer, capacity);
        if (result >= 0) {
            transferred = static_cast<usize>(result);
            return Report{};
        }
        if (errno != EINTR) return report_errno(errno);
    }
}

} // namespace console_backend

} // namespace stxlib
