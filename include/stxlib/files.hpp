#pragma once

// Files and directories (Capability::files), named by stxlib::Path. Ported
// from stx_auxlib's include/stx_auxlib/files.hpp and src/hosted/files.cpp at
// b6e330a. Every operation returns a Status or Result; none throws or
// aborts for a filesystem condition. Host errors map onto the same records
// on every host.
//
// Settled platform differences (stx_auxlib's):
// - read_file and list_directory take a caller limit and refuse larger
//   content before allocating for it.
// - write_file replaces a file through a uniquely named temporary file in
//   the same directory: written, flushed to stable storage (F_FULLFSYNC on
//   Apple hosts, fsync elsewhere on POSIX, write-through on Windows), then
//   renamed over the target, and on POSIX hosts the directory flushed too.
// - list_directory returns entries sorted by name bytes (unsigned), so every
//   host lists a directory in the same order.
// - Times are WallTime (Unix time) on every host.
//
// Survey D13 (files and paths) is open, and this is stx_auxlib's current
// behaviour, flagged where D13 would change it:
// - Binary I/O only, as D13's options all have.
// - One durability level (write_file is durable); D13 a would name levels.
// - Atomic replace: atomic on POSIX hosts (rename); on Windows MoveFileExW
//   with MOVEFILE_REPLACE_EXISTING, which Windows does not promise to be
//   atomic. D13 a would report that as a capability.
// - Host names that are not valid Unicode are refused (invalid_name) or, in
//   a listing, counted and left out: D13's option b. D13 a would carry them
//   losslessly.
//
// Changes from stx_auxlib: std::filesystem is not used (it cannot report
// exhaustion in a build without exceptions); each operation is the host's
// own call (src/hosted/<family>/file_<family>.cpp), with STXlib's buffers.
// Where std::filesystem behaviour was relied on, the host calls reproduce
// it: remove takes a file, a link or an empty directory; remove_tree removes
// without following links and counts what it removed, the top included;
// copy_file copies the permission bits on POSIX hosts and refuses to copy a
// file onto itself (already_exists); temporary_directory reads TMPDIR, TMP,
// TEMP, TEMPDIR and then "/tmp" on POSIX hosts, GetTempPathW on Windows,
// and must name a directory.
//
// Failures. The kind is the class, the code a HostCode (capabilities.hpp),
// and the host's code is the foreign code (errno, GetLastError):
//   stx_auxlib FileStatus  ->  kind, HostCode
//   not_found              ->  not_found
//   already_exists         ->  already_exists
//   permission_denied      ->  permission_denied
//   not_a_directory        ->  invalid_argument, not_a_directory
//   is_a_directory         ->  invalid_argument, is_a_directory
//   directory_not_empty    ->  invalid_argument, directory_not_empty
//   no_space               ->  limit_exceeded, no_space
//   too_large              ->  limit_exceeded, too_long
//   name_too_long          ->  limit_exceeded, name_too_long
//   invalid_name           ->  invalid_argument, invalid_name
//   unsupported            ->  unsupported (a drive path on a POSIX host)
//   cross_device           ->  unsupported, cross_device
//   busy                   ->  limit_exceeded, busy
//   bad_handle             ->  disposed, closed_handle
//   io_error               ->  io_failure
// and out_of_memory when STXlib's own buffers cannot be had.
//
// Concurrency: operations on distinct File objects may run on any threads.
// The filesystem itself is shared with other programs; nothing here can
// make a sequence of operations atomic against them.

#include "stxlib/buffer.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

enum class FileMode : u8 {
    read = 0,       // read an existing file
    create_new = 1, // write a new file; already_exists if the name is taken
    replace = 2,    // write a file, creating it or discarding its old content
    append = 3,     // write at the end of a file, creating it if missing
};

// An open file. Move-only; the destructor closes it and discards any close
// failure, so call close() where that failure matters. A moved-from or
// closed File fails with disposed, HostCode::closed_handle.
class File final {
public:
    // Opening a directory for reading fails with is_a_directory.
    [[nodiscard]] static Result<File> open(const Path& path, FileMode mode) noexcept;

    File() noexcept = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept;
    // Closes this file first, discarding any close failure.
    File& operator=(File&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != closed; }

    // Reads up to output.count() bytes at the current position. Zero bytes
    // means the end of the file.
    [[nodiscard]] Result<usize> read(MutableView<Byte> output) noexcept;
    // Writes every byte or fails; on failure an unknown prefix may have been
    // written.
    [[nodiscard]] Status write_all(View<Byte> input) noexcept;
    [[nodiscard]] Result<u64> size() noexcept;
    [[nodiscard]] Result<u64> position() noexcept;
    [[nodiscard]] Status seek(u64 offset) noexcept;
    // Waits until written content is on stable storage.
    [[nodiscard]] Status sync() noexcept;
    [[nodiscard]] Status close() noexcept;

private:
    static constexpr i64 closed = -1;

    explicit File(const i64 handle) noexcept : handle_(handle) {}

