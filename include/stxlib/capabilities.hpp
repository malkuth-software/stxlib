#pragma once

// The platform layer's capabilities (the forward programme's platform
// layer; ported from stx_auxlib's include/stx_auxlib/capabilities.hpp at
// b6e330a). Each host service STXlib offers is a named capability, and
// capability_supported says whether this build on this host provides it.
// A program asks before relying on one; an operation of a capability that
// is not provided fails with ErrorKind::unsupported, never in a
// host-specific way. A provided capability can still fail for a particular
// request (a missing program, no terminal).
//
// Host families: macOS, glibc and musl (POSIX) and Windows (UCRT) in phase
// I; FreeBSD joins in phase II. A capability whose module has not been
// ported yet is reported unsupported on every host until it is (README,
// "Platform layer").
//
// Failures of the platform layer are ErrorRecords (error.hpp): the kind is
// the class of failure, the code is a HostCode below when a finer reason
// exists, and the host's own code is carried as the foreign code (errno,
// GetLastError) when there is one. stx_auxlib's CapabilityStatus maps as:
// unsupported -> unsupported; not_found -> not_found; permission_denied ->
// permission_denied; invalid_request -> invalid_argument; resources
// exhausted -> out_of_memory or limit_exceeded; busy, too_large ->
// limit_exceeded with HostCode busy or too_long; not_a_terminal ->
// unsupported with HostCode not_a_terminal; io_error -> io_failure.

#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

enum class Capability : u8 {
    // stx_auxlib's capabilities, with their values.
    child_processes = 0,
    desktop_open = 1, // opening a link or document with the desktop's default handler
    file_reveal = 2,  // showing a file selected in the desktop's file manager
    current_executable = 3,
    raw_terminal = 4,
    interrupt_events = 5, // interrupt (Ctrl+C), terminate and hangup notifications
    resize_events = 6,    // terminal resize notifications
    suspend_events = 7,   // terminal suspend (Ctrl+Z) notifications
    // Services stx_auxlib offered without a capability, named here so every
    // service reports.
    console = 8,
    environment = 9,
    interval_clock = 10,
    wall_clock = 11,
    entropy = 12,
    exit_tasks = 13,
    files = 14,
    threads = 15,
    local_time_zone = 16,
    // write_file's final rename replaces the target atomically: a reader sees
    // the old file or the new, never neither (files.hpp; owner decision D13
    // a). A host without it still replaces, without that promise.
    atomic_replace = 17,
    // The two clocks of owner decision D12 = a (clocks.hpp): interval_clock
    // (value 10, stx_auxlib's monotonic clock) stops while the machine is
    // suspended; elapsed_clock continues through suspension.
    elapsed_clock = 18,
    // One display surface with input and present (surface.hpp; owner
    // decision 2026-11-15): FreeBSD's framebuffer, terminal and evdev, for
    // the appliance's foreground target.
    display_surface = 19,
};

inline constexpr usize capability_count = 20;

// Whether this build on this host provides `capability`. A value outside
// the set is not supported.
[[nodiscard]] bool capability_supported(Capability capability) noexcept;

// A stable lower-case name ("raw_terminal"), or "unknown".
[[nodiscard]] TextView capability_name(Capability capability) noexcept;

// The finer reasons a platform-layer failure carries in ErrorRecord::code().
enum class HostCode : u32 {
    none = 0,
    not_a_terminal = 1, // the stream is redirected, not an interactive terminal (kind unsupported)
    busy = 2,           // a one-at-a-time capability is in use (kind limit_exceeded)
    too_long = 3,       // the text or content exceeds the caller's limit (kind limit_exceeded)
    stream_closed = 4,  // the stream is closed or absent (kind io_failure)
    invalid_text = 5,   // the host's text is not UTF-8 (kind invalid_argument)
    invalid_name = 6,   // a name outside the accepted set (kind invalid_argument)
    invalid_value = 7,  // a value the host cannot hold, such as one with NUL (kind invalid_argument)
    table_full = 8,     // a fixed table is full (kind limit_exceeded)
    empty_request = 9,  // a request that names nothing (kind invalid_argument)
    // Files, children and the desktop (batch 2b; files.hpp has the table).
    not_a_directory = 10,     // a directory was needed (kind invalid_argument)
    is_a_directory = 11,      // a file was needed (kind invalid_argument)
    directory_not_empty = 12, // (kind invalid_argument)
    no_space = 13,            // the volume is full (kind limit_exceeded)
    name_too_long = 14,       // the host refused a name's length (kind limit_exceeded)
    cross_device = 15,        // a rename across volumes, which cannot be atomic (kind unsupported)
    closed_handle = 16,       // the object is closed or moved from (kind disposed)
    timed_out = 17,           // the wait's time limit passed (kind limit_exceeded)
    invalid_state = 18,       // the object is not in a state that allows the call (kind invalid_argument)
};

} // namespace stxlib
