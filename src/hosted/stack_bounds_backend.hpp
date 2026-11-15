#pragma once

// The host's answer to "where is the calling thread's stack", asked by
// stxlib_state (src/state/state.cpp, note_stack_here) on a path that is
// not a failure, and kept in the thread's anchor (state.hpp,
// ThreadFlags::stack_low and stack_high), so the frame walk
// (src/hosted/posix/trace_posix.cpp) never asks the host at failure time
// (notes decisions/2026-11-15-boundary-sideloading.md). Each family supplies
// it in posix/stack_bounds_posix.cpp or windows/stack_bounds_windows.cpp,
// compiled into stxlib_state with the anchor it fills.
//
// The query may allocate, lock and make system calls (glibc's
// pthread_getattr_np does all three; stack_bounds_posix.cpp says what each
// host does), which is why it runs only where state.cpp allows it: never
// while the thread is failing fast or running the fail-fast reporter.
// No early exit: it takes no callback, and the cancellation points it
// reaches (open, read and close on a Linux initial thread) run with POSIX
// cancellation held off (stack_bounds_posix.cpp).

#include "stxlib/types.hpp"

namespace stxlib::stack_bounds_backend {

// Writes the calling thread's stack, [low, high), and returns true; false
// when the host cannot say (Windows and Emscripten, whose traces need no
// bounds, and any host whose query fails), leaving both untouched.
[[nodiscard]] bool query(uptr& low, uptr& high) noexcept;

} // namespace stxlib::stack_bounds_backend
