#pragma once

// The program's one path type: UTF-8 text with '/' separators, validated
// and normalized when it is made, so that a Path means the same on every
// host. Conversion to a host's native form happens only inside the
// interface modules (src/hosted/). Path is pure computation and touches no
// filesystem. Ported from stx_auxlib's include/stx_auxlib/paths.hpp and
// src/paths.cpp at b6e330a.
//
// Text. A Path's text is WTF-8 (src/core/wtf8.hpp): UTF-8, plus the
// surrogate code points U+D800..U+DFFF encoded as UTF-8's 3-byte form would,
// never a high surrogate directly followed by a low one. Text made from
// Unicode (parse, parse_bytes) is plain UTF-8; surrogates appear only to
// carry a host name that is not valid Unicode, losslessly (owner, D13
// option a, notes decisions/2026-11-13-d13-files-paths.md: "Carry
// losslessly"; no real file is unreachable because of its name):
// - Windows: a name is 16-bit units, and NTFS does not check that
//   surrogates pair; each unpaired surrogate is carried as itself.
// - POSIX: a name is bytes (any byte but '/' and NUL; POSIX.1-2017, Base
//   Definitions 3.170). A byte that is not part of well-formed UTF-8, and
//   ':' and '\', are carried as the surrogate U+DC00 + byte (the scheme of
//   Python's surrogateescape, PEP 383, extended to the two characters Path
//   refuses in its own syntax).
// from_posix_name/posix_name and from_windows_name/windows_name convert,
// and each round trip is exact (tests/path_names_exhaustive.cpp). PathText
// is the borrowed view of such text; PathText::is_unicode says whether it
// is plain UTF-8, and display() shows it with U+FFFD for each surrogate.
//
// Storage (house style §6; audit A, X10). Each factory returns a Path that
// owns its text, at most maximum_path_bytes. Text from outside (parse and
// the host-name factories) is refused above that bound before anything is
// allocated; join's working storage is bounded by its two Paths. A Path is
// a value the caller keeps, not a scratch output, so no caller-storage form
// is offered.
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
// text that is not well-formed (invalid_utf8: UTF-8 for parse_bytes, WTF-8
// for parse_wtf8; a TextView is UTF-8 by construction), '\' and ':' outside
// a drive root. '\' and ':' are refused because hosts disagree on them ('\'
// separates on Windows, ':' names streams on NTFS and drives elsewhere);
// refusing them keeps a path's meaning identical everywhere, and the POSIX
// escape carries a host name that holds them.
//
// Comparison is by bytes. The host conversions are injective (posix_name
// refuses text that is not exactly the escape of its bytes), so two unequal
// Paths never name one host name. Hosts whose filesystems ignore case or
// normalize Unicode (APFS, NTFS) may still treat two unequal Paths as one
// file.
//
// Hosts: POSIX hosts support "/" roots and report drive roots unsupported
// (files.hpp); Windows supports both ("/" is the current drive's root). UNC
// names ("//server/share") are not supported: their leading "//" collapses.
//
// Changes from stx_auxlib: a Path owns its text on STXlib's allocator; it
// copies as an ordinary value (owner 2026-11-16: exhaustion during a copy
// fails fast; copy() reports it); every other operation that allocates
// returns an OperationResult, so exhaustion is out_of_memory, not an abort; join and
// parent bound only their result (stx_auxlib refused a join whose operands
// together exceeded the bound, and parent returned the path unchanged when
// "<path>/.." did); host names that are not valid Unicode are carried, not
// refused (D13 a, above), and the bound is 98,304 bytes, not 4,096, so that
// carrying them never makes a host path too long (maximum_path_bytes).
//
// Failures: invalid_argument with the PathFailure as the record's code, or
// limit_exceeded with PathFailure::too_long; out_of_memory.

#include "stxlib/buffer.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

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
    // posix_name: the text holds a surrogate that is not a POSIX escape, or
    // an escape the conversion would not have made, so it names no POSIX
    // file.
    not_representable = 8,
};

