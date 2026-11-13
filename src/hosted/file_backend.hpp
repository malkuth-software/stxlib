#pragma once

// The primitives each family's file backend supplies
// (posix/file_posix.cpp, windows/file_windows.cpp); exactly one is linked.
// core/files.cpp builds the public operations (include/stxlib/files.hpp) on
// them. Ported from stx_auxlib's src/hosted/file_backend.hpp at b6e330a,
// with the operations stx_auxlib took from std::filesystem added.
//
// Names: every `name` is a Path's text with a terminating NUL ('/'
// separators, normalized). On POSIX hosts it is never a drive path (core
// refuses those first); the Windows backend converts it to UTF-16 with
// '\' separators and the "\\?\" prefix for drive paths. Text a backend
// returns is the host's name in UTF-8 with '/' separators (raw bytes on
// POSIX hosts), not yet checked as a Path: core parses it.
//
// A handle is a POSIX descriptor or a Windows HANDLE held in an i64; -1
// means none on both. Every primitive retries interrupted calls.

#include "core/growable.hpp"
#include "stxlib/error.hpp"
#include "stxlib/files.hpp"
#include "stxlib/types.hpp"

namespace stxlib::file_backend {

inline constexpr i64 no_handle = -1;

// Whether this family has drive roots ("C:/").
#if defined(_WIN32)
inline constexpr bool drive_roots = true;
#else
inline constexpr bool drive_roots = false;
#endif

// stx_auxlib's FileStatus, plus out_of_memory for the backend's buffers.
enum class FileStatus : u8 {
    ok = 0,
    not_found = 1,
    already_exists = 2,
    permission_denied = 3,
    not_a_directory = 4,
    is_a_directory = 5,
    directory_not_empty = 6,
    no_space = 7,
    too_large = 8,
    name_too_long = 9,
    invalid_name = 10,
    unsupported = 11,
    cross_device = 12,
    busy = 13,
    bad_handle = 14,
    io_error = 15,
    out_of_memory = 16,
};

struct Outcome final {
    FileStatus status{FileStatus::ok};
    ForeignCode foreign{};

    [[nodiscard]] bool ok() const noexcept { return status == FileStatus::ok; }
};

[[nodiscard]] inline Outcome success() noexcept {
    const Outcome result{};
    return result;
}

[[nodiscard]] inline Outcome failure(const FileStatus status) noexcept {
    const Outcome result{status, ForeignCode::none()};
    return result;
}

// Opens `name` for `mode`. Opening a directory for reading fails with
// is_a_directory.
[[nodiscard]] Outcome open(const char* name, FileMode mode, i64& handle) noexcept;
// Reads at most `count` bytes; zero transferred is the end.
[[nodiscard]] Outcome read(i64 handle, Byte* bytes, usize count, usize& transferred) noexcept;
// Writes a prefix of bytes[0, count), at least one byte unless it fails.
[[nodiscard]] Outcome write(i64 handle, const Byte* bytes, usize count, usize& transferred) noexcept;
[[nodiscard]] Outcome size(i64 handle, u64& bytes) noexcept;
[[nodiscard]] Outcome position(i64 handle, u64& offset) noexcept;
[[nodiscard]] Outcome seek(i64 handle, u64 offset) noexcept;
// Waits until the file's content is on stable storage.
[[nodiscard]] Outcome sync(i64 handle) noexcept;
// Releases the handle even when it reports a failure.
[[nodiscard]] Outcome close(i64 handle) noexcept;

[[nodiscard]] Outcome inspect(const char* name, LinkMode links, FileInformation& information) noexcept;
// Renames `from` over `to` and makes the rename durable where the host can.
[[nodiscard]] Outcome replace_durably(const char* from, const char* to) noexcept;
// already_exists when anything has the name.
[[nodiscard]] Outcome create_directory(const char* name) noexcept;
// A file, a symbolic link (not what it names) or an empty directory.
[[nodiscard]] Outcome remove(const char* name) noexcept;
[[nodiscard]] Outcome rename(const char* from, const char* to) noexcept;
[[nodiscard]] Outcome copy_file(const char* from, const char* to, CopyMode mode) noexcept;

// One entry list returned: names[offset, offset + count), its kind without
// following links.
struct RawEntry final {
    usize offset{0};
    usize count{0};
    FileKind kind{FileKind::other};
};

// Appends the directory's entries (not "." and "..") to `names` (each name
// followed by a NUL) and `entries`; too_large once more than `limit` are
// seen. Names the host cannot express in UTF-8 (Windows' unpaired
// surrogates) are counted in `unconvertible` and not appended; they count
// toward the limit.
[[nodiscard]] Outcome list(const char* name, usize limit, Growable<char>& names, Growable<RawEntry>& entries,
                           usize& unconvertible) noexcept;

// Each replaces `text` with a host name (not NUL-terminated).
[[nodiscard]] Outcome temporary_directory(Growable<char>& text) noexcept;
[[nodiscard]] Outcome working_directory(Growable<char>& text) noexcept;
[[nodiscard]] Outcome resolve(const char* name, Growable<char>& text) noexcept;
[[nodiscard]] Outcome read_link(const char* name, Growable<char>& text) noexcept;

// `to_directory`: the target is a directory (Windows distinguishes links to
// directories; POSIX hosts ignore it).
[[nodiscard]] Outcome create_link(const char* target, const char* link, bool to_directory) noexcept;

} // namespace stxlib::file_backend
