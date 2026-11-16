// The process module on Windows (hosted/process_backend.hpp). A1
// platform-rest. Host facts (Microsoft's documentation as published;
// unconfirmed at a pin):
//   - GetCommandLineW returns the process's command line, owned by the
//     process and never freed here; core splits it by the C runtime's rules
//     (core/command_line.hpp), not with CommandLineToArgvW, whose rules for
//     the program name and for quotes differ from the runtime's and which
//     allocates with LocalAlloc.
//   - GetCurrentProcessId.
//   - Windows names no initial thread: every thread is alike once created,
//     so main_thread is unsupported (capabilities.cpp).
//   - K32GetProcessMemoryInfo (PSAPI_VERSION 2: kernel32) fills
//     PROCESS_MEMORY_COUNTERS, whose WorkingSetSize and PeakWorkingSetSize
//     are bytes (what GUI.Forms' bundled_font_memory_report.cpp read on
//     Windows, 491156eac).

#if !defined(PSAPI_VERSION)
#define PSAPI_VERSION 2
#endif

#include "hosted/process_backend.hpp"

#include "core/host_failure.hpp"
#include "core/wtf8.hpp"

#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace stxlib::process_backend {

OperationStatus arguments(const usize limit, Growable<char>& raw) noexcept {
    const Operation operation = "stxlib.process.arguments";
    raw.clear();
    const wchar_t* const line = GetCommandLineW();
    if (line == nullptr) return OperationStatus::success();
    const usize units = static_cast<usize>(std::wcslen(line));
    // Every unit takes at least one byte of WTF-8, so a line of more units
    // than the limit is refused before converting.
    if (units > limit) return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
    // WTF-8, so an unpaired surrogate in an argument is carried (D13 a).
    if (!wtf8::from_utf16(line, units, raw)) {
        raw.clear();
        return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    }
    if (raw.count() > limit) {
        raw.clear();
        return host_failure::failed(ErrorKind::limit_exceeded, operation, HostCode::too_long);
    }
    return OperationStatus::success();
}

u64 process_id() noexcept {
    const u64 result = static_cast<u64>(GetCurrentProcessId());
    return result;
}

OperationStatus is_main_thread(bool& initial) noexcept {
    initial = false;
    return host_failure::failed(ErrorKind::unsupported, "stxlib.threads.is_main_thread", HostCode::none);
}

OperationStatus memory(ProcessMemory& report) noexcept {
    report = ProcessMemory{};
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof counters) == 0) {
        return host_failure::failed(ErrorKind::foreign_failure, "stxlib.process.memory", HostCode::none,
                                    ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError())));
    }
    report.resident_bytes = static_cast<u64>(counters.WorkingSetSize);
    report.peak_resident_bytes = static_cast<u64>(counters.PeakWorkingSetSize);
    return OperationStatus::success();
}

} // namespace stxlib::process_backend
