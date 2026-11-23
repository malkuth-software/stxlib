#pragma once

// Anonymous pipes (Capability::anonymous_pipes): one-way byte streams with no
// name, and the process's standard streams claimed for a protocol. The MCP
// transport of stx_mcp (mcp.hpp; notes planning/studies/MCP_AUTOMATION_STUDY.md,
// sections 1.8 and 4.1, on the public notes' gui_forms branch): a client
// starts the program with two pipes as its standard input and output, and
// nothing else can reach them. No sockets, no names, nothing listening.
//
// Settled platform differences:
// - A read blocks until at least one byte arrives or every writer has
//   closed its end; then it reports the end of the stream, on every host.
//   POSIX: read(2), EINTR retried. Windows: ReadFile; ERROR_BROKEN_PIPE is
//   the end of the stream, as a POSIX read of 0 is.
// - A write writes every byte or fails. A write whose reader has closed
//   fails with io_failure, HostCode::stream_closed, and never raises a
//   signal: create_pipe and claim_protocol_streams call ignore_broken_pipes
//   (events.hpp) first, which ignores SIGPIPE for the process (POSIX; a
//   program that wants SIGPIPE back restores it after the call).
// - Every descriptor and handle made here is close-on-exec (POSIX:
//   O_CLOEXEC and F_DUPFD_CLOEXEC; macOS: pipe then FD_CLOEXEC, with the
//   window wake.hpp describes) or not inheritable (Windows), so no child
//   started by anyone inherits a protocol stream.
// - Emscripten: a page has no standard streams a client can hold, so the
//   capability is false and every call fails with unsupported.
//
// Handles are the host's (descriptors on POSIX hosts, HANDLE values on
// Windows), carried as i64; no platform type crosses this header.
//
// Ownership: a PipeReader or PipeWriter owns its handle and closes it on
// destruction. Move-only; a moved-from end is closed (is_open false).
// Thread safety: one end is used by one thread at a time; the two ends of
// one pipe may be used by two threads at once.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// The outcome of one read: some bytes, or the end of the stream. One state
// at a time; byte_count names the count only in its own state.
class PipeRead final {
public:
    // Requirement: count > 0 (a read that returns has at least one byte or
    // is the end).
    [[nodiscard]] static PipeRead bytes(usize count) noexcept;
    [[nodiscard]] static PipeRead ended() noexcept {
        const PipeRead result(Kind::ended, 0);
        return result;
    }

    [[nodiscard]] bool is_end() const noexcept { return kind_ == Kind::ended; }
    // The bytes read, or null at the end of the stream. Borrowed from this
    // value.
    [[nodiscard]] const usize* byte_count() const& noexcept {
        if (kind_ != Kind::bytes) return nullptr;
        return &count_;
    }
    const usize* byte_count() const&& = delete;

private:
    enum class Kind : u8 {
        bytes = 0,
        ended = 1,
    };

    PipeRead(const Kind read_kind, const usize count) noexcept : kind_(read_kind), count_(count) {}

    Kind kind_;
    usize count_;
};

class PipeReader final {
public:
    PipeReader() noexcept = default;
    ~PipeReader();
    PipeReader(const PipeReader&) = delete;
    PipeReader& operator=(const PipeReader&) = delete;
    PipeReader(PipeReader&& other) noexcept : handle_(other.handle_) { other.handle_ = closed; }
    // Closes this end first.
    PipeReader& operator=(PipeReader&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != closed; }

    // Reads at most into.count() bytes into into[0, n), blocking until at
    // least one byte arrives or the stream ends. Nothing past the reported
    // count is written. Requirement: into is not empty. Failures: disposed
    // with HostCode::closed_handle on a closed end; io_failure with the
    // host's code. A failure leaves `into` unchanged.
    [[nodiscard]] OperationResult<PipeRead> read(MutableView<Byte> into) noexcept;

