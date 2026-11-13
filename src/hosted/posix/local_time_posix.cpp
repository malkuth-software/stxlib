// The host's zone name on POSIX hosts (hosted/local_time_backend.hpp): the
// target of the /etc/localtime link (the usual configuration of Linux
// distributions and macOS: a link into a zoneinfo directory), else the
// first line of /etc/timezone (where some Linux distributions keep the
// name), else "UTC" (POSIX leaves the default zone to the implementation;
// glibc and musl use UTC). These host conventions are stated from common
// practice, not from a cited specification.
// Replaces batch 2c's tzset and localtime_r (owner decision D12 = a): no
// host zone rules are read, so no environment lock or load state is needed.
//
// FreeBSD (at fb9033d2876d): tzsetup(8) either copies the zone file to
// /etc/localtime or links it there (usr.sbin/tzsetup/tzsetup.c:763-770),
// and in both cases writes the zone's name to /var/db/zoneinfo (_PATH_DB,
// tzsetup.c:64, written at 804-805), which takes /etc/timezone's place
// there (A1 platform-rest).

#include "hosted/local_time_backend.hpp"

#include "core/host_failure.hpp"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace stxlib::local_time_backend {

namespace {

constexpr Operation operation = "stxlib.local_time.host_zone";

#if defined(__FreeBSD__)
constexpr const char* zone_name_file = "/var/db/zoneinfo";
#else
constexpr const char* zone_name_file = "/etc/timezone";
#endif

Status io_failure_errno(const int error) noexcept {
    const Status result = host_failure::failed(ErrorKind::io_failure, operation, HostCode::none,
                                               ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error)));
    return result;
}

Status write_utc(ZoneText& text) noexcept {
    text.bytes[0] = 'U';
    text.bytes[1] = 'T';
    text.bytes[2] = 'C';
    text.count = 3;
    return Status::success();
}

// The first line of the zone-name file (/etc/timezone, or /var/db/zoneinfo
// on FreeBSD), or "UTC" when the file does not exist.
// `localtime_exists` says /etc/localtime exists but is no link: the zone is
// then a copied file with no name in it, and a missing zone-name file leaves
// it unnamed.
Status read_timezone_file(ZoneText& text, const bool localtime_exists) noexcept {
    int fd = ::open(zone_name_file, O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR) fd = ::open(zone_name_file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int error = errno;
        if (error != ENOENT) return io_failure_errno(error);
        if (localtime_exists) return host_failure::failed(ErrorKind::not_found, operation, HostCode::none);
        return write_utc(text);
    }
    // One read of up to the capacity: the name is on the first line, far
    // shorter. A longer first line is refused below.
    ZoneText line{};
    ssize_t got = ::read(fd, line.bytes, zone_text_capacity);
    while (got < 0 && errno == EINTR) got = ::read(fd, line.bytes, zone_text_capacity);
    const int read_error = errno;
    static_cast<void>(::close(fd));
    if (got < 0) return io_failure_errno(read_error);
    usize count = 0;
    const usize length = static_cast<usize>(got);
    while (count < length && line.bytes[count] != '\n' && line.bytes[count] != '\r') ++count;
    if (count == length && length == zone_text_capacity) {
        return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
    }
    while (count > 0 && (line.bytes[count - 1] == ' ' || line.bytes[count - 1] == '\t')) --count;
    for (usize index = 0; index < count; ++index) text.bytes[index] = line.bytes[index];
    text.count = count;
    return Status::success();
}

} // namespace

Status system_zone_text(ZoneText& text) noexcept {
    ZoneText link{};
    const ssize_t got = ::readlink("/etc/localtime", link.bytes, zone_text_capacity);
    if (got >= 0) {
        const usize length = static_cast<usize>(got);
        if (length == zone_text_capacity) {
            return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
        }
        for (usize index = 0; index < length; ++index) text.bytes[index] = link.bytes[index];
        text.count = length;
        return Status::success();
    }
    const int error = errno;
    // EINVAL: /etc/localtime exists and is no link (a copied zone file).
    if (error == EINVAL) return read_timezone_file(text, true);
    if (error == ENOENT || error == ENOTDIR) return read_timezone_file(text, false);
    return io_failure_errno(error);
}

} // namespace stxlib::local_time_backend
