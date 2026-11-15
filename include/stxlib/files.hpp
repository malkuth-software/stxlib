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
// - list_directory returns entries sorted by their Path text's bytes
//   (unsigned), so every host lists a directory in the same order.
// - Times are WallTime (Unix time) on every host.
//
// Storage (house style §6; audit A, X10): read_file and list_directory
// return storage of their own (a Buffer, a DirectoryListing) instead of
// writing into storage the caller supplies. Each is a one-shot read whose
// size the caller's limit bounds before anything is allocated, and its
// result is the caller's to keep; no repeated hot path reads files, so no
// caller-storage form is offered until one does.
//
// Owner decision D13, option a (notes decisions/2026-11-13-d13-files-paths.md,
// "Carry losslessly"):
// - Binary I/O only: files are bytes; text encodings are the caller's.
// - Named durability levels (Durability, below), for File::sync and
//   write_file.
// - Atomic replace is a capability: Capability::atomic_replace reports
//   whether write_file's final rename is atomic on this host (true on POSIX
//   hosts, whose rename(2) is specified atomic; false on Windows, which does
//   not document MoveFileExW as atomic).
// - Every host name is reachable: Path carries names that are not valid
//   Unicode losslessly (path.hpp), so listings name every entry and every
//   name a host returns becomes a Path.
// write_file replaces a file through a uniquely named temporary file in the
// same directory: written, flushed as its Durability says, then renamed over
// the target.
//
// Changes from stx_auxlib: std::filesystem is not used (it cannot report
// exhaustion in a build without exceptions); each operation is the host's
// own call (src/hosted/<family>/file_<family>.cpp), with STXlib's buffers.
// Where std::filesystem behaviour was relied on, the host calls reproduce
// it: remove takes a file, a link or an empty directory; remove_tree removes
// without following links and counts what it removed, the top included;
// copy_file copies the permission bits on POSIX hosts (st_mode & 07777: the
// setuid, setgid and sticky bits included, as std::filesystem::copy_file
// does, where cp(1) without -p clears setuid and setgid) and refuses to copy a
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
//   (a Path no POSIX name spells: one holding a surrogate that is not a
//   POSIX escape, made from a Windows name)  ->  invalid_argument, invalid_name
//   (on Emscripten's JavaScript file system, MEMFS and IDBFS, a name that
//   is not UTF-8: that file system would store it under another name)
//                          ->  invalid_argument, invalid_name
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

// How far a flush goes before it returns (D13 a). Host facts relied on:
// - macOS fsync(2) (man page, read on this host): fsync moves data to the
//   drive, which may hold and reorder it; F_FULLFSYNC (fcntl(2)) asks the
//   drive to write everything it holds; F_BARRIERFSYNC (fcntl(2)) is fsync
//   plus a barrier: I/O issued before it is persisted before I/O issued
//   after it.
// - Linux fsync(2): flushes the file's data and metadata to the device and
//   waits; POSIX fsync is the portable form. A rename is durable once the
//   directory holding it is flushed (fsync on a directory descriptor).
// - Windows FlushFileBuffers flushes a file's buffers to the device;
//   MoveFileExW with MOVEFILE_WRITE_THROUGH returns only after the move is
//   flushed to disk (Microsoft's MoveFileExW documentation).
// (The Linux and Windows statements are from those pages as published, not
// re-read in this batch.)
enum class Durability : u8 {
    // No flush: the host writes when it likes. After a crash, a write_file
    // target may hold the old content, the new, or (on hosts that do not
    // order data before renames) an empty or partial file.
    none = 0,
    // The content reaches stable storage before anything written after it:
    // F_BARRIERFSYNC on Apple hosts, fsync elsewhere on POSIX,
    // FlushFileBuffers on Windows. After a crash, a write_file target holds
    // the old content or the new, whole; the replacement itself may be lost.
    ordered = 1,
    // The content is on stable storage when the call returns: F_FULLFSYNC on
    // Apple hosts (fsync where a filesystem refuses it), fsync elsewhere on
    // POSIX, FlushFileBuffers on Windows; write_file also makes the rename
    // durable (the directory flushed on POSIX hosts, MOVEFILE_WRITE_THROUGH
    // on Windows). After write_file returns, a crash leaves the new content.
    durable = 2,
};

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

enum class FileMode : u8 {
    read = 0,       // read an existing file
    create_new = 1, // write a new file; already_exists if the name is taken
    replace = 2,    // write a file, creating it or discarding its old content
    append = 3,     // write at the end of a file, creating it if missing
};

