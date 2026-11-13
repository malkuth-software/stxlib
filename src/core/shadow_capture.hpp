#pragma once

// The capture of a shadow record, shared by the scopes (shadow_frames.cpp)
// and the gates (exception_shadow.cpp). Every build; the native trace is
// taken in development builds only.

#include "stxlib/error.hpp"
#include "stxlib/shadow.hpp"

#include "state/state.hpp"

namespace stxlib::shadow_capture {

// Files the thread's logical stack and a native trace as a new shadow
// record and makes it the pending capture. Returns its reference, or none
// when the thread's shadow sequence is exhausted (2^64 - 1 records).
ShadowReference capture(state::ThreadState& thread, CapturePoint point) noexcept;

// True when the pending capture belongs to the exception unwinding now: one
// was taken and no scope has been entered since (normal execution has not
// resumed).
[[nodiscard]] bool pending_is_current(const state::ThreadState& thread) noexcept;

// For a handler: the pending capture if current, else a capture at the
// handler (CapturePoint::gate); the pending capture is cleared.
ShadowReference take_for_handler(state::ThreadState& thread) noexcept;

} // namespace stxlib::shadow_capture