    // A POSIX descriptor or a Windows HANDLE's value; -1 when closed.
    i64 handle_{closed};
};

// The whole file. Refuses (limit_exceeded, HostCode::too_long) a file of
// more than `limit` bytes before allocating for it, and a file that grows
// past `limit` while it is read.
[[nodiscard]] Result<Buffer<Byte>> read_file(const Path& path, usize limit) noexcept;

// Atomically (see D13 above) and durably replaces or creates the file with
// `content`. On failure the old file, if any, is unchanged and the
// temporary file is removed. A path without a final name ("/", ".") fails
// with invalid_argument, HostCode::invalid_name.
[[nodiscard]] Status write_file(const Path& path, View<Byte> content) noexcept;

enum class FileKind : u8 {
    regular = 0,
    directory = 1,
    symbolic_link = 2,
    other = 3,
};

struct FileInformation final {
    FileKind kind{FileKind::other};
    u64 size{0}; // bytes, for regular files; 0 otherwise
    WallTime modified{};
};

enum class LinkMode : u8 {
    follow = 0,    // describe what a symbolic link points to
    no_follow = 1, // describe a symbolic link itself
};

// not_found when nothing has that name.
[[nodiscard]] Result<FileInformation> inspect(const Path& path, LinkMode links) noexcept;
// Creates one directory; already_exists if anything has that name.
[[nodiscard]] Status create_directory(const Path& path) noexcept;
// Creates a directory and its missing parents; succeeds if it already
// exists as a directory, and fails with not_a_directory if it exists as
// something else.
[[nodiscard]] Status create_directories(const Path& path) noexcept;
// Removes a file, a symbolic link or an empty directory; not_found when
// nothing has that name.
[[nodiscard]] Status remove(const Path& path) noexcept;
// Removes a directory tree without following symbolic links. Yields the
// number of entries removed, the top included; a missing path removes 0.
// Not atomic: on failure, part of the tree may be gone.
[[nodiscard]] Result<u64> remove_tree(const Path& path) noexcept;
// Renames, replacing an existing file at `to`.
[[nodiscard]] Status rename(const Path& from, const Path& to) noexcept;

enum class CopyMode : u8 {
    fail_if_exists = 0,
    replace = 1,
};

[[nodiscard]] Status copy_file(const Path& from, const Path& to, CopyMode mode) noexcept;

namespace detail {

// One listed entry: names_[name_offset, name_offset + name_count).
struct DirectoryEntryRecord final {
    usize name_offset{0};
    usize name_count{0};
    FileKind kind{FileKind::other};
};

class DirectoryListingBuilder;

} // namespace detail

// A directory's entries, without "." and "..", sorted by name bytes.
// Move-only; owns its names.
class DirectoryListing final {
public:
    DirectoryListing() noexcept = default;
    DirectoryListing(DirectoryListing&&) noexcept = default;
    DirectoryListing& operator=(DirectoryListing&&) noexcept = default;
    DirectoryListing(const DirectoryListing&) = delete;
    DirectoryListing& operator=(const DirectoryListing&) = delete;
    ~DirectoryListing() = default;

    [[nodiscard]] usize count() const noexcept { return entries_.count(); }
    // One segment: the entry's name within the directory. Borrowed from this
    // listing. Requirement: index < count().
    [[nodiscard]] TextView name(usize index) const& noexcept;
    TextView name(usize index) const&& = delete;
    // The entry itself, not what a link names. Requirement: index < count().
    [[nodiscard]] FileKind kind(usize index) const noexcept;
    // Entries whose host names are not valid Path segments (not Unicode, or
    // holding a reserved character): counted, not listed (D13, above).
    [[nodiscard]] usize unnamed_entries() const noexcept { return unnamed_; }

private:
    friend class detail::DirectoryListingBuilder;

    Buffer<char> names_;
    Buffer<detail::DirectoryEntryRecord> entries_;
    usize unnamed_{0};
};

// The directory's entries. Refuses (limit_exceeded, HostCode::too_long) a
// directory of more than `limit` entries, unnamed ones included.
[[nodiscard]] Result<DirectoryListing> list_directory(const Path& path, usize limit) noexcept;

// The host's directory for temporary files.
[[nodiscard]] Result<Path> temporary_directory() noexcept;
// The process's working directory.
[[nodiscard]] Result<Path> working_directory() noexcept;
// `path` made absolute against the working directory, lexically: no
// symbolic link is followed and the path need not exist.
[[nodiscard]] Result<Path> absolute(const Path& path) noexcept;
// The existing file's absolute path with every symbolic link resolved.
[[nodiscard]] Result<Path> resolve(const Path& path) noexcept;
// The target a symbolic link names, unresolved.
[[nodiscard]] Result<Path> read_link(const Path& path) noexcept;
// Creates a symbolic link at `link` naming `target`. Hosts that restrict
// links (Windows without developer mode) fail with permission_denied or
// unsupported.
[[nodiscard]] Status create_link(const Path& target, const Path& link) noexcept;

} // namespace stxlib
