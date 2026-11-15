// Web storage over WasmFS (hosted/web_backend.hpp;
// STXLIB_EMSCRIPTEN_FILESYSTEM=wasmfs, linked with -sWASMFS): memory
// volumes in the root's memory backend and OPFS volumes. WasmFS is marked
// experimental at the pin (src/settings.js, WASMFS's comment). Facts at
// emscripten 6.0.13 (system/include/emscripten/wasmfs.h):
//   - wasmfs_get_backend_by_path and wasmfs_get_backend_by_fd (:20, :23);
//     wasmfs_create_directory mounts a backend at a new directory and
//     returns 0 or a negative errno (:32-35).
//   - wasmfs_create_opfs_backend waits for a dedicated worker and "cannot
//     be called on the browser main thread because it might deadlock"
//     (:82-91): refused there with invalid_state. The worker is one of the
//     pool's threads.
//   - File::sync reaches __wasi_fd_sync, which flushes the file's backend
//     (system/lib/wasmfs/syscalls.cpp:310-333); OPFS flushes the access
//     handle (backends/opfs_backend.cpp:241-246). Directories are not
//     synced (syscalls.cpp:316-317, a TODO), so a durable rename is refused
//     on any backend but the root's.
//   - WasmFS has no IndexedDB backend (IDBFS is the JavaScript file
//     system's, src/lib/libidbfs.js): indexed_db is unsupported here.
// Every flush is passed to fsync (FlushRule::host): on the memory backend
// that flush does nothing and succeeds (unconfirmed at the pin: the memory
// backend's flush was not read), the tmpfs rule of web.hpp.
//
// Early exit: the one EM_JS body reads globals inside try/catch. The OPFS
// mount makes the parent directories (each whole or not at all), then
// creates the backend, then mounts; a failure leaves whole directories,
// and a created backend that was not mounted (WasmFS offers no call to
// destroy one; unconfirmed whether it keeps its worker).

#include "hosted/web_backend.hpp"

#include "core/growable.hpp"

#include <cerrno>

#include <emscripten/em_js.h>
#include <emscripten/threading.h>
#include <emscripten/wasmfs.h>

#if !defined(STXLIB_EMSCRIPTEN_WASMFS)
#error "storage_wasmfs_emscripten.cpp is WasmFS's module; CMakeLists.txt defines STXLIB_EMSCRIPTEN_WASMFS with it"
#endif

// clang-format off
EM_JS(int, stxlib_web_has_opfs, (), {
    try {
        if (typeof navigator === "undefined" || !navigator || !navigator.storage) return 0;
        return (typeof navigator.storage.getDirectory === "function") ? 1 : 0;
    } catch (error) {
        return 0;
    }
});

EM_JS(int, stxlib_web_has_page, (), {
    try {
        return (typeof document !== "undefined") ? 1 : 0;
    } catch (error) {
        return 0;
    }
});
// clang-format on

namespace stxlib::web_backend {

namespace {

// The directory holding `name` with a NUL: "." for a bare name, "/" for a
// name in the root. False when memory runs out.
bool directory_of(const char* const name, Growable<char>& directory) noexcept {
    directory.clear();
    usize length = 0;
    usize last_slash = 0;
    bool has_slash = false;
    while (name[length] != '\0') {
        if (name[length] == '/') {
            last_slash = length;
            has_slash = true;
        }
        length = length + 1;
    }
    if (!has_slash) {
        if (!directory.append(".", 1)) return false;
    } else if (last_slash == 0) {
        if (!directory.append("/", 1)) return false;
    } else if (!directory.append(name, last_slash)) {
        return false;
    }
    const bool result = directory.push('\0');
    return result;
}

} // namespace

FlushRule flush_rule(const int fd) noexcept {
    static_cast<void>(fd);
    return FlushRule::host;
}

bool rename_can_be_durable(const char* const name) noexcept {
    Growable<char> directory;
    // Out of memory: the rename goes ahead and its own flush reports.
    if (!directory_of(name, directory)) return true;
    const backend_t holding = wasmfs_get_backend_by_path(directory.data());
    if (holding == nullptr) return true;
    const backend_t root = wasmfs_get_backend_by_path("/");
    const bool result = holding == root;
    return result;
}

Status mount(const char* const name, const StorageKind kind) noexcept {
    const Operation operation = "stxlib.web.mount";
    if (kind == StorageKind::indexed_db) return errno_failure(operation, ENOTSUP, ErrorKind::unsupported);
    if (kind == StorageKind::memory) {
        const int made = make_directories(name);
        if (made != 0) return errno_failure(operation, made, ErrorKind::foreign_failure);
        return Status::success();
    }
    if (emscripten_is_main_browser_thread() != 0) return main_thread_refusal(operation);
    if (stxlib_web_has_opfs() == 0) return errno_failure(operation, ENOTSUP, ErrorKind::unsupported);
    Growable<char> parent;
    if (!directory_of(name, parent)) return errno_failure(operation, ENOMEM, ErrorKind::out_of_memory);
    const int made = make_directories(parent.data());
    if (made != 0) return errno_failure(operation, made, ErrorKind::foreign_failure);
    const backend_t backend = wasmfs_create_opfs_backend();
    if (backend == nullptr) return errno_failure(operation, EIO, ErrorKind::foreign_failure);
    const int mounted = wasmfs_create_directory(name, 0777, backend);
    if (mounted < 0) return errno_failure(operation, -mounted, ErrorKind::foreign_failure);
    if (mounted > 0) return errno_failure(operation, EIO, ErrorKind::foreign_failure);
    return Status::success();
}

Status restore() noexcept {
    if (on_main_thread()) return main_thread_refusal("stxlib.web.restore");
    return Status::success();
}

Status persist() noexcept {
    if (on_main_thread()) return main_thread_refusal("stxlib.web.persist");
    return Status::success();
}

Status persist_on_page_hide() noexcept {
    if (stxlib_web_has_page() == 0) {
        return errno_failure("stxlib.web.persist_on_page_hide", ENOTSUP, ErrorKind::unsupported);
    }
    return Status::success();
}

} // namespace stxlib::web_backend
