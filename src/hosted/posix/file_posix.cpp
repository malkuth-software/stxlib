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
// Names are the host's bytes both ways (file_backend.hpp, "Names").

#include "hosted/file_backend.hpp"

#include "stxlib/environment.hpp"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
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
// nothing.
int flush_descriptor(const int fd, const Durability level) noexcept {
    if (level == Durability::none) return 0;
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
        if (fd_ >= 0) (void)::close(fd_);
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
        if (stream_ != nullptr) (void)::closedir(stream_);
    }

    [[nodiscard]] DIR* get() const noexcept { return stream_; }

private:
    DIR* stream_;
};

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

Outcome inspect(const char* const name, const LinkMode links, FileInformation& information) noexcept {
    information = FileInformation{};
    struct stat described{};
    const int found = links == LinkMode::follow ? ::stat(name, &described) : ::lstat(name, &described);
    if (found != 0) return from_errno(errno);
    information.kind = kind_of_mode(described.st_mode);
    if (information.kind == FileKind::regular) {
        information.size = described.st_size < 0 ? 0 : static_cast<u64>(described.st_size);
    }
#if defined(__APPLE__)
    const struct timespec modified = described.st_mtimespec;
#else
    const struct timespec modified = described.st_mtim;
#endif
    // tv_sec * 10^9 stays in i64 for every time WallTime can hold; seconds
    // beyond its range (past 2262) are clamped to its ends (stx_auxlib's).
    constexpr i64 largest_seconds = 9223372035;
    constexpr i64 smallest_seconds = -9223372035;
    i64 seconds = static_cast<i64>(modified.tv_sec);
    if (seconds > largest_seconds) seconds = largest_seconds;
    if (seconds < smallest_seconds) seconds = smallest_seconds;
    const i64 count = detail::saturating_add(seconds * 1000000000LL, static_cast<i64>(modified.tv_nsec));
    information.modified = WallTime::from_unix_nanoseconds(count);
    return success();
}

Outcome replace(const char* const from, const char* const to, const Durability level) noexcept {
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

Outcome rename(const char* const from, const char* const to) noexcept {
    if (::rename(from, to) != 0) return from_errno(errno);
    return success();
}

// Copies content and permission bits. Refuses to copy a file onto itself,
// which O_TRUNC would empty (std::filesystem::copy_file reports it too).
Outcome copy_file(const char* const from, const char* const to, const CopyMode mode) noexcept {
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
    const mode_t permissions = static_cast<mode_t>(described.st_mode & 07777);
    Descriptor target(open_retrying(to, flags, permissions));
    if (target.get() < 0) return from_errno(errno);
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
    // An existing target kept its old permission bits through O_TRUNC.
    if (::fchmod(target.get(), permissions) != 0) return from_errno(errno);
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
        Result<Utf8String> value = read_variable(variable, maximum_environment_bytes);
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
