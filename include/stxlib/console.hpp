#pragma once

// The console (Capability::console): the process's standard output,
// standard error and standard input, as UTF-8 text. Ported from
// stx_auxlib's include/stx_auxlib/console.hpp at b6e330a.
//
// Settled platform differences:
// - Text is UTF-8 everywhere. On a Windows console it is written and read
//   as UTF-16 through the console API, so it displays correctly whatever
//   the console's code page; redirected to a file or pipe it is UTF-8 bytes,
//   as on POSIX hosts.
// - Writes are unbuffered and whole: each call writes all its text before
//   returning, and concurrent calls never interleave within one call, in
//   any module (the lock is in stxlib_state). The C library's stdout or
//   stderr buffer is flushed first, so output keeps its order with code
//   that still uses printf during migration.
// - read_console_line strips "\n" and "\r\n" alike.
//
// Not settled here: on POSIX hosts, writing to a pipe whose reader has
// closed raises SIGPIPE, which ends the program unless it ignores that
// signal (ignore_broken_pipes, events.hpp); with the signal ignored, the
// write fails with io_failure, HostCode::stream_closed.
//
// A TextView is UTF-8 by construction, so writes cannot be handed invalid
// text; stx_auxlib's invalid_text refusal on output has nothing left to do.

#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

enum class ConsoleStream : u8 {
    output = 0,
    error = 1,
};

// Failures: io_failure (HostCode::stream_closed when the stream is closed
// or absent; otherwise with the host's code).
[[nodiscard]] Status write_console(ConsoleStream stream, TextView text) noexcept;
// `text` followed by "\n", under the same lock, so no other call's text
// comes between them.
[[nodiscard]] Status write_console_line(ConsoleStream stream, TextView text) noexcept;
// True when the stream is an interactive terminal rather than a file or pipe.
[[nodiscard]] bool console_is_terminal(ConsoleStream stream) noexcept;

// One line read from standard input, or the end of input.
class ConsoleLine final {
public:
    [[nodiscard]] static ConsoleLine line(Utf8String&& text) noexcept;
    [[nodiscard]] static ConsoleLine end_of_input() noexcept;

    ConsoleLine(ConsoleLine&&) noexcept = default;
    ConsoleLine& operator=(ConsoleLine&&) noexcept = default;
    ConsoleLine(const ConsoleLine&) = delete;
    ConsoleLine& operator=(const ConsoleLine&) = delete;
    ~ConsoleLine() = default;

    // True when no more input will arrive; text() is then empty.
    [[nodiscard]] bool ended() const noexcept { return kind_ == Kind::end_of_input; }
    // The line without its ending. Borrowed from this object.
    [[nodiscard]] TextView text() const& noexcept { return text_.view(); }
    TextView text() const&& = delete;

private:
    // Which state this is (audit A, X3); text_ is empty unless it is a line.
    // Only the two factories write either field.
    enum class Kind : u8 {
        line = 0,
        end_of_input = 1,
    };

    ConsoleLine() noexcept = default;

    Utf8String text_{};
    Kind kind_{Kind::line};
};

// The next line of standard input, without its line ending. A last line
// without an ending is returned as a line; the call after it returns the
// end of input. Failures: limit_exceeded with HostCode::too_long for a line
// of more than `limit` bytes (the line is consumed through its ending, so
// the next call starts at the following line); invalid_argument with
// HostCode::invalid_text for a line that is not UTF-8 (consumed);
// out_of_memory; io_failure.
[[nodiscard]] Result<ConsoleLine> read_console_line(usize limit) noexcept;

} // namespace stxlib
