// Anonymous pipes on POSIX hosts (include/stxlib/pipes.hpp), shared by
// macOS, glibc, musl and FreeBSD: pipe2 or pipe, read(2), write(2), dup and
// dup2. Host facts (POSIX.1-2017, as published; not re-read at a pin):
//   - read(2) on a pipe blocks until data is available and returns 0 once
//     no process holds the write end open; EINTR is retried.
//   - write(2) on a pipe with no reader fails with EPIPE (and raises
//     SIGPIPE unless ignored: create_pipe and claim_protocol_streams
//     ignore it first, ignore_broken_pipes in events_posix.cpp).
//   - fcntl(F_DUPFD_CLOEXEC, 3) duplicates a descriptor at 3 or above with
//     close-on-exec set atomically (POSIX.1-2008).
//   - dup2(2, 1) makes descriptor 1 refer to standard error's file.
// Close-on-exec of the pipe itself: as wake_posix.cpp (pipe2 with
// O_CLOEXEC; macOS takes pipe and then fcntl).

#include "stxlib/capabilities.hpp"
#include "stxlib/detail/cancellation.hpp"
#include "stxlib/events.hpp"
#include "stxlib/pipes.hpp"

#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace stxlib {

namespace {

ForeignCode errno_code(const int error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    return result;
}

int descriptor(const i64 handle) noexcept { return static_cast<int>(handle); }

ErrorKind kind_of_open_failure(const int error) noexcept {
    const ErrorKind result = error == EMFILE || error == ENFILE ? ErrorKind::limit_exceeded : ErrorKind::foreign_failure;
    return result;
}

bool open_pipe(int (&ends)[2]) noexcept {
#if defined(__APPLE__)
    if (::pipe(ends) != 0) return false;
    for (const int end : ends) {
        if (::fcntl(end, F_SETFD, FD_CLOEXEC) != 0) {
            const int error = errno;
            static_cast<void>(::close(ends[0]));
            static_cast<void>(::close(ends[1]));
            errno = error;
            return false;
        }
    }
    return true;
#else
    const bool result = ::pipe2(ends, O_CLOEXEC) == 0;
    return result;
#endif
}

void close_descriptor(const int value) noexcept {
    // close(2) is not retried on EINTR: on Linux and the BSDs the
    // descriptor is released even then, and a retry could close a
    // descriptor another thread has just been given.
    static_cast<void>(::close(value));
}

} // namespace

void PipeReader::close() noexcept {
    if (handle_ == closed) return;
    const int value = descriptor(handle_);
    handle_ = closed; // let go before releasing (AGENTS.md, rule 1)
    close_descriptor(value);
}

OperationResult<PipeRead> PipeReader::read(const MutableView<Byte> into) noexcept {
    const Operation operation = "stxlib.pipes.read";
    STXLIB_REQUIRE(!into.empty(), "PipeReader::read: no room");
    if (handle_ == closed) {
        return OperationResult<PipeRead>::failure(host_failure::record(ErrorKind::disposed, operation,
                                                                       HostCode::closed_handle));
    }
    for (;;) {
        const ssize_t got = ::read(descriptor(handle_), into.data(), into.count());
        if (got > 0) return OperationResult<PipeRead>::success(PipeRead::bytes(static_cast<usize>(got)));
        if (got == 0) return OperationResult<PipeRead>::success(PipeRead::ended());
        const int error = errno;
        if (error == EINTR) continue;
        return OperationResult<PipeRead>::failure(
            host_failure::record(ErrorKind::io_failure, operation, HostCode::none, errno_code(error)));
    }
}

void PipeWriter::close() noexcept {
    if (handle_ == closed) return;
    const int value = descriptor(handle_);
    handle_ = closed;
    close_descriptor(value);
}