namespace detail {
class PathTextAccess;
} // namespace detail

// Borrowed WTF-8 text: a Path's text or part of it (see above). Lifetime: as
// the storage it was made from.
class PathText final {
public:
    // The empty text.
    constexpr PathText() noexcept = default;
    // Unicode text is path text. Implicit, since they mean the same.
    constexpr PathText(const TextView text) noexcept : bytes_(text.data()), count_(text.count()) {}
    // A string literal, checked as TextView checks it.
    template <usize Count>
    consteval PathText(const char (&literal)[Count]) noexcept : PathText(TextView(literal)) {}

    [[nodiscard]] constexpr const char* data() const noexcept { return bytes_; }
    [[nodiscard]] constexpr usize count() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

    // True when the text holds no surrogate: it is then UTF-8.
    [[nodiscard]] bool is_unicode() const noexcept;
    // The same text as a TextView. Requirement: is_unicode().
    [[nodiscard]] TextView unicode() const noexcept;
    // The text for people to read, written to room[0, count()) with U+FFFD
    // for each surrogate (both are three bytes, so the length is the same).
    // Lossy: use it to show a name, never to name a file. Borrowed from
    // room. Requirement: room.count() >= count().
    [[nodiscard]] TextView display(MutableView<char> room) const noexcept;

    // Byte-wise, as TextView compares.
    [[nodiscard]] friend constexpr bool operator==(const PathText left, const PathText right) noexcept {
        if (left.count_ != right.count_) return false;
        for (usize index = 0; index < left.count_; ++index) {
            if (left.bytes_[index] != right.bytes_[index]) return false;
        }
        return true;
    }
    [[nodiscard]] friend constexpr bool operator!=(const PathText left, const PathText right) noexcept {
        const bool equal = left == right;
        return !equal;
    }

private:
    friend class detail::PathTextAccess;

    constexpr PathText(const char* const validated_bytes, const usize byte_count) noexcept
        : bytes_(validated_bytes), count_(byte_count) {}

    const char* bytes_{nullptr};
    usize count_{0};
};

namespace detail {

// For STXlib's own code that holds text it has checked as WTF-8.
class PathTextAccess final {
public:
    // Requirement: bytes[0, count) is well-formed WTF-8 and outlives the view.
    [[nodiscard]] static constexpr PathText adopt_validated(const char* const bytes, const usize count) noexcept {
        const PathText result(bytes, count);
        return result;
    }
};

} // namespace detail

// Bounds every Path, before and after normalization, in bytes: 3 x 32,768,
// so that no host path a host's own calls accept is too long once carried
// (D13 a). The longest is a Windows path of 32,767 UTF-16 units (the \\?\
// limit), each at most 3 bytes of WTF-8; a POSIX path the host accepts
// (PATH_MAX: 4,096 bytes on Linux, 1,024 on macOS) grows at most three
// times when every byte is escaped. stx_auxlib's bound, 4,096, refused
// such names as too_long.
inline constexpr usize maximum_path_bytes = 98304;

class Path final {
public:
    // The relative path ".". Allocates nothing.
    Path() noexcept = default;

    // Validates and normalizes `text`.
    [[nodiscard]] static OperationResult<Path> parse(TextView text) noexcept;
    // As parse, for foreign bytes not yet known to be UTF-8. Requirement (for
    // each function taking a pointer and a count): the pointer is non-null
    // unless the count is zero.
    [[nodiscard]] static OperationResult<Path> parse_bytes(const char* bytes, usize count) noexcept;
    // As parse, for bytes that must be well-formed WTF-8 (the text of a
    // Path, stored and read back).
    [[nodiscard]] static OperationResult<Path> parse_wtf8(const char* bytes, usize count) noexcept;
    // A POSIX host's name: any bytes, '/' separating. Bytes that are not
    // well-formed UTF-8, ':' and '\' are escaped (see above); a NUL is
    // refused (embedded_nul), as no POSIX name holds one.
    [[nodiscard]] static OperationResult<Path> from_posix_name(const char* bytes, usize count) noexcept;
    // A Windows host's name: 16-bit units, '\' or '/' separating, without the
    // "\\?\" prefix. Unpaired surrogates are carried; ':' is refused outside
    // a drive root (reserved_character), as Windows reserves it (Microsoft,
    // "Naming Files, Paths, and Namespaces").
    [[nodiscard]] static OperationResult<Path> from_windows_name(const u16* units, usize count) noexcept;

