// target_id_files.cpp: the target id ledger's file and folder removal
// (target_id_files.h).

#include "target_id_files.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <optional>

#include <fcntl.h>
#include <fts.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace stxsupervisor {

namespace {

constexpr int interrupted_max = 16;
// fts entries visited at most in one removal (a folder holds fewer).
constexpr std::size_t removal_entries_max = 10000000;

}  // namespace

LedgerRead read_target_id_ledger(stxconfig::TargetIdLedger& ledger, std::span<char> buffer) noexcept {
    ledger.clear();
    const int file = open(stxconfig::target_id_ledger_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        return errno == ENOENT ? LedgerRead::absent : LedgerRead::failed;
    }
    struct stat status{};
    if (fstat(file, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) > buffer.size()) {
        (void)close(file);
        return LedgerRead::failed;
    }
    std::size_t total = 0;
    int interrupted = 0;
    bool failed = false;
    // Each successful read adds at least one byte, up to the buffer's size.
    while (total < buffer.size()) {
        const ssize_t got = read(file, buffer.data() + total, buffer.size() - total);
        if (got > 0) {
            total += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR && interrupted < interrupted_max) {
            interrupted += 1;
            continue;
        }
        failed = got < 0;
        break;
    }
    (void)close(file);
    if (failed || !ledger.parse(std::span<const char>(buffer.data(), total))) {
        ledger.clear();
        return LedgerRead::failed;
    }
    return LedgerRead::read;
}

bool write_target_id_ledger(const stxconfig::TargetIdLedger& ledger, std::span<char> buffer) noexcept {
    const std::optional<std::size_t> length = ledger.format(buffer);
    if (!length.has_value()) {
        return false;
    }
    const int file = open(stxconfig::target_id_ledger_temporary_path,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file < 0) {
        return false;
    }
    std::size_t written = 0;
    int interrupted = 0;
    // Each successful write adds at least one byte.
    while (written < *length) {
        const ssize_t put = write(file, buffer.data() + written, *length - written);
        if (put > 0) {
            written += static_cast<std::size_t>(put);
            continue;
        }
        if (put < 0 && errno == EINTR && interrupted < interrupted_max) {
            interrupted += 1;
            continue;
        }
        break;
    }
    const bool whole = written == *length && fchmod(file, 0600) == 0 && fsync(file) == 0;
    (void)close(file);
    if (!whole || rename(stxconfig::target_id_ledger_temporary_path, stxconfig::target_id_ledger_path) != 0) {
        (void)unlink(stxconfig::target_id_ledger_temporary_path);
        return false;
    }
    const int folder = open(stxconfig::target_id_ledger_directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (folder >= 0) {
        (void)fsync(folder);
        (void)close(folder);
    }
    return true;
}

bool ensure_target_id_directory() noexcept {
    if (mkdir(stxconfig::target_id_ledger_directory, 0700) != 0 && errno != EEXIST) {
        return false;
    }
    struct stat status{};
    if (lstat(stxconfig::target_id_ledger_directory, &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != 0) {
        return false;
    }
    return (status.st_mode & 07777u) == 0700u || chmod(stxconfig::target_id_ledger_directory, 0700) == 0;
}

bool remove_target_folder(const char* folder) noexcept {
    const std::size_t parent = std::strlen(stxconfig::target_folder_parent);
    if (folder == nullptr || std::strncmp(folder, stxconfig::target_folder_parent, parent) != 0 ||
        folder[parent] != '/' || folder[parent + 1u] == '\0') {
        return false;
    }
    struct stat status{};
    if (lstat(folder, &status) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(status.st_mode)) {
        return false;
    }
    char path[stxconfig::line_length_max + 1u] = {};
    const std::size_t length = std::strlen(folder);
    if (length > stxconfig::line_length_max) {
        return false;
    }
    std::memcpy(path, folder, length);
    char* const roots[] = {path, nullptr};
    // Physical: a link is removed, never followed; one file system only.
    FTS* const walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_XDEV, nullptr);
    if (walk == nullptr) {
        return false;
    }
    bool whole = true;
    std::size_t visited = 0;
    for (FTSENT* item = fts_read(walk); item != nullptr && visited < removal_entries_max; item = fts_read(walk)) {
        visited += 1;
        switch ((*item).fts_info) {
        case FTS_D:
            break;  // removed after its contents (FTS_DP)
        case FTS_DP:
            whole = rmdir((*item).fts_accpath) == 0 && whole;
            break;
        case FTS_DNR:
        case FTS_ERR:
        case FTS_NS:
            whole = false;
            break;
        case FTS_DC:
        case FTS_DOT:
            break;
        default:
            // Files, links (FTS_SL, FTS_SLNONE) and the rest: unlinked.
            whole = unlink((*item).fts_accpath) == 0 && whole;
            break;
        }
    }
    (void)fts_close(walk);
    return whole && visited < removal_entries_max;
}

}  // namespace stxsupervisor
