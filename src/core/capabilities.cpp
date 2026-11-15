// The capability table (include/stxlib/capabilities.hpp). stx_auxlib
// answered capability_available in each desktop backend
// (src/hosted/desktop_<host>.cpp at b6e330a); the answers are gathered here
// for every family, FreeBSD 15.2 among them (A1 platform-rest). A desktop capability reported here can still fail at run time
// when the desktop's helper program is missing (desktop.hpp).
//
// Emscripten (owner decision 2026-11-15) has its own table below: a page
// has no terminal, no signals, no child processes, no desktop and no
// loadable images, and wasm cannot protect pages. Some answers are the
// runtime's, asked at each call (hosted/web_backend.hpp), since one build
// runs in browsers that differ. Facts at emscripten 6.0.13:
//   - page_protection: mprotect reaches __syscall_mprotect, which returns 0
//     having done nothing ("let's not and say we did",
//     system/lib/libc/emscripten_syscall_stubs.c:139-142, called from
//     musl's mprotect.c:5-13): false, so PageBlock::map refuses.
//   - process_identity: getpid answers a constant 42
//     (emscripten_syscall_stubs.c:20): false.
//   - process_arguments: outside standalone mode the arguments are passed
//     to main only (src/lib/libwasi.js:114-145): false.
//   - elapsed_clock: emscripten_get_now is performance.timeOrigin plus
//     performance.now() with threads (src/lib/libcore.js:1490-1494);
//     whether it counts time the device is suspended is not stated there
//     (unconfirmed): false, as on FreeBSD, and ElapsedTime reads the
//     interval clock.
//   - exit_tasks: atexit handlers run only when the build keeps the
//     runtime's exit (EXIT_RUNTIME, src/settings.js; STXLIB_EMSCRIPTEN_
//     EXIT_RUNTIME in CMakeLists.txt).
//   - atomic_replace: whether a rename on Emscripten's file systems is
//     seen whole by other threads is not established at the pin
//     (unconfirmed): false, as on Windows.
//   - main_thread: the runtime names the thread that hosts it
//     (emscripten_is_main_runtime_thread, system/include/emscripten/
//     threading.h:34), but with PROXY_TO_PTHREAD, which GUI.Forms' web host
//     uses, main runs on another thread (src/settings.js, PROXY_TO_PTHREAD),
//     and nothing the runtime offers names that one: false, as on Windows.
//   - known_directories: the default environment names HOME=/home/web_user
//     (libwasi.js:49-62), which the file system creates (src/lib/
//     libfs.js:1475).

#include "stxlib/capabilities.hpp"

#if defined(__EMSCRIPTEN__)
#include "hosted/web_backend.hpp"
#endif

