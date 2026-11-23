// The POSIX file backend (hosted/file_backend.hpp), for macOS, glibc and
// musl. Ported from stx_auxlib's src/hosted/file_posix.cpp at b6e330a, with
// the operations stx_auxlib took from std::filesystem written over the
// POSIX calls std::filesystem itself uses (mkdir, rmdir and unlink, rename,
// opendir and readdir, getcwd, realpath, readlink, symlink).
//
// Durability levels (files.hpp, Durability; owner decision D13 a). macOS
// fcntl(2) and fsync(2), read on this host: fsync leaves data in the drive,
// which may reorder it; F_FULLFSYNC makes the drive write everything it
// holds; F_BARRIERFSYNC is fsync plus a barrier, so I/O issued before it is
// persisted before I/O issued after. durable uses F_FULLFSYNC (stx_auxlib's
// flush_descriptor), ordered F_BARRIERFSYNC; elsewhere both use fsync, the
// only flush POSIX offers.
//
// Emscripten (owner decision 2026-11-15): the same calls over Emscripten's
// file system, with each level decided by the volume holding the file
// (hosted/web_backend.hpp, FlushRule; web.hpp has the table): memory
// volumes accept every level as a native tmpfs does, IDBFS honours ordered
// and refuses durable, and anything else is fsync. A durable replace is
// refused before its rename where the rename cannot be made durable, so
// the target is left as it was (early-exit rule 1: the refusal comes
// before any change).
//
// Names are the host's bytes both ways (file_backend.hpp, "Names").

#include "hosted/file_backend.hpp"

#include "stxlib/environment.hpp"

#if defined(__EMSCRIPTEN__)
#include "hosted/web_backend.hpp"
#endif

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace stxlib::file_backend {

namespace {

// One read or write moves at most this much, below every host's ssize_t
// limit.
constexpr usize largest_transfer = static_cast<usize>(1) << 30;

// stx_auxlib's status_from (files.cpp), restated over errno values.
FileStatus status_of_errno(const int error) noexcept {
    switch (error) {
    case ENOENT:
        return FileStatus::not_found;
    case EEXIST:
        return FileStatus::already_exists;
    case EACCES:
    case EPERM:
    case EROFS:
        return FileStatus::permission_denied;
    case ENOTDIR:
        return FileStatus::not_a_directory;
    case EISDIR:
        return FileStatus::is_a_directory;
    case ENOTEMPTY:
        return FileStatus::directory_not_empty;
    case ENOSPC:
        return FileStatus::no_space;
    case EFBIG:
    case EOVERFLOW:
        return FileStatus::too_large;
    case ENAMETOOLONG:
        return FileStatus::name_too_long;
    case EILSEQ:
        return FileStatus::invalid_name;
    case EXDEV:
        return FileStatus::cross_device;
    case EBUSY:
    case ETXTBSY:
        return FileStatus::busy;
    case EBADF:
        return FileStatus::bad_handle;
    case ENOSYS:
    case ENOTSUP:
#if EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP:
#endif
        return FileStatus::unsupported;
    case ENOMEM:
        return FileStatus::out_of_memory;
    default:
        break;
    }
    return FileStatus::io_error;
}

Outcome from_errno(const int error) noexcept {
    const Outcome result{status_of_errno(error), ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error))};
    return result;
}

Outcome no_memory() noexcept {
    const Outcome result = failure(FileStatus::out_of_memory);
    return result;
}

int descriptor(const i64 handle) noexcept {
    const int result = static_cast<int>(handle);
    return result;
}

int open_flags(const FileMode mode) noexcept {
    switch (mode) {
    case FileMode::read:
        return O_RDONLY | O_CLOEXEC;
    case FileMode::create_new:
        return O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
    case FileMode::replace:
        return O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    case FileMode::append:
        return O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
    }
    return -1;
}

int open_retrying(const char* const name, const int flags, const mode_t permissions) noexcept {
    int fd = ::open(name, flags, permissions);
    while (fd < 0 && errno == EINTR) fd = ::open(name, flags, permissions);
    return fd;
}

