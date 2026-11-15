#pragma once

// The primitives each family's file backend supplies
// (posix/file_posix.cpp, windows/file_windows.cpp); exactly one is linked.
// core/files.cpp builds the public operations (include/stxlib/files.hpp) on
// them. Ported from stx_auxlib's src/hosted/file_backend.hpp at b6e330a,
// with the operations stx_auxlib took from std::filesystem added.
//
// Names (D13 a: host names are carried losslessly). Every `name` is a
// host name with a terminating NUL, '/' separating:
// - POSIX hosts (byte_names): the host's bytes, Path::posix_name of a Path
//   (never a drive path: core refuses those first). Names a backend returns
//   are the host's bytes; core makes Paths of them with from_posix_name.
// - Windows: a Path's WTF-8 text, which the backend converts to UTF-16
//   (core/wtf8.hpp, lossless) with '\' separators and the "\\?\" prefix for
//   drive paths. Names it returns are WTF-8 made from the host's units, '/'
//   separating; core makes Paths of them with parse_wtf8.
//
// A handle is a POSIX descriptor or a Windows HANDLE held in an i64; -1
// means none on both. Every primitive retries interrupted calls.

#include "stxlib/detail/early_exit.hpp"
#include "core/growable.hpp"
#include "stxlib/error.hpp"
#include "stxlib/files.hpp"
#include "stxlib/types.hpp"

namespace stxlib::file_backend {

inline constexpr i64 no_handle = -1;

// Whether this family has drive roots ("C:/"), and whether its names are
// bytes (POSIX) rather than WTF-8 made from 16-bit units (Windows).
#if defined(_WIN32)
inline constexpr bool drive_roots = true;
inline constexpr bool byte_names = false;
#else
inline constexpr bool drive_roots = false;
inline constexpr bool byte_names = true;
#endif

// Whether the family's file system keeps only names that are UTF-8, so
// that core refuses any other with invalid_name before the host sees it.
// Emscripten's JavaScript file system (MEMFS and IDBFS) holds names as
// JavaScript strings, made from the bytes by UTF8ToString
// (src/lib/libstrings.js at 6.0.13): a TextDecoder above 16 bytes, which
// replaces what is not UTF-8, and below that a loop that takes a byte such
// as 0xFF as the lead of a four-byte sequence, swallowing the bytes after
// it. Either way a name that is not UTF-8 is created under some other
// name (the first web run listed neither "h\xff.t" nor "s\xc3(" after
// making both), and two such names can reach one file. WasmFS keeps the
// bytes (the same test passed there).
#if defined(__EMSCRIPTEN__) && !defined(STXLIB_EMSCRIPTEN_WASMFS)
inline constexpr bool utf8_names_only = true;
#else
inline constexpr bool utf8_names_only = false;
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
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome open(const char* name, FileMode mode, i64& handle) noexcept;
// Reads at most `count` bytes; zero transferred is the end.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome read(i64 handle, Byte* bytes, usize count, usize& transferred) noexcept;
// Writes a prefix of bytes[0, count), at least one byte unless it fails.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome write(i64 handle, const Byte* bytes, usize count, usize& transferred) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome size(i64 handle, u64& bytes) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome position(i64 handle, u64& offset) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome seek(i64 handle, u64 offset) noexcept;
// Flushes the file's content as `level` says (files.hpp, Durability); none
// does nothing.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome sync(i64 handle, Durability level) noexcept;
// Releases the handle even when it reports a failure.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome close(i64 handle) noexcept;

// Positioned I/O (M2 C2): at most `count` bytes at `offset` (at most
// 2^63 - 1, which core checks), leaving the handle's position as it was.
[[nodiscard]] Outcome read_at(i64 handle, u64 offset, Byte* bytes, usize count, usize& transferred) noexcept;
// Writes a prefix of bytes[0, count) at `offset`, at least one byte unless it
// fails. Never called for an append handle (core refuses it).
[[nodiscard]] Outcome write_at(i64 handle, u64 offset, const Byte* bytes, usize count, usize& transferred) noexcept;
// The open file's kind, size and last-write time.
[[nodiscard]] Outcome information(i64 handle, FileInformation& information) noexcept;
// Sets the last-write time, leaving the access time.
[[nodiscard]] Outcome set_modified(i64 handle, WallTime modified) noexcept;
// Sets the file's length (at most 2^63 - 1, which core checks).
[[nodiscard]] Outcome set_size(i64 handle, u64 bytes) noexcept;

[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome inspect(const char* name, LinkMode links, FileInformation& information) noexcept;
// Renames `from` over `to`; for Durability::durable, also makes the rename
// itself durable (POSIX: flushes the directory holding `to`; Windows:
// MOVEFILE_WRITE_THROUGH).
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome replace(const char* from, const char* to, Durability level) noexcept;
// already_exists when anything has the name.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome create_directory(const char* name) noexcept;
// A file, a symbolic link (not what it names) or an empty directory.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome remove(const char* name) noexcept;
// Removes the tree at `name` as files.hpp's remove_tree states (relative
// descent on POSIX hosts, held directories on Windows), descending at most
// `depth_limit` levels below it and removing at most `entries_limit` entries
// from any one directory (too_large past either), adding each entry it
// removes to `removed` (also on failure). Only `name` is looked up by path.
// not_found, with nothing removed, when nothing has that name.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome remove_tree(const char* name, usize depth_limit,
                                                                                    usize entries_limit, u64& removed) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome rename(const char* from, const char* to) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome copy_file(const char* from, const char* to, CopyMode mode) noexcept;

// One entry list returned: names[offset, offset + count), its kind without
// following links.
struct RawEntry final {
    usize offset{0};
    usize count{0};
    FileKind kind{FileKind::other};
};

// Appends the directory's entries (not "." and "..") to `names` (each name
// followed by a NUL, in the family's name form above) and `entries`;
// too_large once more than `limit` are seen.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL Outcome list(const char* name, usize limit, Growable<char>& names, Growable<RawEntry>& entries) noexcept;

// Each replaces `text` with a host name in the family's form (not
// NUL-terminated).
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome temporary_directory(Growable<char>& text) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome working_directory(Growable<char>& text) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome resolve(const char* name, Growable<char>& text) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome read_link(const char* name, Growable<char>& text) noexcept;

// `to_directory`: the target is a directory (Windows distinguishes links to
// directories; POSIX hosts ignore it).
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT Outcome create_link(const char* target, const char* link, bool to_directory) noexcept;

} // namespace stxlib::file_backend
