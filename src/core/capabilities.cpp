// The capability table (include/stxlib/capabilities.hpp). stx_auxlib
// answered capability_available in each desktop backend
// (src/hosted/desktop_<host>.cpp at b6e330a); the answers are gathered here
// for every family, with the ones whose modules are not ported yet
// (threads, the local time zone) reported unsupported until their batch
// lands. A desktop capability reported here can still fail at run time
// when the desktop's helper program is missing (desktop.hpp).

#include "stxlib/capabilities.hpp"

namespace stxlib {

bool capability_supported(const Capability capability) noexcept {
    switch (capability) {
    case Capability::console:
    case Capability::environment:
    case Capability::interval_clock:
    case Capability::elapsed_clock:
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
    case Capability::current_executable:
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
        return true;
#else
        return false; // FreeBSD joins in phase II (desktop_posix.cpp)
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
        return true; // batch 2c; D11 b (batch 3a); D12 a (batch 3b): compiled zones, the host names one    case Capability::display_surface:
#if defined(__FreeBSD__)
        return true; // hosted/freebsd/surface_freebsd.cpp; open_surface still fails without /dev/fb0
#else
        return false; // hosted/surface_unsupported.cpp
#endif
    }
    return false;
}

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
    case Capability::display_surface:
        return "display_surface";
    }
    return "unknown";
}

} // namespace stxlib