// Flushes the descriptor's content as `level` says; Durability::none does
// nothing. Emscripten: the volume's rule first (the header comment); a
// refusal is -1 with errno ENOTSUP, which status_of_errno reports as
// unsupported.
int flush_descriptor(const int fd, const Durability level) noexcept {
    if (level == Durability::none) return 0;
#if defined(__EMSCRIPTEN__)
    const web_backend::FlushRule rule = web_backend::flush_rule(fd);
    if (rule == web_backend::FlushRule::vacuous) return 0;
    if (rule == web_backend::FlushRule::snapshot) {
        if (level == Durability::ordered) return 0;
        errno = ENOTSUP;
        return -1;
    }
#endif
#if defined(F_FULLFSYNC)
    // Some filesystems (network volumes, RAM disks) refuse F_FULLFSYNC;
    // fsync is then the strongest flush they offer.
    if (level == Durability::durable && ::fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
#if defined(F_BARRIERFSYNC)
    // The same fallback when a filesystem refuses the barrier.
    if (level == Durability::ordered && ::fcntl(fd, F_BARRIERFSYNC) == 0) return 0;
#endif
    int result = ::fsync(fd);
    while (result != 0 && errno == EINTR) result = ::fsync(fd);
    return result;
}

FileKind kind_of_mode(const mode_t mode) noexcept {
    if (S_ISREG(mode)) return FileKind::regular;
    if (S_ISDIR(mode)) return FileKind::directory;
    if (S_ISLNK(mode)) return FileKind::symbolic_link;
    return FileKind::other;
}

// Owns a descriptor for one call; -1 holds none.
class Descriptor final {
public:
    explicit Descriptor(const int fd) noexcept : fd_(fd) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() {
        // Lets go before closing (AGENTS.md, rule 1).
        const int held = fd_;
        fd_ = -1;
        if (held >= 0) (void)::close(held);
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    // Closes now; the close's errno, or 0.
    [[nodiscard]] int close_now() noexcept {
        const int fd = fd_;
        fd_ = -1;
        if (::close(fd) != 0 && errno != EINTR) return errno;
        return 0;
    }

private:
    int fd_;
};

// Owns an open directory stream for one call.
class DirectoryStream final {
public:
    explicit DirectoryStream(DIR* const stream) noexcept : stream_(stream) {}
    DirectoryStream(const DirectoryStream&) = delete;
    DirectoryStream& operator=(const DirectoryStream&) = delete;
    ~DirectoryStream() {
        // Lets go before closing (AGENTS.md, rule 1).
        DIR* const held = stream_;
        stream_ = nullptr;
        if (held != nullptr) (void)::closedir(held);
    }

    [[nodiscard]] DIR* get() const noexcept { return stream_; }

private:
    DIR* stream_;
};

// The last-write time of a stat result, as WallTime. tv_sec * 10^9 stays
// in i64 for every time WallTime can hold; seconds beyond its range (past
// 2262) are clamped to its ends (stx_auxlib's).
WallTime modified_of(const struct stat& described) noexcept {
#if defined(__APPLE__)
    const struct timespec modified = described.st_mtimespec;
#else
    const struct timespec modified = described.st_mtim;
#endif
    constexpr i64 largest_seconds = 9223372035;
    constexpr i64 smallest_seconds = -9223372035;
    i64 seconds = static_cast<i64>(modified.tv_sec);
    if (seconds > largest_seconds) seconds = largest_seconds;
    if (seconds < smallest_seconds) seconds = smallest_seconds;
    const i64 count = detail::saturating_add(seconds * 1000000000LL, static_cast<i64>(modified.tv_nsec));
    const WallTime result = WallTime::from_unix_nanoseconds(count);
    return result;
}

void describe(const struct stat& described, FileInformation& information) noexcept {
    information.kind = kind_of_mode(described.st_mode);
    information.size = 0;
    if (information.kind == FileKind::regular) {
        information.size = described.st_size < 0 ? 0 : static_cast<u64>(described.st_size);
    }
    information.modified = modified_of(described);
}

// Copies a NUL-terminated host string into `text`.
Outcome copy_text(const char* const host, Growable<char>& text) noexcept {
    text.clear();
    if (!text.append(host, std::strlen(host))) return no_memory();
    return success();
}

} // namespace

Outcome open(const char* const name, const FileMode mode, i64& handle) noexcept {
    handle = no_handle;
    const int flags = open_flags(mode);
    if (flags < 0) return failure(FileStatus::io_error);
    const int fd = open_retrying(name, flags, 0666);
    if (fd < 0) return from_errno(errno);
    if (mode == FileMode::read) {
        struct stat information{};
        if (::fstat(fd, &information) != 0) {
            const int error = errno;
            (void)::close(fd);
            return from_errno(error);
        }
        if (S_ISDIR(information.st_mode)) {
            (void)::close(fd);
            return failure(FileStatus::is_a_directory);
        }
    }
    handle = static_cast<i64>(fd);
    return success();
}

Outcome read(const i64 handle, Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    ssize_t got = ::read(descriptor(handle), bytes, request);
    while (got < 0 && errno == EINTR) got = ::read(descriptor(handle), bytes, request);
    if (got < 0) return from_errno(errno);
    transferred = static_cast<usize>(got);
    return success();
}

Outcome write(const i64 handle, const Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    ssize_t put = ::write(descriptor(handle), bytes, request);
    while (put < 0 && errno == EINTR) put = ::write(descriptor(handle), bytes, request);
    if (put < 0) return from_errno(errno);
    if (put == 0 && request != 0) return failure(FileStatus::io_error);
    transferred = static_cast<usize>(put);
    return success();
}

Outcome size(const i64 handle, u64& bytes) noexcept {
    bytes = 0;
    struct stat information{};
    if (::fstat(descriptor(handle), &information) != 0) return from_errno(errno);
    if (information.st_size < 0) return failure(FileStatus::io_error);
    bytes = static_cast<u64>(information.st_size);
    return success();
}

Outcome position(const i64 handle, u64& offset) noexcept {
    offset = 0;
    const off_t current = ::lseek(descriptor(handle), 0, SEEK_CUR);
    if (current < 0) return from_errno(errno);
    offset = static_cast<u64>(current);
    return success();
}

Outcome seek(const i64 handle, const u64 offset) noexcept {
    // off_t is 64 bits on every phase I POSIX host (_FILE_OFFSET_BITS is 64
    // on 64-bit glibc and musl; macOS has only a 64-bit off_t).
    static_assert(sizeof(off_t) == 8, "a 64-bit off_t");
    if (offset > static_cast<u64>(i64_max)) return failure(FileStatus::too_large);
    if (::lseek(descriptor(handle), static_cast<off_t>(offset), SEEK_SET) < 0) return from_errno(errno);
    return success();
}

Outcome sync(const i64 handle, const Durability level) noexcept {
    if (flush_descriptor(descriptor(handle), level) != 0) return from_errno(errno);
    return success();
}

// POSIX leaves a descriptor's state unspecified after close fails with
// EINTR; Linux and macOS release it, so close is never retried.
Outcome close(const i64 handle) noexcept {
    if (::close(descriptor(handle)) != 0 && errno != EINTR) return from_errno(errno);
    return success();
}

// ---- positioned I/O and the open file's metadata (M2 C2) ----
// pread and pwrite take the offset and leave the descriptor's position
// (POSIX.1-2017, pread). off_t is 64 bits (static_assert in seek), and core
// passes offsets of at most 2^63 - 1.

Outcome read_at(const i64 handle, const u64 offset, Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    const off_t at = static_cast<off_t>(offset);
    ssize_t got = ::pread(descriptor(handle), bytes, request, at);
    while (got < 0 && errno == EINTR) got = ::pread(descriptor(handle), bytes, request, at);
    if (got < 0) return from_errno(errno);
    transferred = static_cast<usize>(got);
    return success();
}

Outcome write_at(const i64 handle, const u64 offset, const Byte* const bytes, const usize count,
                 usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    const off_t at = static_cast<off_t>(offset);
    ssize_t put = ::pwrite(descriptor(handle), bytes, request, at);
    while (put < 0 && errno == EINTR) put = ::pwrite(descriptor(handle), bytes, request, at);
    if (put < 0) return from_errno(errno);
    if (put == 0 && request != 0) return failure(FileStatus::io_error);
    transferred = static_cast<usize>(put);
    return success();
}

Outcome information(const i64 handle, FileInformation& information) noexcept {
    information = FileInformation{};
    struct stat described{};
    if (::fstat(descriptor(handle), &described) != 0) return from_errno(errno);
    describe(described, information);
    return success();
}

// futimens with UTIME_OMIT for the access time (POSIX.1-2008; macOS from
// 10.13). WallTime's nanoseconds split with the remainder kept non-negative,
// so a time before 1970 is still exact.
//
// WasmFS (emscripten 6.0.13): musl's futimens is utimensat with a null
// name, and WasmFS's __syscall_utimensat (system/lib/wasmfs/syscalls.cpp)
// reads the name into a std::string_view with no null test; only an empty
// name with AT_EMPTY_PATH, which it adds itself, means the descriptor
// (paths.cpp, getFileAt), and any flag but AT_SYMLINK_NOFOLLOW is EINVAL.
// The first web run's set_modified failed there on every call. So WasmFS
// gets utimensat(fd, "", times, 0). Both Emscripten file systems keep
// milliseconds, the nanoseconds divided down (libsyscall.js,
// __syscall_utimensat; WasmFS's timespec_to_ms); read 2026-11-15.
Outcome set_modified(const i64 handle, const WallTime modified) noexcept {
    const i64 count = modified.unix_nanoseconds();
    i64 seconds = count / 1000000000LL;
    i64 nanoseconds = count % 1000000000LL;
    if (nanoseconds < 0) {
        nanoseconds = nanoseconds + 1000000000LL;
        seconds = seconds - 1;
    }
    struct timespec times[2]{};
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(seconds);
    times[1].tv_nsec = static_cast<long>(nanoseconds);
#if defined(__EMSCRIPTEN__) && defined(STXLIB_EMSCRIPTEN_WASMFS)
    const int changed = ::utimensat(descriptor(handle), "", times, 0);
#else
    const int changed = ::futimens(descriptor(handle), times);
#endif
    if (changed != 0) return from_errno(errno);
    return success();
}

Outcome set_size(const i64 handle, const u64 bytes) noexcept {
    int result = ::ftruncate(descriptor(handle), static_cast<off_t>(bytes));
    while (result != 0 && errno == EINTR) result = ::ftruncate(descriptor(handle), static_cast<off_t>(bytes));
    if (result != 0) return from_errno(errno);
    return success();
}

// The permission bits only, not setuid, setgid or sticky: the replacement
// is owned by the writer, not by the replaced file's owner, so carrying
// setuid would make a set-user-ID file of the writer's that nobody chose.
// A target that is missing (or a path through a missing directory) has
// nothing to carry; one that is not a regular file is left to the rename to
// refuse or replace.
Outcome carry_permissions(const i64 handle, const char* const target) noexcept {
    struct stat existing{};
    if (::stat(target, &existing) != 0) {
        const int error = errno;
        if (error == ENOENT || error == ENOTDIR) return success();
        return from_errno(error);
    }
    if (!S_ISREG(existing.st_mode)) return success();
    if (::fchmod(descriptor(handle), static_cast<mode_t>(existing.st_mode & 0777)) != 0) return from_errno(errno);
    return success();
}

Outcome inspect(const char* const name, const LinkMode links, FileInformation& information) noexcept {
    information = FileInformation{};
    struct stat described{};
    const int found = links == LinkMode::follow ? ::stat(name, &described) : ::lstat(name, &described);
    if (found != 0) return from_errno(errno);
    describe(described, information);
    return success();
}

Outcome identify(const char* const name, FileIdentity& identity) noexcept {
    identity = FileIdentity{};
    struct stat described{};
    if (::stat(name, &described) != 0) return from_errno(errno);
    identity.volume = static_cast<u64>(described.st_dev);
    identity.file = static_cast<u64>(described.st_ino);
    return success();
}

// The lock file holds no content; 0600 keeps another account from taking
// the lock and stalling the owner's programs.
Outcome open_lock(const char* const name, i64& handle) noexcept {
    handle = no_handle;
    const int fd = open_retrying(name, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return from_errno(errno);
    handle = static_cast<i64>(fd);
    return success();
}

// flock (4.4BSD; Linux, macOS and FreeBSD, read 2026-11-22): the lock
// belongs to the open file description and ends when its last descriptor
// closes, so a crash releases it. Emscripten's flock succeeds without
// locking (one process per page), which is all a page needs.
Outcome try_lock(const i64 handle, bool& acquired) noexcept {
    acquired = false;
    int locked = ::flock(descriptor(handle), LOCK_EX | LOCK_NB);
    while (locked != 0 && errno == EINTR) locked = ::flock(descriptor(handle), LOCK_EX | LOCK_NB);
    if (locked == 0) {
        acquired = true;
        return success();
    }
    const int error = errno;
    if (error == EWOULDBLOCK || error == EAGAIN) return success();
    return from_errno(error);
}

Outcome replace(const char* const from, const char* const to, const Durability level) noexcept {
#if defined(__EMSCRIPTEN__)
    if (level == Durability::durable && !web_backend::rename_can_be_durable(to)) return from_errno(ENOTSUP);
#endif
    if (::rename(from, to) != 0) return from_errno(errno);
    if (level != Durability::durable) return success();
    // Make the rename itself durable by flushing the directory that holds
    // `to`. Filesystems that cannot flush a directory report EINVAL or
    // ENOTSUP; the rename has still happened, so those are not failures.
    Growable<char> directory;
    const char* const slash = std::strrchr(to, '/');
    if (slash == nullptr) {
        if (!directory.append(".", 1)) return no_memory();
    } else if (slash == to) {
        if (!directory.append("/", 1)) return no_memory();
    } else if (!directory.append(to, static_cast<usize>(slash - to))) {
        return no_memory();
    }
    if (!directory.push('\0')) return no_memory();
    Descriptor fd(open_retrying(directory.data(), O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0));
    if (fd.get() < 0) return from_errno(errno);
    const int flushed = flush_descriptor(fd.get(), Durability::durable);
    const int error = errno;
    if (flushed != 0 && error != EINVAL && error != ENOTSUP) return from_errno(error);
    return success();
}

Outcome create_directory(const char* const name) noexcept {
    if (::mkdir(name, 0777) != 0) return from_errno(errno);
    return success();
}

// std::filesystem::remove's behaviour: rmdir for a directory, unlink
// otherwise (a link is removed, not what it names). POSIX allows rmdir to
// report a non-empty directory as EEXIST.
Outcome remove(const char* const name) noexcept {
    struct stat described{};
    if (::lstat(name, &described) != 0) return from_errno(errno);
    if (S_ISDIR(described.st_mode)) {
        if (::rmdir(name) == 0) return success();
        const int error = errno;
        if (error == EEXIST) {
            const Outcome result{FileStatus::directory_not_empty, ForeignCode::of(ForeignFamily::posix_errno, error)};
            return result;
        }
        return from_errno(error);
    }
    if (::unlink(name) != 0) return from_errno(errno);
    return success();
}

// ---- remove_tree (files.hpp states the safety and the bounds) ----
//
// One directory is open at a time, as a stream over its own descriptor.
// Its entries are judged and removed relative to that descriptor: a
// directory is entered with openat(O_NOFOLLOW | O_DIRECTORY), so a link put
// in its place is refused (ELOOP on Linux and macOS; EMLINK on FreeBSD,
// lib/libsys/open.2:666-668 and :852-854 at the pin) and removed as a link;
// anything else is removed with unlinkat. A directory is entered as soon as
// one is met; once it is empty, its parent is reopened through ".." and
// checked against the device and inode recorded on the way down, the
// directory is removed from it (if its name still names it), and the
// parent's entries are read again from the start. A pass that removed
// entries is followed by another (POSIX leaves unspecified whether readdir
// returns entries after a removal, POSIX.1-2017 readdir), so a directory is
// left only when one whole pass finds nothing.

namespace {

// A directory entered: its name in its parent (in the name stack, with its
// NUL) and its identity.
struct TreeLevel final {
    usize name_offset{0};
    usize name_count{0};
    dev_t device{};
    ino_t inode{};
    // Entries removed from this directory so far (entries_limit bounds it).
    usize removed_here{0};
};

// Counts `count` entries removed from the directory `level`; false past
// `entries_limit` (RemoveTreeLimits::entries_per_directory).
bool count_removed(TreeLevel& level, const u64 count, const usize entries_limit) noexcept {
    level.removed_here = level.removed_here + static_cast<usize>(count);
    const bool result = level.removed_here <= entries_limit;
    return result;
}

// Owns one directory stream; close() ends it early.
class TreeStream final {
public:
    explicit TreeStream(DIR* const stream) noexcept : stream_(stream) {}
    TreeStream(const TreeStream&) = delete;
    TreeStream& operator=(const TreeStream&) = delete;
    ~TreeStream() { close(); }

    [[nodiscard]] DIR* get() const noexcept { return stream_; }
    void close() noexcept {
        if (stream_ != nullptr) (void)::closedir(stream_);
        stream_ = nullptr;
    }

private:
    DIR* stream_;
};

enum class EntryKind : u8 {
    directory = 0,
    other = 1,
    gone = 2,
};

// Whether an open with O_NOFOLLOW | O_DIRECTORY met a link or a
// non-directory.
bool not_a_directory_now(const int error) noexcept {
    const bool result = error == ELOOP || error == EMLINK || error == ENOTDIR;
    return result;
}

int open_directory_at(const int parent, const char* const name, const int extra_flags) noexcept {
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | extra_flags;
    int fd = ::openat(parent, name, flags);
    while (fd < 0 && errno == EINTR) fd = ::openat(parent, name, flags);
    return fd;
}

EntryKind entry_kind(const int directory, const struct dirent& entry) noexcept {
#if defined(DT_DIR) && defined(DT_UNKNOWN)
    if (entry.d_type == DT_DIR) return EntryKind::directory;
    if (entry.d_type != DT_UNKNOWN) return EntryKind::other;
#endif
    struct stat described{};
    if (::fstatat(directory, entry.d_name, &described, AT_SYMLINK_NOFOLLOW) != 0) {
        // Anything else is reported by the removal that follows.
        return errno == ENOENT ? EntryKind::gone : EntryKind::other;
    }
    return S_ISDIR(described.st_mode) ? EntryKind::directory : EntryKind::other;
}

bool same_object(const struct stat& described, const dev_t device, const ino_t inode) noexcept {
    const bool result = described.st_dev == device && described.st_ino == inode;
    return result;
}

Outcome not_empty(const int error) noexcept {
    const Outcome result{FileStatus::directory_not_empty, ForeignCode::of(ForeignFamily::posix_errno, error)};
    return result;
}

// Removes a non-directory entry. `again` is set when the entry has become a
// directory since it was read (unlink refuses a directory with EISDIR on
// Linux, EPERM on macOS and FreeBSD), so the pass is repeated.
Outcome unlink_entry(const int directory, const char* const name, u64& removed, bool& again) noexcept {
    if (::unlinkat(directory, name, 0) == 0) {
        removed = removed + 1;
        again = true;
        return success();
    }
    const int error = errno;
    if (error == ENOENT) return success();
    if (error == EISDIR || error == EPERM) {
        struct stat described{};
        if (::fstatat(directory, name, &described, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(described.st_mode)) {
            again = true;
            return success();
        }
    }
    return from_errno(error);
}

// Removes the empty directory `level` names from `parent`, if that name
// still names it.
Outcome remove_entered(const int parent, const char* const name, const TreeLevel& level, u64& removed) noexcept {
    struct stat named{};
    if (::fstatat(parent, name, &named, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return success();
        return from_errno(errno);
    }
    // Moved away and replaced: what has the name now is not ours to remove.
    if (!same_object(named, level.device, level.inode)) return success();
    if (::unlinkat(parent, name, AT_REMOVEDIR) == 0) {
        removed = removed + 1;
        return success();
    }
    const int error = errno;
    if (error == ENOENT) return success();
    if (error == EEXIST || error == ENOTEMPTY) return not_empty(error);
    return from_errno(error);
}

} // namespace

Outcome remove_tree(const char* const name, const usize depth_limit, const usize entries_limit, u64& removed) noexcept {
    const int top = open_retrying(name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC, 0);
    if (top < 0) {
        const int error = errno;
        if (!not_a_directory_now(error)) return from_errno(error);
        // Not a directory, or a link: that one entry.
        const Outcome single = remove(name);
        if (single.ok()) removed = removed + 1;
        return single;
    }
    Growable<TreeLevel> levels;
    Growable<char> level_names;
    struct stat described{};
    if (::fstat(top, &described) != 0) {
        const int error = errno;
        (void)::close(top);
        return from_errno(error);
    }
    TreeLevel first{};
    first.device = described.st_dev;
    first.inode = described.st_ino;
    if (!levels.push(first)) {
        (void)::close(top);
        return no_memory();
    }
    int current = top;
    for (;;) {
        DIR* const opened = ::fdopendir(current);
        if (opened == nullptr) {
            const int error = errno;
            (void)::close(current);
            return from_errno(error);
        }
        TreeStream stream(opened);
        const int directory = ::dirfd(stream.get());
        int child = -1;
        bool again = false;
        for (;;) {
            errno = 0;
            const struct dirent* const entry = ::readdir(stream.get());
            if (entry == nullptr) {
                if (errno != 0) return from_errno(errno);
                if (!again) break;
                again = false;
                ::rewinddir(stream.get());
                continue;
            }
            const char* const entry_name = (*entry).d_name;
            if (std::strcmp(entry_name, ".") == 0 || std::strcmp(entry_name, "..") == 0) continue;
            const EntryKind kind = entry_kind(directory, *entry);
            if (kind == EntryKind::gone) continue;
            if (kind == EntryKind::other) {
                const u64 before = removed;
                const Outcome unlinked = unlink_entry(directory, entry_name, removed, again);
                if (!unlinked.ok()) return unlinked;
                if (!count_removed(levels[levels.count() - 1], removed - before, entries_limit)) {
                    return failure(FileStatus::too_large);
                }
                continue;
            }
            // levels holds the top and the directories below it: entering
            // one more makes the depth levels.count().
            if (levels.count() > depth_limit) return failure(FileStatus::too_large);
            child = open_directory_at(directory, entry_name, O_NOFOLLOW);
            if (child < 0) {
                const int error = errno;
                if (error == ENOENT) continue;
                if (!not_a_directory_now(error)) return from_errno(error);
                const u64 before = removed;
                const Outcome unlinked = unlink_entry(directory, entry_name, removed, again);
                if (!unlinked.ok()) return unlinked;
                if (!count_removed(levels[levels.count() - 1], removed - before, entries_limit)) {
                    return failure(FileStatus::too_large);
                }
                continue;
            }
            struct stat entered{};
            if (::fstat(child, &entered) != 0) {
                const int error = errno;
                (void)::close(child);
                return from_errno(error);
            }
            TreeLevel level{};
            level.name_offset = level_names.count();
            level.name_count = std::strlen(entry_name);
            level.device = entered.st_dev;
            level.inode = entered.st_ino;
            if (!level_names.append(entry_name, level.name_count) || !level_names.push('\0') || !levels.push(level)) {
                (void)::close(child);
                return no_memory();
            }
            break;
        }
        if (child >= 0) {
            // The parent's stream closes here; the child is read next.
            current = child;
            continue;
        }
        // The directory is empty (one whole pass found nothing).
        if (levels.count() == 1) {
            stream.close();
            if (::rmdir(name) == 0) {
                removed = removed + 1;
                return success();
            }
            const int error = errno;
            if (error == ENOENT) return success();
            if (error == EEXIST || error == ENOTEMPTY) return not_empty(error);
            return from_errno(error);
        }
        const int parent = open_directory_at(directory, "..", 0);
        if (parent < 0) return from_errno(errno);
        const TreeLevel level = levels[levels.count() - 1];
        const TreeLevel& above = levels[levels.count() - 2];
        struct stat reopened{};
        if (::fstat(parent, &reopened) != 0 || !same_object(reopened, above.device, above.inode)) {
            (void)::close(parent);
            return failure(FileStatus::busy);
        }
        stream.close();
        const u64 before = removed;
        const Outcome gone = remove_entered(parent, level_names.data() + level.name_offset, level, removed);
        if (!gone.ok()) {
            (void)::close(parent);
            return gone;
        }
        level_names.shrink(level.name_offset);
        levels.shrink(levels.count() - 1);
        if (!count_removed(levels[levels.count() - 1], removed - before, entries_limit)) {
            (void)::close(parent);
            return failure(FileStatus::too_large);
        }
        current = parent;
    }
}

Outcome rename(const char* const from, const char* const to) noexcept {
    if (::rename(from, to) != 0) return from_errno(errno);
    return success();
}

// Copies content and permission bits (the ordinary ones, or with the setuid,
// setgid and sticky bits when asked; files.hpp). Refuses to copy a file onto
// itself, which O_TRUNC would empty (std::filesystem::copy_file reports it
// too).
Outcome copy_file(const char* const from, const char* const to, const CopyMode mode,
                  const CopyPermissions carried) noexcept {
    Descriptor source(open_retrying(from, O_RDONLY | O_CLOEXEC, 0));
    if (source.get() < 0) return from_errno(errno);
    struct stat described{};
    if (::fstat(source.get(), &described) != 0) return from_errno(errno);
    if (S_ISDIR(described.st_mode)) return failure(FileStatus::is_a_directory);
    if (!S_ISREG(described.st_mode)) return failure(FileStatus::unsupported);
    struct stat existing{};
    if (::stat(to, &existing) == 0) {
        if (existing.st_dev == described.st_dev && existing.st_ino == described.st_ino) {
            return failure(FileStatus::already_exists);
        }
        if (mode == CopyMode::fail_if_exists) return failure(FileStatus::already_exists);
    }
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (mode == CopyMode::replace ? O_TRUNC : O_EXCL);
    const mode_t kept_bits = static_cast<mode_t>(carried == CopyPermissions::with_special_bits ? 07777 : 0777);
    const mode_t permissions = static_cast<mode_t>(described.st_mode & kept_bits);
    Descriptor target(open_retrying(to, flags, permissions));
    if (target.get() < 0) return from_errno(errno);
    // An existing target keeps its old permission bits through O_TRUNC, so
    // they are narrowed before the first byte is written, not after the last
    // (A-stxlib-hosted-01-F8): a reader allowed by the old bits can open it
    // only while it is still empty. The descriptor was opened for writing,
    // so it stays writable whatever the new bits say.
    if (::fchmod(target.get(), permissions) != 0) return from_errno(errno);
    Byte chunk[16384]{};
    for (;;) {
        usize got = 0;
        const Outcome taken = read(static_cast<i64>(source.get()), chunk, sizeof chunk, got);
        if (!taken.ok()) return taken;
        if (got == 0) break;
        usize written = 0;
        while (written < got) {
            usize put = 0;
            const Outcome given = write(static_cast<i64>(target.get()), chunk + written, got - written, put);
            if (!given.ok()) return given;
            written = written + put;
        }
    }
    const int closed = target.close_now();
    if (closed != 0) return from_errno(closed);
    return success();
}

Outcome list(const char* const name, const usize limit, Growable<char>& names, Growable<RawEntry>& entries) noexcept {
    DirectoryStream stream(::opendir(name));
    if (stream.get() == nullptr) return from_errno(errno);
    usize seen = 0;
    for (;;) {
        errno = 0;
        const struct dirent* const entry = ::readdir(stream.get());
        if (entry == nullptr) {
            if (errno != 0) return from_errno(errno);
            break;
        }
        const char* const entry_name = (*entry).d_name;
        if (std::strcmp(entry_name, ".") == 0 || std::strcmp(entry_name, "..") == 0) continue;
        if (seen == limit) return failure(FileStatus::too_large);
        seen = seen + 1;
        FileKind kind = FileKind::other;
        bool known = true;
        switch ((*entry).d_type) {
        case DT_REG:
            kind = FileKind::regular;
            break;
        case DT_DIR:
            kind = FileKind::directory;
            break;
        case DT_LNK:
            kind = FileKind::symbolic_link;
            break;
        case DT_UNKNOWN:
            known = false;
            break;
        default:
            break;
        }
        if (!known) {
            // Filesystems that do not fill d_type: ask without following.
            struct stat described{};
            if (::fstatat(::dirfd(stream.get()), entry_name, &described, AT_SYMLINK_NOFOLLOW) == 0) {
                kind = kind_of_mode(described.st_mode);
            }
        }
        const usize count = std::strlen(entry_name);
        const RawEntry raw{names.count(), count, kind};
        if (!names.append(entry_name, count) || !names.push('\0') || !entries.push(raw)) return no_memory();
    }
    return success();
}

// std::filesystem::temp_directory_path's sources on POSIX hosts (libc++ and
// libstdc++ read the same variables in the same order).
Outcome temporary_directory(Growable<char>& text) noexcept {
    const TextView variables[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
    for (const TextView variable : variables) {
        OperationResult<Utf8String> value = read_variable(variable, maximum_environment_bytes);
        if (value.failed()) {
            const ErrorKind kind = (*value.error()).kind();
            if (kind == ErrorKind::not_found) continue;
            if (kind == ErrorKind::out_of_memory) return no_memory();
            return failure(FileStatus::invalid_name);
        }
        const TextView found = (*value.value()).view();
        if (found.empty()) continue;
        text.clear();
        if (!text.append(found.data(), found.count())) return no_memory();
        return success();
    }
    const Outcome result = copy_text("/tmp", text);
    return result;
}

Outcome working_directory(Growable<char>& text) noexcept {
    text.clear();
    usize capacity = 256;
    for (;;) {
        if (!text.resize(capacity)) return no_memory();
        if (::getcwd(text.data(), capacity) != nullptr) break;
        if (errno != ERANGE) return from_errno(errno);
        if (capacity >= 65536) return failure(FileStatus::name_too_long);
        capacity = capacity * 2;
    }
    text.shrink(std::strlen(text.data()));
    return success();
}

Outcome resolve(const char* const name, Growable<char>& text) noexcept {
    // POSIX.1-2008: a null buffer makes realpath allocate the result with
    // malloc.
    char* const resolved = ::realpath(name, nullptr);
    if (resolved == nullptr) return from_errno(errno);
    const Outcome result = copy_text(resolved, text);
    std::free(resolved);
    return result;
}

Outcome read_link(const char* const name, Growable<char>& text) noexcept {
    text.clear();
    usize capacity = 256;
    for (;;) {
        if (!text.resize(capacity)) return no_memory();
        const ssize_t length = ::readlink(name, text.data(), capacity);
        if (length < 0) return from_errno(errno);
        if (static_cast<usize>(length) < capacity) {
            text.shrink(static_cast<usize>(length));
            return success();
        }
        if (capacity >= 65536) return failure(FileStatus::name_too_long);
        capacity = capacity * 2;
    }
}

Outcome create_link(const char* const target, const char* const link, const bool to_directory) noexcept {
    (void)to_directory;
    if (::symlink(target, link) != 0) return from_errno(errno);
    return success();
}

} // namespace stxlib::file_backend
