// turn_file_system.h: reading and writing turn files on FreeBSD (C2; the
// format and the rule are turn_file.h).
//
// System calls: open, fstat, read, write, fsync, close, mkdir. Hosted on
// FreeBSD's libc; the supervisor and the web service both link it.
//
// Durability, as far as this code can make it: the file being written is
// opened with O_TRUNC, written whole, fsynced and closed before the write
// is reported done; then the directory is fsynced too (msdosfs updates the
// directory entry's size when the file is synced, msdosfs_fsync and
// deupdat, read in the pin's sys/fs/msdosfs; whether a directory fsync
// adds anything there is not confirmed). The rule does not depend on the
// last step: a write that a crash loses leaves the other file's turn.

#ifndef STXLIB_RUNTIME_STORE_TURN_FILE_SYSTEM_H
#define STXLIB_RUNTIME_STORE_TURN_FILE_SYSTEM_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "turn_file.h"

namespace stxstore {

struct TurnPaths {
    // The directory holding both files (created by write_turn when
    // absent, one level only), and the two files' paths.
    const char* directory;
    const char* files[2];
};

// Both files as read: each one's turn when it is valid. The views borrow
// the two buffers given to read_turns.
struct TurnState {
    std::optional<TurnView> turns[2];
    // errno of a failure other than absence (ENOENT), per file; 0 if none.
    int errors[2];
    TurnChoice choice;
};

// Reads both files into buffer_0 and buffer_1 (each at most
// turn_header_bytes_max + body_max bytes is read; a longer file is
// invalid) and chooses.
[[nodiscard]] TurnState read_turns(const TurnPaths& paths, std::size_t body_max, std::span<char> buffer_0,
                                   std::span<char> buffer_1) noexcept;

struct TurnWritten {
    std::size_t file_index;
    std::uint32_t sequence;
};
struct TurnStoreFull {
};
struct TurnWriteFailed {
    int error_number;
};

class TurnWriteOutcome {
public:
    [[nodiscard]] static TurnWriteOutcome written(std::size_t file_index, std::uint32_t sequence) noexcept;
    [[nodiscard]] static TurnWriteOutcome full() noexcept;
    [[nodiscard]] static TurnWriteOutcome failed(int error_number) noexcept;
    [[nodiscard]] const TurnWritten* as_written() const noexcept;
    [[nodiscard]] bool is_full() const noexcept;
    [[nodiscard]] const TurnWriteFailed* as_failed() const noexcept;

private:
    using Alternatives = std::variant<TurnWritten, TurnStoreFull, TurnWriteFailed>;
    explicit TurnWriteOutcome(const Alternatives& alternatives) noexcept;
    Alternatives alternatives_;
};

// Writes body as the next turn, as choice (from read_turns, just before)
// says: into choice.write_index with choice.next_sequence. mode is the new
// file's permission bits (msdosfs ignores them; its mount options decide).
[[nodiscard]] TurnWriteOutcome write_turn(const TurnPaths& paths, const TurnChoice& choice,
                                          std::span<const char> body, unsigned int mode) noexcept;

}  // namespace stxstore

#endif  // STXLIB_RUNTIME_STORE_TURN_FILE_SYSTEM_H
