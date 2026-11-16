// Desktop integration on POSIX hosts (hosted/desktop_backend.hpp). Apple
// hosts use /usr/bin/open; other hosts use xdg-open, which can open a
// directory but not select a file in it. Ported from stx_auxlib's
// src/hosted/desktop_posix.cpp at b6e330a.
//
// current_executable: _NSGetExecutablePath and realpath on Apple hosts,
// /proc/self/exe on Linux (glibc and musl), and on FreeBSD the sysctl
// {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1} (sys/sys/sysctl.h:961, 999,
// 1040 at fb9033d2876d): the kernel resolves the text vnode's path for the
// calling process (pid -1) and returns it with its terminating NUL
// (sys/kern/kern_proc.c:2363-2390). A1 platform-rest.

#include "hosted/desktop_backend.hpp"

#include "core/host_failure.hpp"
#include "stxlib/children.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace stxlib::desktop_backend {

namespace {

#if defined(__APPLE__)
constexpr TextView desktop_helper = "/usr/bin/open";
#else
constexpr TextView desktop_helper = "xdg-open";
#endif

// Runs the helper detached with its output discarded and does not wait; the
// Child is abandoned and reaped later (children.hpp). A desktop without its
// helper program lacks the capability.
OperationStatus run_helper(const View<PathText> arguments) noexcept {
    const ChildRequest request{
        .program = desktop_helper, .arguments = arguments, .streams = ChildStreams::discard, .detached = true};
    const OperationResult<Child> started = Child::start(request);
    if (started.succeeded()) return OperationStatus::success();
    const ErrorRecord& failure = *started.error();
    if (failure.kind() == ErrorKind::not_found) {
        return host_failure::failed(ErrorKind::unsupported, "stxlib.desktop.helper", HostCode::none, failure.foreign());
    }
    return OperationStatus::failure(failure);
}

OperationStatus io_failure_errno(const int error) noexcept {
    const OperationStatus result = host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable",
                                                        HostCode::none, ForeignCode::of(ForeignFamily::posix_errno,
                                                                                         static_cast<i64>(error)));
    return result;
}

OperationStatus exhausted() noexcept {
    const OperationStatus result =
        host_failure::failed(ErrorKind::out_of_memory, "stxlib.desktop.current_executable", HostCode::none);
    return result;
}

} // namespace

OperationStatus open_target(const TextView target, const bool is_link) noexcept {
    (void)is_link;
    const PathText arguments[] = {target};
    const OperationStatus result = run_helper(View<PathText>(arguments));
    return result;
}

OperationStatus reveal(const Path& path) noexcept {
#if defined(__APPLE__)
    const PathText arguments[] = {"-R", path.text()};
    const OperationStatus result = run_helper(View<PathText>(arguments));
    return result;
#else
    OperationResult<Path> directory = path.parent();
    if (directory.failed()) return OperationStatus::failure(*directory.error());
    const PathText arguments[] = {(*directory.value()).text()};
    const OperationStatus result = run_helper(View<PathText>(arguments));
    return result;
#endif
}

OperationStatus executable_name(Growable<char>& text) noexcept {
    text.clear();
#if defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    Growable<char> raw;
    if (!raw.resize(static_cast<usize>(size) + 1)) return exhausted();
    if (_NSGetExecutablePath(raw.data(), &size) != 0) {
        return host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable", HostCode::none);
    }
    // POSIX.1-2008: a null buffer makes realpath allocate with malloc.
    char* const resolved = ::realpath(raw.data(), nullptr);
    if (resolved == nullptr) return io_failure_errno(errno);
    const bool copied = text.append(resolved, std::strlen(resolved));
    std::free(resolved);
    if (!copied) return exhausted();
    return OperationStatus::success();
#elif defined(__linux__)
    usize capacity = 256;
    for (;;) {
        if (!text.resize(capacity)) return exhausted();
        const ssize_t length = ::readlink("/proc/self/exe", text.data(), capacity);
        if (length < 0) return io_failure_errno(errno);
        if (static_cast<usize>(length) < capacity) {
            text.shrink(static_cast<usize>(length));
            return OperationStatus::success();
        }
        if (capacity >= 65536) {
            return host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable", HostCode::too_long);
        }
        capacity = capacity * 2;
    }
#elif defined(__FreeBSD__)
    int names[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    size_t size = 0;
    if (::sysctl(names, 4, nullptr, &size, nullptr, 0) != 0) return io_failure_errno(errno);
    if (size == 0 || size > 65536) {
        return host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable", HostCode::too_long);
    }
    if (!text.resize(static_cast<usize>(size))) return exhausted();
    size_t got = size;
    if (::sysctl(names, 4, text.data(), &got, nullptr, 0) != 0) {
        const int error = errno;
        text.clear();
        return io_failure_errno(error);
    }
    usize length = static_cast<usize>(got);
    // The kernel's count includes the terminating NUL.
    while (length > 0 && text[length - 1] == '\0') length = length - 1;
    text.shrink(length);
    return OperationStatus::success();
#else
    return host_failure::failed(ErrorKind::unsupported, "stxlib.desktop.current_executable", HostCode::none);
#endif
}

} // namespace stxlib::desktop_backend
