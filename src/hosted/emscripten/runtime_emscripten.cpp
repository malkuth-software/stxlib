// The Emscripten runtime's thread facts (hosted/web_backend.hpp), cited at
// emscripten 6.0.13 (tag eb132757325583b787fe9414b09afea9442b72ad in
// emsdk's emscripten-releases-tags.json; GUI.Forms' pin):
//   - emscripten_has_threading_support, emscripten_is_main_runtime_thread
//     and emscripten_is_main_browser_thread: system/include/emscripten/
//     threading.h:26, :34 and :40.
//   - The pool: with PTHREAD_POOL_SIZE_STRICT=2 a browser with no free
//     pool worker makes pthread_create fail with EAGAIN rather than start a
//     worker it would have to wait for (src/settings.js, the setting's
//     comment; src/lib/libpthread.js:563-590 and :714-716). Node ignores
//     the strict setting and starts workers on demand (libpthread.js:567-570
//     and :588), so it has no fixed limit.
//   - Without SharedArrayBuffer, pthread_create fails with EAGAIN
//     (libpthread.js:792-796).
// The pool's size is the one CMakeLists.txt links with
// (-sPTHREAD_POOL_SIZE=STXLIB_EMSCRIPTEN_POOL_SIZE, passed here as a
// definition), so a program that links its own pool size must keep the
// two equal (risk stated in the hand-in).
//
// Early exit: the two EM_JS bodies read globals inside try/catch and return
// a code; nothing in STXlib changes around them.

#include "hosted/web_backend.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"

#include <cerrno>

#include <emscripten/em_js.h>
#include <emscripten/threading.h>
#include <sys/stat.h>

#if !defined(STXLIB_EMSCRIPTEN_POOL_SIZE)
#error "CMakeLists.txt defines STXLIB_EMSCRIPTEN_POOL_SIZE for the Emscripten family"
#endif

// clang-format off
EM_JS(int, stxlib_web_runs_in_node, (), {
    try {
        if (typeof process === "object" && process !== null && typeof process.versions === "object" &&
            process.versions !== null && typeof process.versions.node === "string") return 1;
        return 0;
    } catch (error) {
        return 0;
    }
});

EM_JS(int, stxlib_web_cross_origin_isolated, (), {
    try {
        if (typeof crossOriginIsolated === "boolean") return crossOriginIsolated ? 1 : 0;
        return 0;
    } catch (error) {
        return 0;
    }
});
// clang-format on

namespace stxlib::web_backend {

bool shared_memory() noexcept {
    const bool result = emscripten_has_threading_support() != 0;
    return result;
}

bool runs_in_node() noexcept {
    const bool result = stxlib_web_runs_in_node() != 0;
    return result;
}

bool cross_origin_isolated() noexcept {
    if (runs_in_node()) return true;
    const bool result = stxlib_web_cross_origin_isolated() != 0;
    return result;
}

bool on_main_thread() noexcept {
    const bool result = emscripten_is_main_runtime_thread() != 0 || emscripten_is_main_browser_thread() != 0;
    return result;
}

u32 thread_pool_limit() noexcept {
    if (runs_in_node()) return 0;
    constexpr u32 pool = STXLIB_EMSCRIPTEN_POOL_SIZE;
    return pool;
}

namespace {

// mkdir of `prefix`, which already ends in its NUL; 0 when it was made or
// was already there.
int make_one(const char* const prefix) noexcept {
    if (::mkdir(prefix, 0777) == 0) return 0;
    const int error = errno;
    if (error == EEXIST) return 0;
    return error;
}

} // namespace

int make_directories(const char* const name) noexcept {
    Growable<char> prefix;
    usize index = 0;
    while (name[index] != '\0') {
        const char byte = name[index];
        if (byte == '/' && prefix.count() > 0 && prefix.data()[prefix.count() - 1] != '/') {
            if (!prefix.push('\0')) return ENOMEM;
            const int made = make_one(prefix.data());
            prefix.shrink(prefix.count() - 1);
            if (made != 0) return made;
        }
        if (!prefix.push(byte)) return ENOMEM;
        index = index + 1;
    }
    if (prefix.count() == 0) return ENOENT;
    if (!prefix.push('\0')) return ENOMEM;
    const int made = make_one(prefix.data());
    if (made != 0) return made;
    struct stat described{};
    if (::stat(prefix.data(), &described) != 0) return errno;
    if (!S_ISDIR(described.st_mode)) return ENOTDIR;
    return 0;
}

OperationStatus errno_failure(const Operation operation, const int error, const ErrorKind otherwise) noexcept {
    const ForeignCode code = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    ErrorKind kind = otherwise;
    HostCode host_code = HostCode::none;
    switch (error) {
    case ENOENT:
        kind = ErrorKind::not_found;
        break;
    case EEXIST:
    case EBUSY:
        kind = ErrorKind::already_exists;
        break;
    case EACCES:
    case EPERM:
        kind = ErrorKind::permission_denied;
        break;
    case ENOMEM:
        kind = ErrorKind::out_of_memory;
        break;
    case ENOTSUP:
    case ENOSYS:
        kind = ErrorKind::unsupported;
        break;
    case ENOTDIR:
        kind = ErrorKind::invalid_argument;
        host_code = HostCode::not_a_directory;
        break;
    default:
        break;
    }
    const OperationStatus result = host_failure::failed(kind, operation, host_code, code);
    return result;
}

OperationStatus main_thread_refusal(const Operation operation) noexcept {
    const OperationStatus result = host_failure::failed(ErrorKind::invalid_argument, operation,
                                                        HostCode::invalid_state);
    return result;
}

} // namespace stxlib::web_backend
