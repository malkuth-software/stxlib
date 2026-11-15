#pragma once

// The calling thread's stack (Capability::stack_trace). Replaces GUI.Forms'
// direct backtrace, backtrace_symbols_fd and RtlCaptureStackBackTrace
// (core/failure/fail_fast.cpp at 491156eac; A0 inventory, section 2).
// A1 platform-rest.
//
// capture_stack_trace takes return addresses by STXlib's own walk
// (src/hosted/host.hpp, capture_report_addresses): on POSIX hosts the
// frame-pointer chain, bounded by the thread's stack as STXlib recorded it
// before any failure (at stxlib_state's load for the loading thread, when
// the thread first uses STXlib's thread state, and when a Thread starts;
// src/state/state.hpp, ThreadFlags::stack_low), so the walk asks the host
// nothing and is the same whichever C library STXlib sideloads alongside
// (glibc, musl, FreeBSD's). _Unwind_Backtrace is the fallback where the
// walk cannot run: a thread with no recorded bounds (macOS asks its own
// lock-free query instead), a stack other than the recorded one, or code
// without frame pointers (src/hosted/posix/trace_posix.cpp). On Windows
// RtlCaptureStackBackTrace (src/hosted/windows/trace_windows.cpp); on
// Emscripten none.
//
// A thread that never used STXlib's thread state and is not the loading
// thread (a thread created by other code whose first STXlib call is a
// failure) has no recorded bounds: its trace comes from the unwinder,
// which may give fewer frames (on Alpine's libgcc, fewer than two).
//
// write_stack_trace writes them to standard error, one line each, with the
// image and nearest symbol describe_address (images.hpp) finds, holding
// POSIX thread cancellation off for the writes.
//
// In a fail-fast reporter (fail_fast.hpp lists them among its calls):
// - Neither allocates in STXlib's code, and neither takes an STXlib lock,
//   on any family. Below STXlib, the host's unwinder and loader do the
//   work; trace_posix.cpp and images.hpp state what each was read to do,
//   and where it can still allocate or lock (libgcc's first lookup in
//   frames registered with __register_frame_info; the loader's lock, as
//   for any dladdr).
// - Neither is async-signal-safe (the loader's locks), and neither needs to
//   be: STXlib never fails fast from a signal handler (its only handler,
//   src/hosted/posix/events_posix.cpp, records the event and returns).
// - Early exits: the only state either changes is the caller's array and
//   the stream, so an early exit at any host call leaves nothing to undo
//   (form 2); the host calls are not cancellation points except write,
//   which runs with cancellation held off.

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// The most return addresses one capture takes (the hosts' walks are
// bounded; Windows' RtlCaptureStackBackTrace takes at most 62 frames from
// an XP-era limit, unconfirmed at a pin, and STXlib's walk takes 32 there).
inline constexpr usize maximum_stack_trace_addresses = 128;

// Writes up to `addresses.count()` (at most maximum_stack_trace_addresses)
// return addresses of the calling thread, innermost first, the first being
// in the caller of capture_stack_trace or deeper (an inlined caller has no
// frame of its own). Returns how many were written: 0 where
// Capability::stack_trace is false or the host cannot walk.
[[nodiscard]] usize capture_stack_trace(MutableView<uptr> addresses) noexcept;

// Writes one line per address to standard error:
//   "  #<index> 0x<address, 16 hex digits> <image>+0x<offset>[ <symbol>+0x<offset>]\n"
// with "?" for an image the host cannot name. Control bytes in a host name
// are written as '?'. Each line is at most stack_trace_line_capacity bytes;
// longer names are cut.
void write_stack_trace(View<uptr> addresses) noexcept;

inline constexpr usize stack_trace_line_capacity = 512;

} // namespace stxlib
