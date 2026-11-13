// The process module on FreeBSD (hosted/process_backend.hpp). A1
// platform-rest. Every fact is cited at FreeBSD releng/15.2, commit
// fb9033d2876dc3ca2a2c56b68d3ce698da1a6269 (the pin, A0-freebsd-pin.md),
// read at github.com/freebsd/freebsd-src:
//   - Arguments: sysctl {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, pid}
//     (sys/sys/sysctl.h:961, 999, 1035) returns the process's argument
//     strings, each NUL-terminated: the kernel's saved copy (p_args), or,
//     when it kept none (the arguments exceeded kern.ps_arg_cache_limit),
//     the strings read from the process (proc_getargv)
//     (sys/kern/kern_proc.c:2141-2190). A pid of -1 names the caller
//     (kern_proc.c:2156-2158). A first call with no buffer measures.
//   - The initial thread: pthread_main_np (include/pthread_np.h:48) returns
//     1 on the initial thread, 0 on others, and -1 before libthr has
//     initialised (lib/libthr/thread/thr_main_np.c:37-50); before then the
//     process has no other thread, so -1 is read as the initial thread.
//   - Memory: sysctl {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid}
//     (sys/sys/sysctl.h:1029) fills struct kinfo_proc, whose ki_rssize is
//     the resident set in pages (sys/sys/user.h:152); getrusage's
//     ru_maxrss is the peak in KiB, sampled at statistics-clock ticks
//     (sys/kern/kern_clock.c:739-744: pgtok of the resident count).

#include "hosted/process_backend.hpp"

#include "core/host_failure.hpp"

#include <cerrno>

#include <pthread.h>
#include <pthread_np.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

namespace stxlib::process_backend {

namespace {

ForeignCode errno_code(const int error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::posix_errno, static_cast<i64>(error));
    return result;
}

} // namespace

Status arguments(const usize limit, Growable<char>& raw) noexcept {
    const Operation operation = "stxlib.process.arguments";
    raw.clear();
    int names[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, -1};
    // Another thread may change the process title between the measuring
    // call and the reading one; ENOMEM then asks for a larger buffer, at
    // most a few times.
    for (int attempt = 0; attempt < 4; ++attempt) {
        size_t size = 0;
        if (::sysctl(names, 4, nullptr, &size, nullptr, 0) != 0) {
            return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(errno));
        }
        if (size > limit) return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
        // Room for growth between the calls, within the limit.
        const usize room = size + 256 < limit ? size + 256 : limit;
        if (!raw.resize(room)) return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        size_t got = room;
        if (::sysctl(names, 4, raw.data(), &got, nullptr, 0) == 0) {
            raw.shrink(static_cast<usize>(got));
            return Status::success();
        }
        if (errno != ENOMEM) {
            return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(errno));
        }
    }
    return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(ENOMEM));
}

u64 process_id() noexcept {
    const u64 result = static_cast<u64>(::getpid());
    return result;
}

Status is_main_thread(bool& initial) noexcept {
    const int answer = ::pthread_main_np();
    initial = answer != 0;
    return Status::success();
}

Status memory(ProcessMemory& report) noexcept {
    const Operation operation = "stxlib.process.memory";
    report = ProcessMemory{};
    int names[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(::getpid())};
    kinfo_proc information{};
    size_t size = sizeof information;
    if (::sysctl(names, 4, &information, &size, nullptr, 0) != 0) {
        return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, errno_code(errno));
    }
    if (size < sizeof information) return host_failure::failed(ErrorKind::io_failure, operation, HostCode::invalid_value);
    const long reported_page = ::sysconf(_SC_PAGESIZE);
    const u64 page = reported_page > 0 ? static_cast<u64>(reported_page) : 4096;
    const u64 pages = information.ki_rssize > 0 ? static_cast<u64>(information.ki_rssize) : 0;
    report.resident_bytes = pages > u64_max / page ? u64_max : pages * page;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0 && usage.ru_maxrss > 0) {
        const u64 kibibytes = static_cast<u64>(usage.ru_maxrss);
        report.peak_resident_bytes = kibibytes > u64_max / 1024 ? u64_max : kibibytes * 1024;
    }
    return Status::success();
}

} // namespace stxlib::process_backend