namespace detail {
// STXlib's own access to an open File's host handle (src/core/files.cpp).
class FileAccess;
} // namespace detail

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
    // Flushes written content as `level` says (Durability, above).
    [[nodiscard]] Status sync(Durability level) noexcept;
    [[nodiscard]] Status close() noexcept;

    // ---- positioned I/O and the open file's metadata (M2 C2) ----
    // Each leaves the position unchanged on every host (POSIX pread and
    // pwrite do not move it; on Windows, where ReadFile and WriteFile with
    // an offset move a synchronous handle's pointer, the backend puts it
    // back).

    // Reads up to output.count() bytes at `offset`; zero bytes means the
    // offset is at or past the end. Offsets above 2^63 - 1 fail with
    // limit_exceeded, HostCode::too_long.
    [[nodiscard]] Result<usize> read_at(u64 offset, MutableView<Byte> output) noexcept;
    // Writes every byte at `offset`, extending the file as needed; on
    // failure an unknown prefix may have been written. A file opened with
    // FileMode::append writes only at its end (O_APPEND; FILE_APPEND_DATA,
    // and Linux's pwrite ignores the offset under O_APPEND), so it is
    // refused with invalid_argument, HostCode::invalid_state, as is a file
    // opened for reading.
    [[nodiscard]] Status write_all_at(u64 offset, View<Byte> input) noexcept;
    // The open file's kind, size and last-write time (fstat;
    // GetFileInformationByHandle): the file this File holds, even if its
    // name has since been renamed or replaced.
    [[nodiscard]] Result<FileInformation> information() noexcept;
    // Sets the last-write time (futimens with the access time left as it
    // is; SetFileTime; on WasmFS, utimensat with an empty name). The host
    // keeps it at its own resolution: 1 ns on APFS, ext4 and UFS2, 100 ns
    // on NTFS, 1 ms on both Emscripten file systems (WallTime is cut,
    // toward the past, to the host's tick). A file opened for reading is refused
    // with invalid_argument, HostCode::invalid_state.
    [[nodiscard]] Status set_modified(WallTime modified) noexcept;
    // Cuts or extends the file to `bytes` (ftruncate; SetFileInformationByHandle
    // with FileEndOfFileInfo); extended bytes read as zero. The position is
    // unchanged. Refused, invalid_argument with HostCode::invalid_state, for
    // a file opened for reading or with FileMode::append (Windows' append
    // access cannot set the end).
    [[nodiscard]] Status set_size(u64 bytes) noexcept;

private:
    friend class detail::FileAccess;

    static constexpr i64 closed = -1;

    File(const i64 handle, const FileMode opened_mode) noexcept : handle_(handle), mode_(opened_mode) {}

    // A POSIX descriptor or a Windows HANDLE's value; -1 when closed.
    i64 handle_{closed};
    // The mode it was opened with, for the refusals above.
    FileMode mode_{FileMode::read};
};

// The whole file. Refuses (limit_exceeded, HostCode::too_long) a file of
// more than `limit` bytes before allocating for it, and a file that grows
// past `limit` while it is read.
[[nodiscard]] Result<Buffer<Byte>> read_file(const Path& path, usize limit) noexcept;

// Replaces or creates the file with `content` through a temporary file and a
// rename, flushed as `level` says (Durability, above). The rename is atomic
// where Capability::atomic_replace is true. On failure the old file, if any,
// is unchanged and the temporary file is removed. A path without a final
// name ("/", ".") fails with invalid_argument, HostCode::invalid_name.
// The replacement is a new file: on POSIX hosts it takes the replaced
// regular file's permission bits (st_mode & 0777, read through a link;
// setuid, setgid and sticky are dropped) before any content is written,
// and is owned by the writer; ownership, ACLs, extended attributes and a
// link at the name are not kept (a link is replaced by the file). On
// Windows it takes the directory's inherited ACL, not the replaced file's
// own.
[[nodiscard]] Status write_file(const Path& path, View<Byte> content, Durability level) noexcept;

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
// The deepest directory remove_tree descends by default (levels below the
// top).
inline constexpr usize maximum_remove_tree_depth = 4096;

// remove_tree's bounds, the caller's control (house style section 1, audit
// A, C3). `depth` (the top is depth 0) bounds the names held at once, one
// per directory entered; `entries_per_directory` bounds the entries removed
// from any one directory. The defaults hold any tree a desktop application
// writes.
struct RemoveTreeLimits final {
    usize entries_per_directory{static_cast<usize>(1) << 20};
    usize depth{maximum_remove_tree_depth};
};

