// Web storage over Emscripten's JavaScript file system
// (hosted/web_backend.hpp; STXLIB_EMSCRIPTEN_FILESYSTEM=js, the default):
// MEMFS volumes and IDBFS volumes. Facts at emscripten 6.0.13:
//   - The file system lives on the main runtime thread; file calls from
//     other threads are proxied to it, synchronously (src/lib/
//     libsyscall.js, the syscalls' proxy markings). So does every
//     JavaScript body here: MAIN_THREAD_EM_ASM_INT runs on the main runtime
//     thread and returns its value to the caller (system/include/
//     emscripten/em_asm.h:253).
//   - MEMFS is the root (src/lib/libfs.js:1584). IDBFS needs IndexedDB and
//     asserts it is present when used (src/lib/libidbfs.js:12-16), so the
//     mount checks first and refuses with unsupported where there is none
//     (Node). FS.mount (libfs.js:659) with IDBFS's autoPersist option
//     (libidbfs.js:25-46 and :65-111) persists after changes.
//   - FS.syncfs(populate, callback) synchronises every mount, completing
//     later on the main thread's event loop (libfs.js:614-656); IDBFS does
//     it in one readwrite transaction (libidbfs.js:357).
//   - The waiting thread uses emscripten_proxy_sync_with_ctx, which returns
//     once emscripten_proxy_finish is called for the task, and false if the
//     target thread exits first (system/include/emscripten/proxying.h:69-79
//     and :49-50).
//   - OPFS requires WasmFS (src/lib/libopfs.js:16-17): origin_private is
//     unsupported in this module.
//
// Early exit (decisions/2026-11-15-early-exit-integrity.md):
//   - Every JavaScript body catches every exception and returns an errno
//     (an FS.ErrnoError's own errno, else EIO passed in), so none crosses
//     into wasm.
//   - The mount makes its directories (each mkdir whole or not at all) and
//     then mounts; a failure leaves whole directories and no mount.
//   - A persist or restore waits on a PendingSync record taken from
//     stxlib::memory. The completion (stxlib_web_sync_done, called from the
//     syncfs callback, outside any catch so a trap inside it is not
//     swallowed) writes the record and finishes the task once; a second
//     call is ignored. If the wait ends without completion (the main
//     runtime thread exited, proxying.h:73-75), the record is left
//     allocated, not released, so a late callback writes into memory still
//     owned: a bounded leak in a runtime that is ending, chosen over a
//     write into released memory.

#include "hosted/web_backend.hpp"

#include "stxlib/memory.hpp"

#include <cerrno>
#include <new>

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/em_macros.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <pthread.h>

#if defined(STXLIB_EMSCRIPTEN_WASMFS)
#error "storage_js_emscripten.cpp is the JavaScript file system's module; WasmFS builds use storage_wasmfs_emscripten.cpp"
#endif

namespace {

// One persist or restore in flight. Written by the waiting thread before
// the task is queued and by the completion on the main runtime thread; the
// proxied task's queue and finish order the two (proxying.h:69-79).
struct PendingSync final {
    em_proxying_ctx* context{nullptr};
    int populate{0};
    int status{0};
    int finished{0};
};

double address_number(const void* const address) {
    const double result = static_cast<double>(reinterpret_cast<stxlib::uptr>(address));
    return result;
}

} // namespace

// The completion, called by the syncfs callback below with the record's
// address (a JavaScript number, exact below 2^53) and an errno or 0.
extern "C" __attribute__((visibility("default"), export_name("stxlib_web_sync_done"))) EMSCRIPTEN_KEEPALIVE void
stxlib_web_sync_done(const double pending, const int status) {
    PendingSync* const sync = reinterpret_cast<PendingSync*>(static_cast<stxlib::uptr>(pending));
    if (sync->finished != 0) return;
    sync->finished = 1;
    sync->status = status;
    emscripten_proxy_finish(sync->context);
}

EM_JS_DEPS(stxlib_web_storage_js, "$UTF8ToString,$FS,$IDBFS,$MEMFS");

// clang-format off
EM_JS(void, stxlib_web_start_sync, (double pending, int populate, int io_error), {
    var code = 0;
    var started = false;
    try {
        FS.syncfs(populate !== 0, function(error) {
            var result = 0;
            try {
                if (error) result = (typeof error.errno === "number") ? error.errno : io_error;
            } catch (inner) {
                result = io_error;
            }
            _stxlib_web_sync_done(pending, result);
        });
        started = true;
    } catch (error) {
        code = (error && typeof error.errno === "number") ? error.errno : io_error;
    }
    if (!started) _stxlib_web_sync_done(pending, code);
});
// clang-format on