OperationStatus PipeWriter::write_all(const View<Byte> bytes) noexcept {
    const Operation operation = "stxlib.pipes.write";
    if (handle_ == closed) return host_failure::failed(ErrorKind::disposed, operation, HostCode::closed_handle);
    usize written = 0;
    while (written < bytes.count()) {
        const ssize_t result = ::write(descriptor(handle_), bytes.data() + written, bytes.count() - written);
        if (result > 0) {
            written += static_cast<usize>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        const int error = result < 0 ? errno : EIO;
        if (error == EPIPE) {
            return host_failure::failed(ErrorKind::io_failure, operation, HostCode::stream_closed, errno_code(error));
        }
        return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(error));
    }
    return OperationStatus::success();
}

OperationResult<PipePair> create_pipe() noexcept {
    const Operation operation = "stxlib.pipes.create";
    if (!capability_supported(Capability::anonymous_pipes)) {
        return OperationResult<PipePair>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                       HostCode::none));
    }
    ignore_broken_pipes();
    int ends[2] = {-1, -1};
    if (!open_pipe(ends)) {
        const int error = errno;
        return OperationResult<PipePair>::failure(
            host_failure::record(kind_of_open_failure(error), operation, HostCode::none, errno_code(error)));
    }
    PipePair pair{};
    pair.reader = PipeReader::adopt(static_cast<i64>(ends[0]));
    pair.writer = PipeWriter::adopt(static_cast<i64>(ends[1]));
    return OperationResult<PipePair>::success(static_cast<PipePair&&>(pair));
}

OperationResult<ProtocolStreams> claim_protocol_streams() noexcept {
    const Operation operation = "stxlib.pipes.claim_protocol_streams";
    if (!capability_supported(Capability::anonymous_pipes)) {
        return OperationResult<ProtocolStreams>::failure(host_failure::record(ErrorKind::unsupported, operation,
                                                                              HostCode::none));
    }
    state::ProcessState& process = state::current_process();
    if (process.protocol_streams_claimed.exchange(true, std::memory_order_acq_rel)) {
        return OperationResult<ProtocolStreams>::failure(host_failure::record(ErrorKind::limit_exceeded, operation,
                                                                              HostCode::busy));
    }
    ignore_broken_pipes();
    const int input = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    if (input < 0) {
        const int error = errno;
        process.protocol_streams_claimed.store(false, std::memory_order_release);
        const ErrorKind kind = error == EBADF ? ErrorKind::io_failure : kind_of_open_failure(error);
        const HostCode code = error == EBADF ? HostCode::stream_closed : HostCode::none;
        return OperationResult<ProtocolStreams>::failure(host_failure::record(kind, operation, code, errno_code(error)));
    }
    // The console's output lock keeps write_console's text whole across the
    // switch; the C library's stdout buffer is flushed first, so what the
    // program printed before the claim goes where it was meant to.
    const detail::CancellationHold no_cancel; // fflush and dup2 under a lock (audit E, 4.1)
    const spin::Guard lock(process.console_output_lock);
    host::flush_c_stream(false);
    const int output = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
    if (output < 0) {
        const int error = errno;
        close_descriptor(input);
        process.protocol_streams_claimed.store(false, std::memory_order_release);
        const ErrorKind kind = error == EBADF ? ErrorKind::io_failure : kind_of_open_failure(error);
        const HostCode code = error == EBADF ? HostCode::stream_closed : HostCode::none;
        return OperationResult<ProtocolStreams>::failure(host_failure::record(kind, operation, code, errno_code(error)));
    }
    for (;;) {
        if (::dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) break;
        const int error = errno;
        if (error == EINTR) continue;
        close_descriptor(output);
        close_descriptor(input);
        process.protocol_streams_claimed.store(false, std::memory_order_release);
        return OperationResult<ProtocolStreams>::failure(
            host_failure::record(ErrorKind::foreign_failure, operation, HostCode::none, errno_code(error)));
    }
    ProtocolStreams streams{};
    streams.input = PipeReader::adopt(static_cast<i64>(input));
    streams.output = PipeWriter::adopt(static_cast<i64>(output));
    return OperationResult<ProtocolStreams>::success(static_cast<ProtocolStreams&&>(streams));
}

} // namespace stxlib
