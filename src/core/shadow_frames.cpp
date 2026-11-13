// The shadow stack and its capture (include/stxlib/shadow.hpp; survey S1 = 1;
// the A2 swap-in's frame records, captured in every build).
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
#if defined(STXLIB_SHADOW)
    record.trace_count = host::capture_return_addresses(record.trace, shadow_trace_capacity);
#else
    record.trace_count = 0; // a release build takes no trace (S5 = 1)
#endif
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

namespace {

// The bytes of `text` that fit in `capacity`: all of it, or up to the last
// whole UTF-8 sequence that fits (text is valid UTF-8, so a byte that is
// not a continuation byte starts a sequence).
usize fitting_bytes(const TextView text, const usize capacity) noexcept {
    if (text.count() <= capacity) return text.count();
    usize result = capacity;
    while (result > 0 && (static_cast<u8>(text.data()[result]) & 0xc0U) == 0x80U) result = result - 1;
    return result;
}

// Copies `text` into a frame's field, zeroing the rest; true when cut.
bool copy_text(const TextView text, char* const field, const usize capacity, u32& count) noexcept {
    const usize kept = fitting_bytes(text, capacity);
    for (usize index = 0; index < capacity; ++index) field[index] = index < kept ? text.data()[index] : '\0';
    count = static_cast<u32>(kept);
    const bool result = kept < text.count();
    return result;
}

} // namespace

namespace detail {

u32 shadow_frame_enter(const FramePush& push) noexcept {
    state::ThreadState& thread = state::current_thread();
    STXLIB_REQUIRE(thread.depth < u32_max, "the shadow stack depth counter is exhausted");
    STXLIB_REQUIRE(thread.pushes < u64_max, "the shadow push counter is exhausted");
#if defined(STXLIB_SHADOW)
    STXLIB_REQUIRE(!thread.in_reporter,
                   "a fail-fast reporter opened a shadow frame (a gate or in_shadow_frame); fail_fast.hpp lists "
                   "what a reporter may call");
#endif
    const u32 depth = thread.depth;
    if (depth < shadow_frame_capacity) {
        ShadowFrameRecord& entry = thread.frames[depth];
        entry.kind = push.kind;
        entry.first = push.first;
        entry.second = push.second;
        entry.sequence = push.sequence;
        u32 flags = 0;
        if (copy_text(push.subject, entry.subject, shadow_subject_capacity, entry.subject_count)) {
            flags = flags | shadow_subject_truncated;
        }
        if (copy_text(push.name, entry.name, shadow_name_capacity, entry.name_count)) {
            flags = flags | shadow_name_truncated;
        }
        entry.flags = flags;
    } else {
        thread.dropped = thread.dropped + 1;
    }
    thread.depth = depth + 1;
    thread.pushes = thread.pushes + 1;
    return depth;
}

void shadow_frame_leave(const u32 depth, const bool closed) noexcept {
    state::ThreadState& thread = state::current_thread();
    STXLIB_REQUIRE(thread.depth == depth + 1, "a shadow frame was left out of order");
    if (closed) {
        // Normal execution has come back through a frame: any capture still
        // pending belongs to an exception an ordinary handler consumed.
        thread.pending_shadow = 0;
    } else if (!shadow_capture::pending_is_current(thread)) {
        static_cast<void>(shadow_capture::capture(thread, CapturePoint::throw_site));
    }
    thread.depth = depth;
}

} // namespace detail

ShadowFrameMark enter_shadow_frame(const ShadowFrameText& frame) noexcept {
    const ShadowFrameMark result{detail::shadow_frame_enter(detail::push_of(frame))};
    return result;
}

void leave_shadow_frame(const ShadowFrameMark mark, const bool closed) noexcept {
    detail::shadow_frame_leave(mark.depth, closed);
}

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

ShadowStackState read_shadow_stack(const MutableView<ShadowFrameRecord> frames) noexcept {
    const state::ThreadState& thread = state::current_thread();
    const usize depth = static_cast<usize>(thread.depth);
    const usize recorded = depth < shadow_frame_capacity ? depth : shadow_frame_capacity;
    const usize copies = recorded < frames.count() ? recorded : frames.count();
    for (usize index = 0; index < copies; ++index) frames[index] = thread.frames[index];
    const ShadowStackState result{.depth = depth, .recorded = recorded, .pushes = thread.pushes, .dropped = thread.dropped};
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
    for (usize index = 0; index < frame_copies; ++index) frames[index] = record.frames[index];
    const usize trace_copies = record.trace_count < trace.count() ? record.trace_count : trace.count();
    for (usize index = 0; index < trace_copies; ++index) trace[index] = record.trace[index];
    const ShadowSummary summary{record.point, record.frame_count, record.frames_lost, record.trace_count};
    return Result<ShadowSummary>::success(summary);
}

ShadowReference park_shadow() noexcept {
    state::ThreadState& thread = state::current_thread();
    const ShadowReference result = shadow_capture::take_for_handler(thread);
    return result;
}

void restore_shadow(const ShadowReference reference) noexcept {
    if (!reference.present()) return;
    state::ThreadState& thread = state::current_thread();
    thread.pending_shadow = reference.sequence;
    thread.pending_pushes = thread.pushes;
}

} // namespace stxlib
