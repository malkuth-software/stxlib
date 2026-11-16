// target_id_files.h: the target id ledger's file, and the removal of a
// deleted target's folder (config/target_ids.h; sb2).
//
// The ledger is /data/stx/target_ids, root's only (0600, in a 0700
// folder), written as a new file, fsynced, renamed over the old one and
// the folder fsynced: the old ledger or the new, never half of one. A
// ledger that is absent is empty; one that cannot be read or parsed is
// never taken as empty (ids could be issued twice), so issuing fails
// until it is repaired.
//
// System calls: open, fstat, read, write, fsync, close, rename, mkdir,
// lstat, chmod, fts(3) (unlink, rmdir). Hosted on FreeBSD's libc.

#ifndef STXLIB_RUNTIME_SUPERVISOR_TARGET_ID_FILES_H
#define STXLIB_RUNTIME_SUPERVISOR_TARGET_ID_FILES_H

#include <cstddef>
#include <span>

#include "config/target_ids.h"

namespace stxsupervisor {

enum class LedgerRead : unsigned char {
    read,
    absent,   // no file: the ledger is empty
    failed,   // unreadable or malformed: the ledger is not to be used
};

// buffer: at least target_id_ledger_text_max bytes.
[[nodiscard]] LedgerRead read_target_id_ledger(stxconfig::TargetIdLedger& ledger, std::span<char> buffer) noexcept;
[[nodiscard]] bool write_target_id_ledger(const stxconfig::TargetIdLedger& ledger, std::span<char> buffer) noexcept;
// The ledger's folder, 0700 root, not a link (made when absent). False
// when /data holds none and none can be made (a read-only root: /data not
// mounted).
[[nodiscard]] bool ensure_target_id_directory() noexcept;
// Removes folder and everything below it without following links and
// without leaving its file system; folder must lie below
// target_folder_parent. True when it is gone (or was absent).
[[nodiscard]] bool remove_target_folder(const char* folder) noexcept;

}  // namespace stxsupervisor

#endif  // STXLIB_RUNTIME_SUPERVISOR_TARGET_ID_FILES_H