namespace stxlib {

#if defined(__EMSCRIPTEN__)

namespace {

bool emscripten_supported(const Capability capability) noexcept {
    switch (capability) {
    case Capability::console:
    case Capability::environment:
    case Capability::interval_clock:
    case Capability::wall_clock:
    case Capability::entropy:
    case Capability::files:
    case Capability::local_time_zone:
    case Capability::memory_report:
    case Capability::wake_channel:
    case Capability::known_directories:
    case Capability::web_storage:
        return true;
    case Capability::exit_tasks:
#if defined(STXLIB_EMSCRIPTEN_EXIT_RUNTIME)
        return true;
#else
        return false;
#endif
    case Capability::threads:
    case Capability::shared_memory:
        return web_backend::shared_memory();
    case Capability::cross_origin_isolated:
        return web_backend::cross_origin_isolated();
    case Capability::fixed_thread_pool:
        return !web_backend::runs_in_node();
    case Capability::child_processes:
    case Capability::desktop_open:
    case Capability::file_reveal:
    case Capability::current_executable:
    case Capability::raw_terminal:
    case Capability::interrupt_events:
    case Capability::resize_events:
    case Capability::suspend_events:
    case Capability::atomic_replace:
    case Capability::elapsed_clock:
    case Capability::process_arguments:
    case Capability::dynamic_libraries:
    case Capability::address_lookup:
    case Capability::image_regions:
    case Capability::stack_trace:
    case Capability::page_protection:
    case Capability::process_identity:
    case Capability::main_thread:
    case Capability::display_surface:
        return false;
    }
    return false;
}

} // namespace

bool capability_supported(const Capability capability) noexcept {
    const bool result = emscripten_supported(capability);
    return result;
}

#else

bool capability_supported(const Capability capability) noexcept {
    switch (capability) {
    case Capability::console:
    case Capability::environment:
    case Capability::interval_clock:
    case Capability::wall_clock:
    case Capability::entropy:
    case Capability::exit_tasks:
    case Capability::raw_terminal:
    case Capability::interrupt_events:
        return true;
    case Capability::resize_events:
    case Capability::suspend_events:
#if defined(_WIN32)
        return false; // not console control events (events_windows.cpp)
#else
        return true;
#endif
    case Capability::child_processes:
    case Capability::desktop_open:
    case Capability::file_reveal:
    case Capability::files:
        return true;
    case Capability::elapsed_clock:
#if defined(__FreeBSD__)
        // FreeBSD has no clock that counts suspended time; ElapsedTime
        // reads uptime there (clocks_posix.cpp, A1 platform-rest).
        return false;
#else
        return true;
#endif
    case Capability::current_executable:
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__) || defined(__FreeBSD__)
        return true; // FreeBSD: KERN_PROC_PATHNAME (desktop_posix.cpp, A1 platform-rest)
#else
        return false;
#endif
    case Capability::atomic_replace:
#if defined(_WIN32)
        // MoveFileExW with MOVEFILE_REPLACE_EXISTING is not documented as
        // atomic (Microsoft's MoveFileExW documentation promises no such
        // thing), so Windows does not claim it.
        return false;
#else
        // POSIX.1-2017 rename(): if `new` names an existing file, "a link
        // named new shall remain visible to other threads throughout the
        // renaming operation and refer either to the file referred to by new
        // or old" (rename's DESCRIPTION; from the published text, not
        // re-read in this batch). macOS rename(2), read on this host: rename
        // guarantees an instance of `new` always exists, even if the system
        // crashes during the operation.
        return true;
#endif
    case Capability::threads:
    case Capability::local_time_zone:
        return true; // batch 2c; D11 b (batch 3a); D12 a (batch 3b): compiled zones, the host names one
    case Capability::main_thread:
#if defined(_WIN32)
        // Windows names no initial thread: a process's first thread is an
        // ordinary thread once created (process_windows.cpp).
        return false;
#else
        return true;
#endif
    case Capability::process_arguments:
    case Capability::memory_report:
    case Capability::dynamic_libraries:
    case Capability::address_lookup:
    case Capability::image_regions:
    case Capability::stack_trace:
    case Capability::page_protection:
    case Capability::wake_channel:
    case Capability::known_directories:
    case Capability::process_identity:
        return true; // A1 platform-rest, every family
    case Capability::shared_memory:
    case Capability::cross_origin_isolated:
        return true; // a process's threads share its memory; no other site's content is in it
    case Capability::fixed_thread_pool:
    case Capability::web_storage:
        return false; // the Emscripten family only
    case Capability::display_surface:
#if defined(__FreeBSD__)
        return true; // hosted/freebsd/surface_freebsd.cpp; open_surface still fails without /dev/fb0
#else
        return false; // hosted/surface_unsupported.cpp
#endif
    }
    return false;
}

#endif

TextView capability_name(const Capability capability) noexcept {
    switch (capability) {
    case Capability::child_processes:
        return "child_processes";
    case Capability::desktop_open:
        return "desktop_open";
    case Capability::file_reveal:
        return "file_reveal";
    case Capability::current_executable:
        return "current_executable";
    case Capability::raw_terminal:
        return "raw_terminal";
    case Capability::interrupt_events:
        return "interrupt_events";
    case Capability::resize_events:
        return "resize_events";
    case Capability::suspend_events:
        return "suspend_events";
    case Capability::console:
        return "console";
    case Capability::environment:
        return "environment";
    case Capability::interval_clock:
        return "interval_clock";
    case Capability::wall_clock:
        return "wall_clock";
    case Capability::entropy:
        return "entropy";
    case Capability::exit_tasks:
        return "exit_tasks";
    case Capability::files:
        return "files";
    case Capability::threads:
        return "threads";
    case Capability::local_time_zone:
        return "local_time_zone";
    case Capability::atomic_replace:
        return "atomic_replace";
    case Capability::elapsed_clock:
        return "elapsed_clock";
    case Capability::process_arguments:
        return "process_arguments";
    case Capability::main_thread:
        return "main_thread";
    case Capability::memory_report:
        return "memory_report";
    case Capability::dynamic_libraries:
        return "dynamic_libraries";
    case Capability::address_lookup:
        return "address_lookup";
    case Capability::image_regions:
        return "image_regions";
    case Capability::stack_trace:
        return "stack_trace";
    case Capability::page_protection:
        return "page_protection";
    case Capability::wake_channel:
        return "wake_channel";
    case Capability::known_directories:
        return "known_directories";
    case Capability::process_identity:
        return "process_identity";
    case Capability::shared_memory:
        return "shared_memory";
    case Capability::cross_origin_isolated:
        return "cross_origin_isolated";
    case Capability::fixed_thread_pool:
        return "fixed_thread_pool";
    case Capability::web_storage:
        return "web_storage";
    case Capability::display_surface:
        return "display_surface";
    }
    return "unknown";
}

} // namespace stxlib
