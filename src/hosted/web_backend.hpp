#pragma once

// The Emscripten family's own primitives (owner decision 2026-11-15,
// "Emscripten family and the appliance surface interface"): the runtime's
// thread facts for core/capabilities.cpp and core/threads.cpp, the volume
// rules file_posix.cpp applies to Durability, and web storage for
// core/web.cpp (include/stxlib/web.hpp). Linked only in the Emscripten
// family: emscripten/runtime_emscripten.cpp, and one of
// emscripten/storage_js_emscripten.cpp (Emscripten's JavaScript file
// system: MEMFS and IDBFS) or emscripten/storage_wasmfs_emscripten.cpp
// (WasmFS: its memory backend and OPFS), chosen by
// STXLIB_EMSCRIPTEN_FILESYSTEM in CMakeLists.txt.
//
// Early exit (decisions/2026-11-15-early-exit-integrity.md). Every
// function here that reaches JavaScript does so through an EM_JS or EM_ASM
// body that catches every JavaScript exception and returns a code, so no
// JavaScript exception enters STXlib's frames (compiled without
// exceptions, they have no landing pads; a JavaScript exception crossing
// them would skip their cleanups, the "foreign exception" of
// src/preamble.js:265-267 at emscripten 6.0.13). The only calls back into
// wasm from JavaScript are the completion export of the storage module,
// which stores two words and signals a waiting thread, outside any catch.

#include "stxlib/files.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/web.hpp"

namespace stxlib::web_backend {

// ---- the runtime -------------------------------------------------------------

// Whether threads can share memory: SharedArrayBuffer is present and the
// build has threads (emscripten_has_threading_support, threading.h:26 at
// 6.0.13).
[[nodiscard]] bool shared_memory() noexcept;

// Whether the page is cross-origin isolated (globalThis.crossOriginIsolated,
// which COOP and COEP headers set; site/source/docs/porting/pthreads.rst:7
// at 6.0.13 for the headers). Node is a process, not a page: true there.
[[nodiscard]] bool cross_origin_isolated() noexcept;

// Whether the runtime is Node.js rather than a browser.
[[nodiscard]] bool runs_in_node() noexcept;

// Whether the calling thread may not block on the event loop: the main
// runtime thread or the main browser thread (threading.h:34 and :40).
[[nodiscard]] bool on_main_thread() noexcept;

// The most threads the program can have running at once, or 0 when the
// host sets no fixed limit (threads.hpp, thread_pool_limit).
[[nodiscard]] u32 thread_pool_limit() noexcept;

// ---- the volume rules --------------------------------------------------------

// What a flush of an open file can honour on the volume holding it
// (files.hpp, Durability; web.hpp has the table).
enum class FlushRule : u8 {
    // A memory volume (MEMFS, or a WasmFS memory directory in the JS layer's
    // terms): it ends with the runtime, so every level holds as on a native
    // tmpfs, where fsync succeeds and does nothing.
    vacuous = 0,
    // IDBFS: the store changes only by whole-mount snapshots, each one
    // IndexedDB transaction, so ordered holds without a flush; durable is
    // refused (the transaction's commit is not known to reach the disk).
    snapshot = 1,
    // Anything else (OPFS, WasmFS backends, Node's file systems): fsync.
    host = 2,
};

[[nodiscard]] FlushRule flush_rule(int fd) noexcept;

// Whether a rename into the directory holding `name` (a host name with a
// NUL) can be made durable: true on memory volumes (vacuously) and on
// volumes whose directories fsync flushes; false on IDBFS and on WasmFS
// backends other than the root's (WasmFS does not sync directories,
// system/lib/wasmfs/syscalls.cpp:316-317 at 6.0.13). Asked before the
// rename, so a refusal leaves the target as it was.
[[nodiscard]] bool rename_can_be_durable(const char* name) noexcept;

// ---- web storage ---------------------------------------------------------------

// Shared by the two storage modules (runtime_emscripten.cpp).
// Makes every directory along `name` (absolute, with a NUL), each one
// already present or made; 0, or the errno of the first that fails, or
// ENOTDIR when the last component exists and is no directory. Each mkdir
// either makes a whole directory or none, so a failure part way leaves
// only whole directories behind.
[[nodiscard]] int make_directories(const char* name) noexcept;
// The failure for an errno value from the file system or the browser:
// not_found, already_exists (EEXIST and EBUSY: a volume already there),
// permission_denied, out_of_memory, unsupported (ENOTSUP, ENOSYS),
// invalid_argument with HostCode::not_a_directory, else `otherwise`; the
// errno is carried as the foreign code.
[[nodiscard]] OperationStatus errno_failure(Operation operation, int error, ErrorKind otherwise) noexcept;
// invalid_argument with HostCode::invalid_state: a wait the main runtime or
// browser thread may not make.
[[nodiscard]] OperationStatus main_thread_refusal(Operation operation) noexcept;

// Mounts `kind` at `name` (an absolute host name with a NUL), making the
// directories leading to it. Failures as mount_web_storage's (web.hpp).
[[nodiscard]] OperationStatus mount(const char* name, StorageKind kind) noexcept;
[[nodiscard]] OperationStatus restore() noexcept;
[[nodiscard]] OperationStatus persist() noexcept;
[[nodiscard]] OperationStatus persist_on_page_hide() noexcept;

} // namespace stxlib::web_backend
