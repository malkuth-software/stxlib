#pragma once

// The primitives each family's console backend supplies
// (posix/console_posix.cpp, windows/console_windows.cpp); exactly one is
// linked. core/console.cpp serializes calls and assembles lines on them.
// Ported from stx_auxlib's src/hosted/console_backend.hpp at b6e330a.

#include "stxlib/console.hpp"
#include "stxlib/error.hpp"
#include "stxlib/types.hpp"

namespace stxlib::console_backend {

enum class Outcome : u8 {
    ok = 0,
    closed = 1,       // the stream is closed or absent
    invalid_text = 2, // the host delivered text that is not UTF-8 or UTF-16
    out_of_memory = 3,
    io_error = 4,
};

// An outcome and, for closed and io_error, the host's code.
struct Report final {
    Outcome outcome{Outcome::ok};
    ForeignCode foreign{};
};

// Writes all `count` bytes of valid UTF-8 to the stream.
[[nodiscard]] Report write(ConsoleStream stream, const char* bytes, usize count) noexcept;
// Writes the bytes and then "\n", in one host call where the host can.
[[nodiscard]] Report write_line(ConsoleStream stream, const char* bytes, usize count) noexcept;
[[nodiscard]] bool is_terminal(ConsoleStream stream) noexcept;
// Reads UTF-8 bytes from standard input into `buffer` (at least 16 bytes);
// zero transferred with ok is the end of input.
[[nodiscard]] Report read(char* buffer, usize capacity, usize& transferred) noexcept;

} // namespace stxlib::console_backend
