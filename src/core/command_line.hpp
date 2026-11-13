#pragma once

// Windows command lines: splitting one into arguments by the rules the
// C runtime uses for main's argv, and quoting arguments so that the split
// returns them (process.hpp, process_arguments on Windows). Pure: no host
// call, no allocation; every write is inside the caller's views. In proof
// scope (tests/platform_rest_exhaustive.cpp; proofs/esbmc/platform_rest.cpp).
//
// The rules (Microsoft Learn, "Parsing C++ command-line arguments", the
// Microsoft-specific section, as published; unconfirmed against the UCRT
// source at a pin, and checked against the C runtime's own argv on the
// Windows CI host by tests/process_tests.cpp):
//   1. The first argument, the program name, runs from the line's first
//      character to the first space or tab outside double quotes. Double
//      quotes toggle quoting and are dropped; backslashes are literal.
//   2. Later arguments are separated by runs of spaces and tabs.
//   3. A double quote toggles quoting and is dropped, except as below.
//   4. 2n backslashes then a double quote: n backslashes, and the quote
//      toggles quoting. 2n + 1 backslashes then a double quote: n
//      backslashes and a literal double quote.
//   5. Backslashes not followed by a double quote are literal.
//   6. Inside quotes, two double quotes give one literal double quote, and
//      quoting continues (the documentation's example: a"b"" c d gives
//      ab" c d).
//   7. A line that ends inside quotes ends its last argument there.
// Only ASCII bytes are special, so the split works on WTF-8 as on UTF-8:
// every byte of a multi-byte sequence is 0x80 or above.

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::command_line {

struct SplitCounts final {
    usize argument_count{0};
    // Bytes the arguments need, each followed by one NUL.
    usize text_bytes{0};
};

namespace detail {

// Writes into the caller's views while they have room; counts always.
class SplitWriter final {
public:
    SplitWriter(const MutableView<char> text, const MutableView<usize> starts) noexcept
        : text_(text), starts_(starts) {}

    void begin_argument() noexcept {
        if (counts_.argument_count < starts_.count()) starts_[counts_.argument_count] = counts_.text_bytes;
        counts_.argument_count = counts_.argument_count + 1;
    }
    void put(const char byte) noexcept {
        if (counts_.text_bytes < text_.count()) text_[counts_.text_bytes] = byte;
        counts_.text_bytes = counts_.text_bytes + 1;
    }
    void put_backslashes(const usize count) noexcept {
        for (usize index = 0; index < count; ++index) put('\\');
    }
    void end_argument() noexcept { put('\0'); }

    [[nodiscard]] SplitCounts counts() const noexcept { return counts_; }

private:
    MutableView<char> text_;
    MutableView<usize> starts_;
    SplitCounts counts_{};
};

[[nodiscard]] constexpr bool is_separator(const char byte) noexcept {
    const bool result = byte == ' ' || byte == '\t';
    return result;
}

} // namespace detail

// Splits `line` (rules above). Writes each argument's bytes and a NUL into
// `text` and each argument's first offset in `text` into `starts`, as far as
// they have room, and returns what the whole split needs: the output is
// complete exactly when text.count() >= text_bytes and starts.count() >=
// argument_count. A first call with empty views measures. The needs are
// bounded: text_bytes <= line.count() + argument_count, and argument_count
// <= line.count() / 2 + 1 (each argument after the first takes a separator
// and a byte, or the two quotes of an empty one). An empty line has no
// arguments. `line` and the outputs must not overlap.
[[nodiscard]] inline SplitCounts split_windows_command_line(const View<char> line, const MutableView<char> text,
                                                           const MutableView<usize> starts) noexcept {
    detail::SplitWriter writer(text, starts);
    const usize count = line.count();
    if (count == 0) return writer.counts();
    const char* const bytes = line.data();
    usize index = 0;

    // Rule 1: the program name.
    writer.begin_argument();
    bool quoted = false;
    while (index < count) {
        const char byte = bytes[index];
        if (byte == '"') {
            quoted = !quoted;
            index = index + 1;
            continue;
        }
        if (!quoted && detail::is_separator(byte)) break;
        writer.put(byte);
        index = index + 1;
    }
    writer.end_argument();

    // Rules 2 to 7: the arguments.
    for (;;) {
        while (index < count && detail::is_separator(bytes[index])) index = index + 1;
        if (index == count) break;
        writer.begin_argument();
        quoted = false;
        while (index < count) {
            const char byte = bytes[index];
            if (!quoted && detail::is_separator(byte)) break;
            if (byte == '\\') {
                usize run_end = index;
                while (run_end < count && bytes[run_end] == '\\') run_end = run_end + 1;
                const usize run = run_end - index;
                if (run_end < count && bytes[run_end] == '"') {
                    writer.put_backslashes(run / 2);
                    if (run % 2 == 1) {
                        writer.put('"');
                        index = run_end + 1;
                    } else {
                        // The quote is read by the next pass as rule 3 or 6.
                        index = run_end;
                    }
                } else {
                    writer.put_backslashes(run);
                    index = run_end;
                }
                continue;
            }
            if (byte == '"') {
                if (quoted && index + 1 < count && bytes[index + 1] == '"') {
                    writer.put('"');
                    index = index + 2;
                } else {
                    quoted = !quoted;
                    index = index + 1;
                }
                continue;
            }
            writer.put(byte);
            index = index + 1;
        }
        writer.end_argument();
    }
    return writer.counts();
}

// Whether `argument` must be quoted to survive the split as one argument.
[[nodiscard]] inline bool needs_quotes(const View<char> argument) noexcept {
    if (argument.count() == 0) return true;
    for (usize index = 0; index < argument.count(); ++index) {
        const char byte = argument.data()[index];
        if (detail::is_separator(byte) || byte == '"') return true;
    }
    return false;
}

// Writes `argument` quoted for a position after the program name, so that
// split_windows_command_line returns it unchanged, into `output` as far as
// it has room; returns the bytes the whole quoting needs (at most
// 2 * argument.count() + 2). An argument without spaces, tabs or quotes is
// written as it is.
[[nodiscard]] inline usize quote_windows_argument(const View<char> argument, const MutableView<char> output) noexcept {
    usize written = 0;
    const usize room = output.count();
    const usize count = argument.count();
    const char* const bytes = argument.data();
    if (!needs_quotes(argument)) {
        for (usize index = 0; index < count; ++index) {
            if (written < room) output[written] = bytes[index];
            written = written + 1;
        }
        return written;
    }
    if (written < room) output[written] = '"';
    written = written + 1;
    usize index = 0;
    while (index < count) {
        usize run_end = index;
        while (run_end < count && bytes[run_end] == '\\') run_end = run_end + 1;
        const usize run = run_end - index;
        // Backslashes before the closing quote or a literal quote double;
        // others stay literal (rules 4 and 5).
        usize emitted = run;
        if (run_end == count) emitted = 2 * run;
        if (run_end < count && bytes[run_end] == '"') emitted = 2 * run + 1;
        for (usize copy = 0; copy < emitted; ++copy) {
            if (written < room) output[written] = '\\';
            written = written + 1;
        }
        if (run_end == count) break;
        if (written < room) output[written] = bytes[run_end];
        written = written + 1;
        index = run_end + 1;
    }
    if (written < room) output[written] = '"';
    written = written + 1;
    return written;
}

} // namespace stxlib::command_line
