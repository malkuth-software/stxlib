// Desktop integration on POSIX hosts (hosted/desktop_backend.hpp). Apple
// hosts use /usr/bin/open; other hosts use xdg-open, which can open a
// directory but not select a file in it. Ported from stx_auxlib's
// src/hosted/desktop_posix.cpp at b6e330a.
//
// current_executable: _NSGetExecutablePath and realpath on Apple hosts,
// /proc/self/exe on Linux (glibc and musl); FreeBSD (phase II) reports
// unsupported until its module lands.

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
Status run_helper(const View<TextView> arguments) noexcept {
    const ChildRequest request{
        .program = desktop_helper, .arguments = arguments, .streams = ChildStreams::discard, .detached = true};
    const Result<Child> started = Child::start(request);
    if (started.succeeded()) return Status::success();
    const ErrorRecord& failure = *started.error();
    if (failure.kind() == ErrorKind::not_found) {
        return host_failure::failed(ErrorKind::unsupported, "stxlib.desktop.helper", HostCode::none, failure.foreign());
    }
    return Status::failure(failure);
}

Status io_failure_errno(const int error) noexcept {
    const Status result = host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable",
                                               HostCode::none,
                                               ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
    return result;
}

Status exhausted() noexcept {
    const Status result =
        host_failure::failed(ErrorKind::out_of_memory, "stxlib.desktop.current_executable", HostCode::none);
    return result;
}

} // namespace

Status open_target(const TextView target, const bool is_link) noexcept {
    (void)is_link;
    const TextView arguments[] = {target};
    const Status result = run_helper(View<TextView>(arguments));
    return result;
}

Status reveal(const Path& path) noexcept {
#if defined(__APPLE__)
    const TextView arguments[] = {"-R", path.text()};
    const Status result = run_helper(View<TextView>(arguments));
    return result;
#else
    Result<Path> directory = path.parent();
    if (directory.failed()) return Status::failure(*directory.error());
    const TextView arguments[] = {(*directory.value()).text()};
    const Status result = run_helper(View<TextView>(arguments));
    return result;
#endif
}

Status executable_name(Growable<char>& text) noexcept {
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
    return Status::success();
#elif defined(__linux__)
    usize capacity = 256;
    for (;;) {
        if (!text.resize(capacity)) return exhausted();
        const ssize_t length = ::readlink("/proc/self/exe", text.data(), capacity);
        if (length < 0) return io_failure_errno(errno);
        if (static_cast<usize>(length) < capacity) {
            text.shrink(static_cast<usize>(length));
            return Status::success();
        }
        if (capacity >= 65536) {
            return host_failure::failed(ErrorKind::io_failure, "stxlib.desktop.current_executable", HostCode::too_long);
        }
        capacity = capacity * 2;
    }
#else
    return host_failure::failed(ErrorKind::unsupported, "stxlib.desktop.current_executable", HostCode::none);
#endif
}

} // namespace stxlib::desktop_backend
