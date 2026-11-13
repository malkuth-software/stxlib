// The process module on macOS (hosted/process_backend.hpp). A1
// platform-rest. Host facts (Apple's headers and manual pages as published;
// unconfirmed at a pin):
//   - _NSGetArgc and _NSGetArgv (<crt_externs.h>) give the C runtime's
//     argc and argv, which GUI.Forms' theme_files.cpp read (491156eac); the
//     strings belong to the runtime and are only read here.
//   - pthread_main_np returns non-zero on the initial thread.
//   - task_info with MACH_TASK_BASIC_INFO fills mach_task_basic_info, whose
//     resident_size and resident_size_max are bytes (what GUI.Forms'
//     bundled_font_memory_report.cpp read).

#include "hosted/process_backend.hpp"

#include "core/host_failure.hpp"

#include <cstring>

#include <crt_externs.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

namespace stxlib::process_backend {

Status arguments(const usize limit, Growable<char>& raw) noexcept {
    const Operation operation = "stxlib.process.arguments";
    raw.clear();
    const int count = *::_NSGetArgc();
    char** const vector = *::_NSGetArgv();
    if (vector == nullptr || count <= 0) return Status::success();
    // Measured first, so an over-limit set is refused before copying.
    usize total = 0;
    for (int index = 0; index < count && vector[index] != nullptr; ++index) {
        const usize length = std::strlen(vector[index]) + 1;
        if (length > limit || total > limit - length) {
            return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
        }
        total = total + length;
    }
    if (!raw.reserve(total)) return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    for (int index = 0; index < count && vector[index] != nullptr; ++index) {
        const usize length = std::strlen(vector[index]);
        if (!raw.append(vector[index], length) || !raw.push('\0')) {
            return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        }
    }
    return Status::success();
}

u64 process_id() noexcept {
    const u64 result = static_cast<u64>(::getpid());
    return result;
}

Status is_main_thread(bool& initial) noexcept {
    initial = ::pthread_main_np() != 0;
    return Status::success();
}

Status memory(ProcessMemory& report) noexcept {
    report = ProcessMemory{};
    mach_task_basic_info_data_t information{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    const kern_return_t status = ::task_info(::mach_task_self(), MACH_TASK_BASIC_INFO,
                                             reinterpret_cast<task_info_t>(&information), &count);
    if (status != KERN_SUCCESS) {
        // Mach's kern_return_t has no ForeignFamily (error.hpp), so the
        // value is not carried; error.hpp is sb1's to extend.
        return host_failure::failed(ErrorKind::foreign_failure, "stxlib.process.memory", HostCode::none);
    }
    report.resident_bytes = static_cast<u64>(information.resident_size);
    report.peak_resident_bytes = static_cast<u64>(information.resident_size_max);
    return Status::success();
}

} // namespace stxlib::process_backend