namespace stxlib::web_backend {

namespace {

// The proxied task: runs on the main runtime thread and starts the
// synchronisation; the callback finishes the task later.
void start_on_main_thread(em_proxying_ctx* const context, void* const argument) {
    PendingSync* const sync = static_cast<PendingSync*>(argument);
    sync->context = context;
    stxlib_web_start_sync(address_number(sync), sync->populate, EIO);
}

Status synchronise(const bool populate, const Operation operation) noexcept {
    if (on_main_thread()) return main_thread_refusal(operation);
    void* const block = memory::allocate(sizeof(PendingSync), alignof(PendingSync));
    if (block == nullptr) return errno_failure(operation, ENOMEM, ErrorKind::out_of_memory);
    PendingSync* const sync = ::new (block) PendingSync;
    sync->populate = populate ? 1 : 0;
    const bool completed = emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                                          emscripten_main_runtime_thread_id(),
                                                          start_on_main_thread, sync);
    // Not completed: the record stays allocated (the header's argument).
    if (!completed) return errno_failure(operation, ECANCELED, ErrorKind::io_failure);
    const int status = sync->status;
    sync->~PendingSync();
    memory::release(block);
    if (status != 0) return errno_failure(operation, status, ErrorKind::io_failure);
    return Status::success();
}

double name_number(const char* const name) {
    const double result = address_number(name);
    return result;
}

} // namespace

FlushRule flush_rule(const int fd) noexcept {
    // clang-format off
    const int rule = MAIN_THREAD_EM_ASM_INT({
        try {
            var stream = FS.getStream($0);
            if (!stream || !stream.node || !stream.node.mount) return 2;
            var type = stream.node.mount.type;
            if (type === MEMFS) return 0;
            if (typeof IDBFS !== "undefined" && type === IDBFS) return 1;
            return 2;
        } catch (error) {
            return 2;
        }
    }, fd);
    // clang-format on
    if (rule == 0) return FlushRule::vacuous;
    if (rule == 1) return FlushRule::snapshot;
    return FlushRule::host;
}

bool rename_can_be_durable(const char* const name) noexcept {
    // clang-format off
    const int possible = MAIN_THREAD_EM_ASM_INT({
        try {
            var path = UTF8ToString($0);
            var slash = path.lastIndexOf("/");
            var directory = slash < 0 ? "." : (slash === 0 ? "/" : path.substring(0, slash));
            var node = FS.lookupPath(directory, { follow: true }).node;
            if (typeof IDBFS !== "undefined" && node.mount.type === IDBFS) return 0;
            return 1;
        } catch (error) {
            return 1;
        }
    }, name_number(name));
    // clang-format on
    return possible != 0;
}

Status mount(const char* const name, const StorageKind kind) noexcept {
    const Operation operation = "stxlib.web.mount";
    if (kind == StorageKind::origin_private) return errno_failure(operation, ENOTSUP, ErrorKind::unsupported);
    if (kind == StorageKind::indexed_db) {
        // clang-format off
        const int present = MAIN_THREAD_EM_ASM_INT({
            try {
                return (typeof indexedDB !== "undefined" && indexedDB !== null && typeof IDBFS !== "undefined") ? 1 : 0;
            } catch (error) {
                return 0;
            }
        });
        // clang-format on
        if (present == 0) return errno_failure(operation, ENOTSUP, ErrorKind::unsupported);
    }
    const int made = make_directories(name);
    if (made != 0) return errno_failure(operation, made, ErrorKind::foreign_failure);
    if (kind == StorageKind::memory) return Status::success();
    // clang-format off
    const int mounted = MAIN_THREAD_EM_ASM_INT({
        try {
            FS.mount(IDBFS, { autoPersist: true }, UTF8ToString($0));
            return 0;
        } catch (error) {
            return (error && typeof error.errno === "number") ? error.errno : $1;
        }
    }, name_number(name), EIO);
    // clang-format on
    if (mounted != 0) return errno_failure(operation, mounted, ErrorKind::foreign_failure);
    return Status::success();
}

Status restore() noexcept {
    const Status result = synchronise(true, "stxlib.web.restore");
    return result;
}

Status persist() noexcept {
    const Status result = synchronise(false, "stxlib.web.persist");
    return result;
}

Status persist_on_page_hide() noexcept {
    const Operation operation = "stxlib.web.persist_on_page_hide";
    // clang-format off
    const int registered = MAIN_THREAD_EM_ASM_INT({
        try {
            if (typeof document === "undefined" || typeof globalThis.addEventListener !== "function") return $0;
            if (Module["stxlibPersistOnHide"]) return 0;
            var persist = function() {
                try {
                    FS.syncfs(false, function(error) {});
                } catch (error) {
                }
            };
            Module["stxlibPersistOnHide"] = persist;
            globalThis.addEventListener("pagehide", persist);
            document.addEventListener("visibilitychange", function() {
                if (document.visibilityState === "hidden") persist();
            });
            return 0;
        } catch (error) {
            return $1;
        }
    }, ENOTSUP, EIO);
    // clang-format on
    if (registered != 0) return errno_failure(operation, registered, ErrorKind::foreign_failure);
    return Status::success();
}

} // namespace stxlib::web_backend
