#pragma once

// Web storage (Capability::web_storage): the Emscripten family's file
// volumes that outlive a page, and the close hook that makes the files a
// program wrote survive the page closing. Owner decision 2026-11-15,
// "Emscripten family and the appliance surface interface", for GUI.Forms'
// web host (claude/cp34-web-host). On every other family each call fails
// with ErrorKind::unsupported.
//
// The files themselves are ordinary files (files.hpp): a program mounts a
// volume once, then opens, writes and replaces files under it as anywhere.
// What differs is how far each Durability level reaches. Facts are cited
// at emscripten 6.0.13 (GUI.Forms' emsdk pin):
//
// | Volume | Survives a reload | File::sync, write_file honour |
// |---|---|---|
// | memory (MEMFS, the root) | no | every level, vacuously (below) |
// | indexed_db (IDBFS) | after a persist | none, ordered; durable refused |
// | origin_private (OPFS) | yes | File::sync every level; write_file durable refused |
//
// - memory: the volume ends with the runtime, so every level holds as on a
//   native tmpfs, where fsync succeeds and does nothing: after a crash
//   there is no volume to hold the old content or the new. web_storage_
//   durability(memory) is none: nothing survives a reload.
// - indexed_db: files live in memory and reach IndexedDB only when the
//   mount is synchronised (FS.syncfs, src/lib/libfs.js:614-656), which
//   IDBFS does as one readwrite transaction (src/lib/libidbfs.js:357), so
//   the store holds one whole snapshot or the previous one: ordered holds
//   with no flush. The transaction asks for no durability (libidbfs.js:357
//   passes none), so a completed persist is not known to be on the disk:
//   durable is refused with unsupported. Volumes are mounted with IDBFS's
//   autoPersist (libidbfs.js:25-46), which persists after each change;
//   persist_web_storage is the close hook that waits for one.
// - origin_private: WasmFS's OPFS backend (only with
//   STXLIB_EMSCRIPTEN_FILESYSTEM=wasmfs; OPFS requires WasmFS,
//   src/lib/libopfs.js:16-17). File::sync flushes the file's access
//   handle (system/lib/wasmfs/backends/opfs_backend.cpp:241-246,
//   src/lib/libwasmfs_opfs.js:470-475), whose algorithm is to "attempt to
//   transfer all cached modifications of the file's content to the file
//   system's underlying storage device" (File System Living Standard,
//   last updated 19 October 2026, section 2.6.5, flush()): durable for
//   content, as far as the browser's attempt reaches. WasmFS does not sync
//   directories (system/lib/wasmfs/syscalls.cpp:316-317), so write_file
//   with durable is refused before its rename, leaving the target as it
//   was; ordered is honoured (the content is flushed before the rename).
//
// Threads: restore and persist wait for the browser's IndexedDB work, which
// runs on the main browser thread's event loop, so they cannot be called
// there or on the main runtime thread; they fail with invalid_argument and
// HostCode::invalid_state. Call them from an stxlib::Thread, or from the
// main thread use persist_web_storage_on_page_hide, which does not wait.
// The browser completes the wait on the main thread's event loop, so the
// main thread must not itself block (join, lock, condition wait) on the
// thread that persists: the two would wait on each other.
// Mounting an origin_private volume also waits (WasmFS starts a worker for
// OPFS, system/include/emscripten/wasmfs.h:82-91) and is refused on the
// main browser thread the same way; that worker is a thread of the pool
// (threads.hpp, thread_pool_limit).
//
// Early exit (decisions/2026-11-15-early-exit-integrity.md): every call
// into the browser is made before any STXlib state changes or after it is
// complete; JavaScript failures come back as codes, never as exceptions.
// A wait whose completion can no longer arrive (the main runtime thread
// ended) reports failure, and its record is left allocated so a late
// completion writes into memory still owned.

#include "stxlib/files.hpp"
#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

enum class StorageKind : u8 {
    memory = 0,         // MEMFS, or WasmFS's memory backend: gone with the page
    indexed_db = 1,     // IDBFS (the JavaScript file system only)
    origin_private = 2, // OPFS (WasmFS only)
};

// Mounts a volume of `kind` at `path`, an absolute path, making the
// directories leading to it. A memory volume is an ordinary directory of
// the in-memory root. Failures: unsupported where Capability::web_storage
// is false, for a kind this build's file system cannot mount, or where the
// browser lacks it (IndexedDB or OPFS, as in Node); invalid_argument with
// HostCode::invalid_name for a relative path or one the host cannot name;
// invalid_argument with HostCode::invalid_state for origin_private on the
// main browser thread; already_exists or foreign_failure with the host's
// code when the browser refuses the mount (a volume already there).
[[nodiscard]] OperationStatus mount_web_storage(const Path& path, StorageKind kind) noexcept;

// The strongest level that survives the end of the page on a volume of
// `kind` (the table above): none, ordered or durable.
[[nodiscard]] Durability web_storage_durability(StorageKind kind) noexcept;

// Reads every indexed_db volume back from IndexedDB, replacing what memory
// holds, and waits until it is done: call once after mounting, before the
// first read. Does nothing for the other kinds (OPFS is read directly).
// Failures: unsupported where Capability::web_storage is false;
// invalid_argument with HostCode::invalid_state on the main runtime or
// browser thread; io_failure with the host's code when IndexedDB fails.
[[nodiscard]] OperationStatus restore_web_storage() noexcept;

// The close hook: writes every indexed_db volume to IndexedDB and waits
// until the transaction completes (ordered: the store holds this snapshot
// or the one before). OPFS needs no persist: its files are written in
// place, and File::sync flushes them. Failures as restore_web_storage's.
[[nodiscard]] OperationStatus persist_web_storage() noexcept;

// From any thread: asks the browser to persist every indexed_db volume
// whenever the page is hidden or unloaded (the pagehide and
// visibilitychange events). Does not wait, so a page closed during the
// write can lose it: a program that must keep its files calls
// persist_web_storage from a thread before letting the page close.
// Registering twice is harmless. Failures: unsupported where
// Capability::web_storage is false or the runtime has no page (Node).
[[nodiscard]] OperationStatus persist_web_storage_on_page_hide() noexcept;

} // namespace stxlib
