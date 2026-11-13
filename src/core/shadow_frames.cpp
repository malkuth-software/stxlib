// The shadow stack and its capture (include/stxlib/shadow.hpp; survey S1 = 1).
//
// Each function reaches the thread's state in stxlib_state (state.hpp), so
// every module's copy of this file works on one stack.
//
// The capture rule, at a frame's exit: a frame its wrapper did not mark
// closed is being left by an unwind; if no current capture is pending
// (pending_is_current), it is the innermost frame the unwind has left, and
// it captures the stack with its own frame still on it. Outer frames of
// the same unwind see the pending capture and only pop. No query of the
// C++ runtime is made (shadow.hpp, "Detecting an unwind").
// A capture stays current until the gate attaches it, or until normal
// execution resumes, which is seen as the next frame opened or the next
// frame closed: either makes it stale. An exception thrown from an ordinary
// handler before either happens (a translated exception) keeps the capture
// of the one it translates, which is the throw site that matters.

#include "stxlib/shadow.hpp"

#include "core/shadow_capture.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include "stxlib/require.hpp"

namespace stxlib {

namespace shadow_capture {

ShadowReference capture(state::ThreadState& thread, const CapturePoint point) noexcept {
    if (thread.last_shadow_sequence == u64_max) return ShadowReference{};
    thread.last_shadow_sequence = thread.last_shadow_sequence + 1;
    const usize slot = thread.shadow_count == 0 ? 0 : (thread.shadow_newest + 1) % shadow_record_capacity;
    state::ShadowRecordData& record = thread.shadows[slot];
    record.sequence = thread.last_shadow_sequence;
    record.point = point;
    const usize depth = static_cast<usize>(thread.depth);
    const usize stored = depth < shadow_frame_capacity ? depth : shadow_frame_capacity;
    for (usize index = 0; index < stored; ++index) record.frames[index] = thread.frames[index];
    record.frame_count = stored;
    record.frames_lost = depth - stored;
    record.trace_count = host::capture_return_addresses(record.trace, shadow_trace_capacity);
    thread.shadow_newest = slot;
    if (thread.shadow_count < shadow_record_capacity) thread.shadow_count = thread.shadow_count + 1;
    thread.pending_shadow = record.sequence;
    thread.pending_pushes = thread.pushes;
    const ShadowReference result{record.sequence};
    return result;
}

bool pending_is_current(const state::ThreadState& thread) noexcept {
    const bool result = thread.pending_shadow != 0 && thread.pending_pushes == thread.pushes;
    return result;
}

ShadowReference take_for_handler(state::ThreadState& thread) noexcept {
    ShadowReference result{};
    if (pending_is_current(thread)) {
        result.sequence = thread.pending_shadow;
    } else {
        result = capture(thread, CapturePoint::gate);
    }
    thread.pending_shadow = 0;
    return result;
}

} // namespace shadow_capture

namespace detail {

u32 shadow_frame_enter(const TextView operation, const u64 first, const u64 second) noexcept {
    state::ThreadState& thread = state::current_thread();
    STXLIB_REQUIRE(thread.depth < u32_max, "the shadow stack depth counter is exhausted");
    STXLIB_REQUIRE(thread.pushes < u64_max, "the shadow push counter is exhausted");
    const u32 depth = thread.depth;
    if (depth < shadow_frame_capacity) {
        thread.frames[depth] = state::FrameEntry{operation.data(), operation.count(), first, second};
    }
    thread.depth = depth + 1;
    thread.pushes = thread.pushes + 1;
    return depth;
}

void shadow_frame_leave(const u32 depth, const bool closed) noexcept {
    state::ThreadState& thread = state::current_thread();
    STXLIB_REQUIRE(thread.depth == depth + 1, "a shadow frame was left out of order");
#if defined(STXLIB_SHADOW)
    if (closed) {
        // Normal execution has come back through a frame: any capture still
        // pending belongs to an exception an ordinary handler consumed.
        thread.pending_shadow = 0;
    } else if (!shadow_capture::pending_is_current(thread)) {
        static_cast<void>(shadow_capture::capture(thread, CapturePoint::throw_site));
    }
#else
    static_cast<void>(closed);
#endif
    thread.depth = depth;
}

} // namespace detail

bool shadowing_enabled() noexcept {
#if defined(STXLIB_SHADOW)
    return true;
#else
    return false;
#endif
}

usize shadow_depth() noexcept {
    const state::ThreadState& thread = state::current_thread();
    const usize result = static_cast<usize>(thread.depth);
    return result;
}

Result<ShadowSummary> read_shadow(const ShadowReference reference, const MutableView<ShadowFrameRecord> frames,
                                  const MutableView<uptr> trace) noexcept {
    const state::ThreadState& thread = state::current_thread();
    const state::ShadowRecordData* found = nullptr;
    if (reference.present()) {
        for (usize index = 0; index < thread.shadow_count; ++index) {
            if (thread.shadows[index].sequence == reference.sequence) found = &thread.shadows[index];
        }
    }
    if (found == nullptr) {
        const ErrorRecord missing = ErrorRecord::make(
            ErrorKind::not_found, "stxlib.shadow.read",
            ErrorDetail{.message = "no such shadow record on this thread (none, or overwritten)"});
        return Result<ShadowSummary>::failure(missing);
    }
    const state::ShadowRecordData& record = *found;
    const usize frame_copies = record.frame_count < frames.count() ? record.frame_count : frames.count();
    for (usize index = 0; index < frame_copies; ++index) {
        const state::FrameEntry& entry = record.frames[index];
        frames[index] = ShadowFrameRecord{detail::TextViewAccess::adopt_validated(entry.name, entry.name_count),
                                          entry.first, entry.second};
    }
    const usize trace_copies = record.trace_count < trace.count() ? record.trace_count : trace.count();
    for (usize index = 0; index < trace_copies; ++index) trace[index] = record.trace[index];
    const ShadowSummary summary{record.point, record.frame_count, record.frames_lost, record.trace_count};
    return Result<ShadowSummary>::success(summary);
}

ShadowReference park_shadow() noexcept {
#if defined(STXLIB_SHADOW)
    state::ThreadState& thread = state::current_thread();
    const ShadowReference result = shadow_capture::take_for_handler(thread);
    return result;
#else
    return ShadowReference{};
#endif
}

void restore_shadow(const ShadowReference reference) noexcept {
#if defined(STXLIB_SHADOW)
    if (!reference.present()) return;
    state::ThreadState& thread = state::current_thread();
    thread.pending_shadow = reference.sequence;
    thread.pending_pushes = thread.pushes;
#else
    static_cast<void>(reference);
#endif
}

} // namespace stxlib