    Path(Path&&) noexcept = default;
    Path& operator=(Path&&) noexcept = default;
    // The ordinary copy (owner 2026-11-16): a second Path with the same
    // text. An allocation it cannot have ends the process through fail_fast
    // with copy()'s out_of_memory record. Assignment makes the copy whole
    // first, then moves it in; self-assignment changes nothing.
    Path(const Path& other) noexcept;
    Path& operator=(const Path& other) noexcept;
    ~Path() = default;

    // A second Path with the same text.
    [[nodiscard]] OperationResult<Path> copy() const noexcept;

    // Borrowed from this path: valid until it is moved from or destroyed. A
    // moved-from Path reads ".".
    [[nodiscard]] PathText text() const& noexcept;
    PathText text() const&& = delete;

    // The POSIX host's name for this path, followed by a NUL that is the
    // buffer's last element. Fails with not_representable (see PathFailure)
    // or out_of_memory. A drive root ("C:/") is not representable: its ':'
    // is not an escape.
    [[nodiscard]] OperationResult<Buffer<char>> posix_name() const noexcept;
    // The Windows host's name for this path, '\' separating, followed by a
    // NUL unit that is the buffer's last element; no "\\?\" prefix. Fails
    // only with out_of_memory.
    [[nodiscard]] OperationResult<Buffer<u16>> windows_name() const noexcept;

    [[nodiscard]] bool is_absolute() const noexcept;
    // True for "/" and "X:/" alone.
    [[nodiscard]] bool is_root() const noexcept;
    // The drive letter of a "X:/" root, or 0.
    [[nodiscard]] char drive() const noexcept;

    // This path followed by `relative`, normalized. Fails with absolute_join
    // when `relative` is absolute, and with escapes_root or too_long as parse
    // does (for the result).
    [[nodiscard]] OperationResult<Path> join(const Path& relative) const noexcept;
    // The path without its last segment: "a/b" -> "a", "a" -> ".",
    // "." -> "..", ".." -> "../..", and a root is its own parent. Fails with
    // too_long ("../.." past the bound) or out_of_memory.
    [[nodiscard]] OperationResult<Path> parent() const noexcept;

    // The last segment, or empty for a root, "." and a path ending in "..".
    // Borrowed from this path.
    [[nodiscard]] PathText file_name() const& noexcept;
    PathText file_name() const&& = delete;
    // The last segment's text from its last '.', including the dot
    // ("a.tar.gz" -> ".gz"); empty when there is none or the name starts with
    // its only dot (".profile"). Borrowed from this path.
    [[nodiscard]] PathText extension() const& noexcept;
    PathText extension() const&& = delete;

    [[nodiscard]] bool operator==(const Path& other) const noexcept;
    [[nodiscard]] bool operator!=(const Path& other) const noexcept;

    // For src/core/path.cpp only: adopts normalized text.
    struct Access;

private:
    friend struct Access;

    explicit Path(Buffer<char> normalized) noexcept : text_(static_cast<Buffer<char>&&>(normalized)) {}

    // The length of the root prefix: 0, 1 ("/") or 3 ("X:/").
    [[nodiscard]] usize root_length() const noexcept;

    // Well-formed, normalized WTF-8; empty storage reads as ".".
    Buffer<char> text_;
};

} // namespace stxlib