    // Closes the handle; does nothing when closed already.
    void close() noexcept;

    // The handle, for a wait that watches it (wake.hpp). -1 when closed.
    [[nodiscard]] i64 native_handle() const noexcept { return handle_; }

    // Adopts a host handle the caller owns and gives up (the end of a pipe
    // made elsewhere). Requirement: handle is not -1.
    [[nodiscard]] static PipeReader adopt(i64 handle) noexcept;

private:
    static constexpr i64 closed = -1;

    explicit PipeReader(const i64 handle) noexcept : handle_(handle) {}

    i64 handle_{closed};
};

class PipeWriter final {
public:
    PipeWriter() noexcept = default;
    ~PipeWriter();
    PipeWriter(const PipeWriter&) = delete;
    PipeWriter& operator=(const PipeWriter&) = delete;
    PipeWriter(PipeWriter&& other) noexcept : handle_(other.handle_) { other.handle_ = closed; }
    // Closes this end first.
    PipeWriter& operator=(PipeWriter&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != closed; }

    // Writes every byte of `bytes`, blocking while the pipe is full.
    // Failures: disposed with HostCode::closed_handle on a closed end;
    // io_failure with HostCode::stream_closed when the reader has closed;
    // io_failure with the host's code otherwise. After a failure an unknown
    // prefix of `bytes` may have been written: the stream is then broken,
    // and its user closes it.
    [[nodiscard]] OperationStatus write_all(View<Byte> bytes) noexcept;

    // Closes the handle, so the reader sees the end of the stream once it
    // has read what was written; does nothing when closed already.
    void close() noexcept;

    [[nodiscard]] i64 native_handle() const noexcept { return handle_; }

    // Adopts a host handle the caller owns and gives up. Requirement:
    // handle is not -1.
    [[nodiscard]] static PipeWriter adopt(i64 handle) noexcept;

private:
    static constexpr i64 closed = -1;

    explicit PipeWriter(const i64 handle) noexcept : handle_(handle) {}

    i64 handle_{closed};
};

// The two ends of one new pipe: bytes written to `writer` are read from
// `reader`, in order.
struct PipePair final {
    PipeReader reader{};
    PipeWriter writer{};
};

// A new anonymous pipe. Failures: unsupported where
// Capability::anonymous_pipes is false; limit_exceeded with the host's code
// when the process or system is out of descriptors or handles;
// foreign_failure with the host's code otherwise.
[[nodiscard]] OperationResult<PipePair> create_pipe() noexcept;

// The process's standard streams, claimed for a protocol (MCP's stdio
// transport; study section 4.2: "standard output belongs to MCP").
struct ProtocolStreams final {
    // What the client writes: a duplicate of standard input.
    PipeReader input{};
    // What the client reads: a duplicate of standard output as it was
    // before the claim.
    PipeWriter output{};
};

// Claims standard input and output for a protocol, once per process:
// - `input` is a close-on-exec duplicate of standard input; standard input
//   itself is left as it was, and nothing else should read it;
// - `output` is a close-on-exec duplicate of standard output; standard
//   output is then pointed at standard error (POSIX: dup2(2, 1), after the
//   C library's stdout buffer is flushed; Windows: SetStdHandle and the C
//   runtime's _dup2(2, 1)), so a program's printf and STXlib's
//   write_console(output) land in the error stream and cannot break the
//   protocol.
// Failures: unsupported where Capability::anonymous_pipes is false;
// limit_exceeded with HostCode::busy once the streams have been claimed in
// this process (by any module: the flag is in stxlib_state), and the
// process's streams are not touched again; io_failure with
// HostCode::stream_closed when standard input or output is absent;
// limit_exceeded or foreign_failure with the host's code. A failure after
// the duplicates were made closes them and leaves standard output as it
// was; the claim then counts as not made.
[[nodiscard]] OperationResult<ProtocolStreams> claim_protocol_streams() noexcept;

} // namespace stxlib
