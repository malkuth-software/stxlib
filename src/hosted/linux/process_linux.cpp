// The process module on Linux, glibc and musl alike
// (hosted/process_backend.hpp). A1 platform-rest. Host facts (the Linux
// manual pages proc(5), gettid(2) and getrusage(2) as published;
// unconfirmed at a pin):
//   - /proc/self/cmdline holds the argument area, each argument followed by
//     a NUL (what GUI.Forms' theme_files.cpp read at 491156eac); a process
//     that rewrote the area reads what it wrote, and the final NUL can be
//     missing then (core accepts that).
//   - The initial thread's kernel thread identifier equals the process
//     identifier. SYS_gettid is used through syscall(2), which glibc and
//     musl both declare, rather than gettid, which musl gained late.
//   - /proc/self/statm's second field is the resident set in pages;
//     getrusage's ru_maxrss is the peak resident set in KiB.

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "hosted/process_backend.hpp"

#include "core/host_failure.hpp"

#include <cerrno>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace stxlib::process_backend {

namespace {

ForeignCode errno_code(const int error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    return result;
}

// Replaces `bytes` with the whole proc file, or fails once it passes
// `limit` bytes.
OperationStatus read_proc_file(const char* const name, const usize limit, Growable<char>& bytes, const Operation operation) noexcept {
    bytes.clear();
    int fd = ::open(name, O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR) fd = ::open(name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(errno));
    char chunk[4096]{};
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof chunk);
        if (got < 0) {
            if (errno == EINTR) continue;
            const int error = errno;
            static_cast<void>(::close(fd));
            return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(error));
        }
        if (got == 0) break;
        const usize count = static_cast<usize>(got);
        if (count > limit || bytes.count() > limit - count) {
            static_cast<void>(::close(fd));
            return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
        }
        if (!bytes.append(chunk, count)) {
            static_cast<void>(::close(fd));
            return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        }
    }
    static_cast<void>(::close(fd));
    return OperationStatus::success();
}

// The second decimal field of statm's text; false when there is none.
bool second_field(const Growable<char>& text, u64& value) noexcept {
    usize index = 0;
    const usize count = text.count();
    while (index < count && text[index] != ' ') index = index + 1;
    if (index == count) return false;
    index = index + 1;
    u64 parsed = 0;
    usize digits = 0;
    while (index < count && text[index] >= '0' && text[index] <= '9') {
        const u64 digit = static_cast<u64>(text[index] - '0');
        if (parsed > (u64_max - digit) / 10) return false;
        parsed = parsed * 10 + digit;
        digits = digits + 1;
        index = index + 1;
    }
    if (digits == 0) return false;
    value = parsed;
    return true;
}

} // namespace

OperationStatus arguments(const usize limit, Growable<char>& raw) noexcept {
    const OperationStatus result = read_proc_file("/proc/self/cmdline", limit, raw, "stxlib.process.arguments");
    return result;
}

u64 process_id() noexcept {
    const u64 result = static_cast<u64>(::getpid());
    return result;
}

OperationStatus is_main_thread(bool& initial) noexcept {
    const long thread = ::syscall(SYS_gettid);
    initial = thread == static_cast<long>(::getpid());
    return OperationStatus::success();
}

OperationStatus memory(ProcessMemory& report) noexcept {
    const Operation operation = "stxlib.process.memory";
    report = ProcessMemory{};
    Growable<char> statm;
    const OperationStatus read = read_proc_file("/proc/self/statm", 4096, statm, operation);
    if (read.failed()) return read;
    u64 pages = 0;
    if (!second_field(statm, pages)) return host_failure::failed(ErrorKind::io_failure, operation, HostCode::invalid_value);
    const long reported_page = ::sysconf(_SC_PAGESIZE);
    const u64 page = reported_page > 0 ? static_cast<u64>(reported_page) : 4096;
    report.resident_bytes = pages > u64_max / page ? u64_max : pages * page;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
        const u64 kibibytes = static_cast<u64>(usage.ru_maxrss);
        report.peak_resident_bytes = kibibytes > u64_max / 1024 ? u64_max : kibibytes * 1024;
    }
    return OperationStatus::success();
}

} // namespace stxlib::process_backend
