// The user database's home on POSIX hosts (hosted/directories_backend.hpp),
// through getpwuid_r for the real user (getuid), shared by macOS, glibc,
// musl and FreeBSD. getpwuid_r reports ERANGE when its buffer is too small
// (POSIX.1-2017); the buffer grows to at most 1 MiB. A1 platform-rest.

#include "hosted/directories_backend.hpp"

#include "core/host_failure.hpp"

#include <cerrno>
#include <cstring>

#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

namespace stxlib::directories_backend {

namespace {

constexpr Operation operation = "stxlib.files.known_directory";

} // namespace

OperationStatus user_database_home(Growable<char>& text) noexcept {
    text.clear();
    const long suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    usize capacity = suggested > 0 ? static_cast<usize>(suggested) : 1024;
    constexpr usize largest = 1048576;
    Growable<char> buffer;
    for (;;) {
        if (!buffer.resize(capacity)) return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        passwd entry{};
        passwd* found = nullptr;
        const int error = ::getpwuid_r(::getuid(), &entry, buffer.data(), capacity, &found);
        if (error == ERANGE && capacity < largest) {
            capacity = capacity * 2;
            continue;
        }
        if (error != 0) {
            return host_failure::failed(ErrorKind::not_found, operation, HostCode::no_home,
                                        ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
        }
        if (found == nullptr || entry.pw_dir == nullptr || entry.pw_dir[0] == '\0') {
            return host_failure::failed(ErrorKind::not_found, operation, HostCode::no_home);
        }
        if (!text.append(entry.pw_dir, std::strlen(entry.pw_dir))) {
            return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        }
        return OperationStatus::success();
    }
}

OperationStatus known_folder(const KnownDirectory which, Growable<char>& text) noexcept {
    static_cast<void>(which);
    text.clear();
    return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
}

} // namespace stxlib::directories_backend
