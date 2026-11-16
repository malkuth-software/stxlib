// The process module on Emscripten (hosted/process_backend.hpp; owner
// decision 2026-11-15). Facts at emscripten 6.0.13:
//   - Arguments: outside standalone mode they are passed to main and
//     nowhere else (src/lib/libwasi.js:114-145), so process_arguments is
//     unsupported (core/capabilities.cpp; core never calls arguments here).
//   - getpid answers a constant 42 (system/lib/libc/
//     emscripten_syscall_stubs.c:20): process_id answers 0 and
//     process_identity is false.
//   - The main thread: emscripten_is_main_runtime_thread
//     (system/include/emscripten/threading.h:34) names the thread that
//     hosts the runtime, which is not the thread running main under
//     PROXY_TO_PTHREAD (src/settings.js), GUI.Forms' configuration; nothing
//     names main's thread, so main_thread is false (core/capabilities.cpp)
//     and is_main_thread answers unsupported, as on Windows.
//   - Memory: the wasm memory's size is the resident size, all of it the
//     page's (emscripten_get_heap_size, system/include/emscripten/
//     heap.h:41). It only grows (memory.grow has no inverse), so it is its
//     own peak.
//
// Early exit: emscripten_get_heap_size reads the memory's size and does not
// throw (heap.h); nothing is changed.

#include "hosted/process_backend.hpp"

#include "core/host_failure.hpp"

#include <emscripten/heap.h>

namespace stxlib::process_backend {

OperationStatus arguments(const usize limit, Growable<char>& raw) noexcept {
    static_cast<void>(limit);
    raw.clear();
    const OperationStatus result = host_failure::failed(ErrorKind::unsupported, "stxlib.process.arguments",
                                                        HostCode::none);
    return result;
}

u64 process_id() noexcept { return 0; }

OperationStatus is_main_thread(bool& initial) noexcept {
    initial = false;
    const OperationStatus result = host_failure::failed(ErrorKind::unsupported, "stxlib.threads.is_main_thread",
                                                        HostCode::none);
    return result;
}

OperationStatus memory(ProcessMemory& report) noexcept {
    const u64 size = static_cast<u64>(emscripten_get_heap_size());
    report = ProcessMemory{};
    report.resident_bytes = size;
    report.peak_resident_bytes = size;
    return OperationStatus::success();
}

} // namespace stxlib::process_backend
