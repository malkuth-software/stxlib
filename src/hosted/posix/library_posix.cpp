// Libraries and address lookup on POSIX hosts (hosted/image_backend.hpp),
// over dlopen, dlsym, dlclose, dlerror and dladdr, shared by macOS, glibc,
// musl and FreeBSD. A1 platform-rest.
//
// FreeBSD at the pin (fb9033d2876d): the entry points are rtld's
// (libexec/rtld-elf/rtld.c: dlclose 3887, dlerror 3934, dlopen 3964 with
// RTLD_NOLOAD handled at 3997, dlsym 4322, dladdr 4374), reached through
// libc's weak stubs (lib/libc/gen/dlfcn.c:65-149); dladdr fills dli_fname
// with the object's path and dli_fbase with its map base, and the nearest
// dynamic symbol at or below the address (rtld.c:4390-4427); RTLD_NOW,
// RTLD_LOCAL and RTLD_NOLOAD are include/dlfcn.h:42, 45, 48. glibc, musl
// and macOS: the same POSIX calls, with RTLD_NOLOAD as an extension each
// provides (unconfirmed at a pin). No -ldl is needed on any of them with
// the C libraries STXlib supports (glibc 2.34 merged libdl into libc;
// unconfirmed at a pin).
//
// Ownership: a handle is dlopen's; dlclose gives it back. dladdr's names
// point into the loader's memory, valid while the image stays loaded; they
// are copied into the caller's rooms at once and never freed here.

// glibc declares Dl_info and dladdr only with _GNU_SOURCE (dlfcn.h; Clang
// and GCC predefine it for C++ on Linux, so this is a guard).
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "hosted/image_backend.hpp"

#include "core/host_failure.hpp"
#include "stxlib/text.hpp"

#include <cstring>

#include <dlfcn.h>

namespace stxlib::image_backend {

namespace {

// The loader's message for the last failure on this thread, in the record
// when it is UTF-8 (cut to the record's capacity).
ErrorRecord loader_failure(const ErrorKind kind, const Operation operation, const HostCode code) noexcept {
    const char* const message = ::dlerror();
    if (message != nullptr) {
        const Result<TextView> text = validate_utf8(message, std::strlen(message));
        if (text.succeeded()) {
            const ErrorRecord result =
                ErrorRecord::make(kind, operation, ErrorDetail{.code = static_cast<u32>(code), .message = *text.value()});
            return result;
        }
    }
    const ErrorRecord result = host_failure::record(kind, operation, code);
    return result;
}

void* handle_of(const uptr handle) noexcept {
    void* const result = reinterpret_cast<void*>(handle);
    return result;
}

} // namespace

Status open_library(const char* const name, const LoadMode mode, uptr& handle) noexcept {
    handle = 0;
    // Clears a stale message, so the one read after a failure is this call's.
    static_cast<void>(::dlerror());
    int flags = RTLD_NOW | RTLD_LOCAL;
    if (mode == LoadMode::loaded_only) flags = flags | RTLD_NOLOAD;
    void* const opened = ::dlopen(name, flags);
    if (opened == nullptr) {
        const Operation operation = mode == LoadMode::loaded_only ? Operation("stxlib.library.find_loaded")
                                                                  : Operation("stxlib.library.open");
        return Status::failure(loader_failure(ErrorKind::not_found, operation, HostCode::none));
    }
    handle = reinterpret_cast<uptr>(opened);
    return Status::success();
}

Status close_library(const uptr handle) noexcept {
    static_cast<void>(::dlerror());
    if (::dlclose(handle_of(handle)) != 0) {
        return Status::failure(loader_failure(ErrorKind::foreign_failure, "stxlib.library.close", HostCode::none));
    }
    return Status::success();
}

Status find_symbol(const uptr handle, const char* const name, uptr& address) noexcept {
    address = 0;
    // A symbol may legitimately be at address 0 only for an absolute symbol
    // no library of ours exports; null is reported as missing (POSIX:
    // dlsym returns null on failure, and dlerror tells the cases apart).
    static_cast<void>(::dlerror());
    void* const found = ::dlsym(handle_of(handle), name);
    if (found == nullptr) {
        return Status::failure(loader_failure(ErrorKind::not_found, "stxlib.library.symbol", HostCode::symbol_not_found));
    }
    address = reinterpret_cast<uptr>(found);
    return Status::success();
}

Status describe(const uptr address, const MutableView<char> image_name_room, const MutableView<char> symbol_name_room,
                AddressDescription& description) noexcept {
    description = AddressDescription{};
    Dl_info information{};
    // dladdr returns 0 when no loaded object holds the address.
    if (::dladdr(reinterpret_cast<const void*>(address), &information) == 0) {
        return host_failure::failed(ErrorKind::not_found, "stxlib.images.describe_address", HostCode::no_image);
    }
    description.image_base = reinterpret_cast<uptr>(information.dli_fbase);
    if (information.dli_fname != nullptr) {
        description.image_name = copy_into(information.dli_fname, std::strlen(information.dli_fname), image_name_room,
                                           description.image_name_truncated);
    }
    if (information.dli_sname != nullptr && information.dli_saddr != nullptr) {
        description.symbol_name = copy_into(information.dli_sname, std::strlen(information.dli_sname),
                                            symbol_name_room, description.symbol_name_truncated);
        description.symbol_address = reinterpret_cast<uptr>(information.dli_saddr);
    }
    return Status::success();
}

} // namespace stxlib::image_backend
