#pragma once

// The program's one path type: UTF-8 text with '/' separators, validated
// and normalized when it is made, so that a Path means the same on every
// host. Conversion to a host's native form happens only inside the
// interface modules (src/hosted/). Path is pure computation and touches no
// filesystem. Ported from stx_auxlib's include/stx_auxlib/paths.hpp and
// src/paths.cpp at b6e330a.
//
// Syntax. A path is a root followed by segments separated by single '/'.
// The root is "/" (absolute), "X:/" for a drive letter X (absolute; always
// upper case), or none (relative). parse normalizes: repeated '/' collapse,
// "." segments vanish, a ".." removes the segment before it, and a trailing
// '/' is dropped. A ".." at the start of a relative path stays; a ".." above
// an absolute root is refused (escapes_root), never silently dropped. The
// empty relative path is ".".
//
// Refused text: empty text, more than maximum_path_bytes bytes, a NUL,
// invalid UTF-8 (parse_bytes only: a TextView is UTF-8 by construction),
// '\' and ':' outside a drive root. '\' and ':' are refused because hosts
// disagree on them ('\' separates on Windows, ':' names streams on NTFS and
// drives elsewhere); refusing them keeps a path's meaning identical
// everywhere.
//
// Comparison is by bytes. Hosts whose filesystems ignore case or normalize
// Unicode (APFS, NTFS) may still treat two unequal Paths as one file.
//
// Hosts: POSIX hosts support "/" roots and report drive roots unsupported
// (files.hpp); Windows supports both ("/" is the current drive's root). UNC
// names ("//server/share") are not supported: their leading "//" collapses.
// Survey D13 (files and paths) is open; this is stx_auxlib's current
// behaviour (host names that are not valid Unicode are refused, D13's
// option b), kept until the owner answers.
//
// Changes from stx_auxlib: a Path owns its text on STXlib's allocator and is
// move-only (copy() makes a second one); every operation that allocates
// returns a Result, so exhaustion is out_of_memory, not an abort; join and
// parent bound only their result (stx_auxlib refused a join whose operands
// together exceeded the bound, and parent returned the path unchanged when
// "<path>/.." did).
//
// Failures: invalid_argument with the PathFailure as the record's code, or
// limit_exceeded with PathFailure::too_long; out_of_memory.

#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// The record's code when a path is refused.
enum class PathFailure : u32 {
    empty = 1,
    too_long = 2, // kind limit_exceeded
    embedded_nul = 3,
    invalid_utf8 = 4,
    reserved_character = 5,
    escapes_root = 6,
    absolute_join = 7,
};

// Bounds every Path, before and after normalization, in bytes.
inline constexpr usize maximum_path_bytes = 4096;

class Path final {
public:
    // The relative path ".". Allocates nothing.
    Path() noexcept = default;

    // Validates and normalizes `text`.
    [[nodiscard]] static Result<Path> parse(TextView text) noexcept;
    // As parse, for foreign bytes not yet known to be UTF-8 (a host's name).
    // Requirement: bytes is non-null unless count is zero.
    [[nodiscard]] static Result<Path> parse_bytes(const char* bytes, usize count) noexcept;

    Path(Path&&) noexcept = default;
    Path& operator=(Path&&) noexcept = default;
    Path(const Path&) = delete;
    Path& operator=(const Path&) = delete;
    ~Path() = default;

    // A second Path with the same text.
    [[nodiscard]] Result<Path> copy() const noexcept;

    // Borrowed from this path: valid until it is moved from or destroyed. A
    // moved-from Path reads ".".
    [[nodiscard]] TextView text() const& noexcept;
    TextView text() const&& = delete;

    [[nodiscard]] bool is_absolute() const noexcept;
    // True for "/" and "X:/" alone.
    [[nodiscard]] bool is_root() const noexcept;
    // The drive letter of a "X:/" root, or 0.
    [[nodiscard]] char drive() const noexcept;

    // This path followed by `relative`, normalized. Fails with absolute_join
    // when `relative` is absolute, and with escapes_root or too_long as parse
    // does (for the result).
    [[nodiscard]] Result<Path> join(const Path& relative) const noexcept;
    // The path without its last segment: "a/b" -> "a", "a" -> ".",
    // "." -> "..", ".." -> "../..", and a root is its own parent. Fails with
    // too_long ("../.." past the bound) or out_of_memory.
    [[nodiscard]] Result<Path> parent() const noexcept;

    // The last segment, or empty for a root, "." and a path ending in "..".
    // Borrowed from this path.
    [[nodiscard]] TextView file_name() const& noexcept;
    TextView file_name() const&& = delete;
    // The last segment's text from its last '.', including the dot
    // ("a.tar.gz" -> ".gz"); empty when there is none or the name starts with
    // its only dot (".profile"). Borrowed from this path.
    [[nodiscard]] TextView extension() const& noexcept;
    TextView extension() const&& = delete;

    [[nodiscard]] bool operator==(const Path& other) const noexcept;
    [[nodiscard]] bool operator!=(const Path& other) const noexcept;

    // For src/core/path.cpp only: adopts normalized text.
    struct Access;

private:
    friend struct Access;

    explicit Path(Utf8String normalized) noexcept : text_(static_cast<Utf8String&&>(normalized)) {}

    // The length of the root prefix: 0, 1 ("/") or 3 ("X:/").
    [[nodiscard]] usize root_length() const noexcept;

    // Empty storage reads as ".".
    Utf8String text_;
};

} // namespace stxlib
