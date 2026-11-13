#pragma once

// The calling thread's stack (Capability::stack_trace). Replaces GUI.Forms'
// direct backtrace, backtrace_symbols_fd and RtlCaptureStackBackTrace
// (core/failure/fail_fast.cpp at 491156eac; A0 inventory, section 2).
// A1 platform-rest.
//
// capture_stack_trace takes return addresses through the same walk the
// shadow records use (src/hosted/posix/trace_posix.cpp: the frame-pointer
// chain bounded by the thread's stack, then _Unwind_Backtrace;
// src/hosted/windows/trace_windows.cpp: RtlCaptureStackBackTrace). Frames
// compiled without frame pointers can end the walk early on Linux and
// FreeBSD (trace_posix.cpp states where frame pointers are kept).
//
// write_stack_trace writes them to standard error, one line each, with the
// image and nearest symbol describe_address (images.hpp) finds. Neither
// allocates, so a fault report can call them; neither is safe in a signal
// handler (the loader's locks, images.hpp).

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
