// turn_file.h: small files kept on the user partition by turns (C2; design
// dev/notes/planning/stages/C2-web.md, section 6; sb2's C-minimal-host.md,
// K18).
//
// The user partition is FAT32 (msdosfs). A rename there does not survive a
// crash atomically, so a file that must never be lost is kept as two
// files, written by turns: each write goes to the file that does not hold
// the newest valid turn, with a sequence number one above it, and is
// synced before anything relies on it. A crash while writing damages only
// the file being written; the other still holds the previous turn.
//
// One turn file:
//
//   STXTURN 1 <sequence> <length> <crc>\n<body>
//
// sequence: decimal, 1..4294967295, no leading zero; length: decimal, the
// body's byte count, at most the caller's maximum, no leading zero (0 is
// "0"); crc: the body's CRC-32 (ISO-HDLC: reflected polynomial 0xedb88320,
// initial and final value 0xffffffff), 8 lower-case hex digits. The file is
// exactly the header and length bytes: anything shorter, longer or
// different is not a valid turn. The CRC finds torn and partial writes; it
// is not a defence against someone who edits the card on purpose (the
// contents are checked by their own readers: the configuration parser, the
// password record's format).
//
// Users: the supervisor's user configuration (config/user_config.h) and the
// web service's password record and TLS identity (runtime/web/).
//
// Pure code: no system call, no allocation. turn_file_system.h reads and
// writes the files.

#ifndef STXLIB_RUNTIME_STORE_TURN_FILE_H
#define STXLIB_RUNTIME_STORE_TURN_FILE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace stxstore {

// "STXTURN 1 " + 10 digits + " " + 10 digits + " " + 8 hex + "\n".
constexpr std::size_t turn_header_bytes_max = 41;
constexpr std::uint32_t turn_sequence_max = 0xffffffffu;
// The largest body any user keeps (the user configuration's 16384 bytes).
constexpr std::size_t turn_body_bytes_max = 16384;
constexpr std::size_t turn_file_bytes_max = turn_header_bytes_max + turn_body_bytes_max;

[[nodiscard]] std::uint32_t crc32_of(std::span<const char> bytes) noexcept;

// Writes the header for a body into out; returns its length.
[[nodiscard]] std::size_t format_turn_header(std::uint32_t sequence, std::span<const char> body,
                                             std::span<char, turn_header_bytes_max> out) noexcept;

struct TurnView {
    std::uint32_t sequence;
    // Borrowed from the file's bytes.
    std::span<const char> body;
};

// The turn a file holds, or nothing when it is not a valid turn (or its
// body is longer than body_max).
[[nodiscard]] std::optional<TurnView> parse_turn_file(std::span<const char> file, std::size_t body_max) noexcept;

// Which file to trust, and which to write next.
struct TurnChoice {
    // The file holding the newest valid turn, then the other valid one;
    // nothing where there is none. Readers try newest, then older.
    std::optional<std::size_t> newest;
    std::optional<std::size_t> older;
    // The file the next write goes to: the one not holding the newest
    // valid turn (file 0 when neither is valid).
    std::size_t write_index;
    // The next write's sequence: one above the newest, 1 when there is
    // none; nothing when the newest is at turn_sequence_max (the store is
    // full and refuses writes).
    std::optional<std::uint32_t> next_sequence;
};

// From the two files' turns (nothing for a file that is absent or
// invalid). Equal sequences in both (no write of this module makes them)
// count file 0 as the newer.
[[nodiscard]] TurnChoice choose_turns(const std::optional<TurnView>& file_0,
                                      const std::optional<TurnView>& file_1) noexcept;

}  // namespace stxstore

#endif  // STXLIB_RUNTIME_STORE_TURN_FILE_H