// Removes a directory tree without following symbolic links. Yields the
// number of entries removed, the top included; a missing path removes 0,
// and a path naming anything but a directory removes that one entry. Not
// atomic: on failure, part of the tree may be gone (and what remains is a
// tree that a later call can remove).
//
// Safe against another program swapping a directory inside the tree for a
// link while it runs (the race libc++'s Windows remove_all has, C-37 of
// audit C): only `path` itself is looked up by name.
//   - POSIX hosts: each directory is opened relative to its parent's
//     descriptor with O_NOFOLLOW | O_DIRECTORY (openat), and its entries are
//     removed relative to it (unlinkat), so a link is removed, never
//     followed. One descriptor is open at a time: on the way back up the
//     parent is reopened through ".." and checked to be the directory left
//     (device and inode); a parent that changed fails with limit_exceeded,
//     HostCode::busy.
//   - Windows: each directory is held open, without FILE_SHARE_DELETE, from
//     when it is entered until it is removed, so no other program can
//     rename or delete it, and the path through the held directories
//     cannot be rebound; each entry is opened with
//     FILE_FLAG_OPEN_REPARSE_POINT and judged by that handle (a symbolic
//     link or junction is removed, never entered) and removed through it
//     (FileDispositionInfoEx with POSIX semantics, so the name goes at once;
//     FileDispositionInfo where the volume refuses that). Read-only files are
//     removed, as unlink removes them on POSIX hosts.
// Bounded: the work held at once is one directory's position (a listing is
// read in passes, not stored) and the names of the directories entered.
// Deeper than limits.depth levels, or more than limits.entries_per_directory
// entries removed from one directory, fails with limit_exceeded,
// HostCode::too_long (what was removed before stays removed). Terminates
// absent concurrent writers: each pass over a directory removes an entry or
// finds it empty.
[[nodiscard]] Result<u64> remove_tree(const Path& path, const RemoveTreeLimits& limits = {}) noexcept;
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

// A directory's entries, without "." and "..", sorted by their names' Path
// text bytes. Move-only; owns its names.
class DirectoryListing final {
public:
    DirectoryListing() noexcept = default;
    DirectoryListing(DirectoryListing&&) noexcept = default;
    DirectoryListing& operator=(DirectoryListing&&) noexcept = default;
    DirectoryListing(const DirectoryListing&) = delete;
    DirectoryListing& operator=(const DirectoryListing&) = delete;
    ~DirectoryListing() = default;

    [[nodiscard]] usize count() const noexcept { return entries_.count(); }
    // One segment: the entry's name within the directory, as Path text (a
    // name that is not valid Unicode is carried, path.hpp). Borrowed from
    // this listing. Requirement: index < count().
    [[nodiscard]] PathText name(usize index) const& noexcept;
    PathText name(usize index) const&& = delete;
    // The entry itself, not what a link names. Requirement: index < count().
    [[nodiscard]] FileKind kind(usize index) const noexcept;
    // Entries whose names no Path segment can spell, counted and not listed.
    // Always 0 on POSIX hosts, where every name is carried (path.hpp). On
    // Windows, an entry whose name holds '/', '\' or ':', which Win32 file
    // names cannot hold (Microsoft, "Naming Files, Paths, and Namespaces")
    // and so no Win32 path reaches; such names exist only when made through
    // the NT namespace below Win32.
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

// The user's directories (Capability::known_directories; replaces
// GUI.Forms' HOME, APPDATA and XDG_CONFIG_HOME reading in theme_files.cpp,
// A0 inventory section 2).
enum class KnownDirectory : u8 {
    home = 0,               // the user's home directory
    user_configuration = 1, // settings the user keeps (and roams, on Windows)
    user_data = 2,          // data the program keeps for the user
    user_cache = 3,         // data the program can rebuild; may be removed by the host
};

// The directory `which` names, as each host's convention defines it. The
// directory need not exist (a fresh account may lack it); create it with
// create_directories before writing into it.
//   - macOS: home is HOME, else the user database's entry (getpwuid_r);
//     user_configuration and user_data are home/Library/Application Support,
//     user_cache home/Library/Caches (Apple's File System Programming Guide;
//     unconfirmed at a pin).
//   - Linux and FreeBSD (the XDG Base Directory Specification 0.8, as
//     published; unconfirmed at a pin): home as on macOS;
//     user_configuration is XDG_CONFIG_HOME, else home/.config; user_data
//     XDG_DATA_HOME, else home/.local/share; user_cache XDG_CACHE_HOME, else
//     home/.cache. A variable that is empty or not an absolute path is
//     ignored, as the specification requires.
//   - Windows: SHGetKnownFolderPath with FOLDERID_Profile, RoamingAppData,
//     RoamingAppData and LocalAppData, in order of the enumerators. A
//     folder redirected to a UNC share ("\\server\share") is refused as
//     unsupported: a Path has no UNC form, as with the file functions.
// The variables are read through environment.hpp, under its lock. Failures:
// not_found with HostCode::no_home when no home can be named; invalid_argument
// for a value the variable holds that is not a Path; out_of_memory;
// foreign_failure with the host's code (Windows); unsupported for a value
// outside the set or a folder on a UNC share (Windows).
[[nodiscard]] Result<Path> known_directory(KnownDirectory which) noexcept;
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
