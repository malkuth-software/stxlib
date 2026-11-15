// The shadow stack and its capture (include/stxlib/shadow.hpp; survey S1 = 1;
// the A2 swap-in's frame records, captured in every build).
//
// Each function reaches the thread's state in stxlib_state (state.hpp), so
// every module's copy of this file works on one stack: the block's store,
// or with no block the anchor's failure reserve (8 frames and one untraced
// record; state.hpp).
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

// Each operation below is written once over a store (state.hpp: the
// block's FullShadow or the anchor's ReserveShadow) and dispatched by
// the public function, which picks the thread's store.

template <typename Store>
u32 enter_on(Store& thread, const detail::FramePush& push) noexcept {
    STXLIB_REQUIRE(thread.depth < u32_max, "the shadow stack depth counter is exhausted");
    STXLIB_REQUIRE(thread.pushes < u64_max, "the shadow push counter is exhausted");
    const u32 depth = thread.depth;
    if (depth < Store::frame_capacity) {
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

template <typename Store>
void leave_on(Store& thread, const u32 depth, const bool closed) noexcept {
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

template <typename Store>
ShadowStackState read_stack_of(const Store& thread, const MutableView<ShadowFrameRecord> frames) noexcept {
    const usize depth = static_cast<usize>(thread.depth);
    const usize recorded = depth < Store::frame_capacity ? depth : Store::frame_capacity;
    const usize copies = recorded < frames.count() ? recorded : frames.count();
    for (usize index = 0; index < copies; ++index) frames[index] = thread.frames[index];
    const ShadowStackState result{.depth = depth, .recorded = recorded, .pushes = thread.pushes, .dropped = thread.dropped};
    return result;
}

template <typename Store>
Result<ShadowSummary> read_record_of(const Store& thread, const ShadowReference reference,
                                     const MutableView<ShadowFrameRecord> frames, const MutableView<uptr> trace) noexcept {
    const typename Store::Record* found = nullptr;
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
    const typename Store::Record& record = *found;
    const usize frame_copies = record.frame_count < frames.count() ? record.frame_count : frames.count();
    for (usize index = 0; index < frame_copies; ++index) frames[index] = record.frames[index];
    if constexpr (Store::trace_capacity > 0) {
        const usize trace_copies = record.trace_count < trace.count() ? record.trace_count : trace.count();
        for (usize index = 0; index < trace_copies; ++index) trace[index] = record.trace[index];
    } else {
        static_cast<void>(trace); // a release record, or the reserve's, has no trace (layouts 9, 10)
    }
    const ShadowSummary summary{record.point, record.frame_count, record.frames_lost, record.trace_count};
    return Result<ShadowSummary>::success(summary);
}

template <typename Store>
void restore_on(Store& thread, const ShadowReference reference) noexcept {
    thread.pending_shadow = reference.sequence;
    thread.pending_pushes = thread.pushes;
}

} // namespace

namespace detail {

u32 shadow_frame_enter(const FramePush& push) noexcept {
#if defined(STXLIB_SHADOW)
    STXLIB_REQUIRE(!state::current_flags().in_reporter,
                   "a fail-fast reporter opened a shadow frame (a gate or in_shadow_frame); fail_fast.hpp lists "
                   "what a reporter may call");
#endif
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) return enter_on((*thread).shadow, push);
    return enter_on(state::current_flags().reserve, push);
}

void shadow_frame_leave(const u32 depth, const bool closed) noexcept {
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) {
        leave_on((*thread).shadow, depth, closed);
    } else {
        leave_on(state::current_flags().reserve, depth, closed);
    }
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
    const state::ThreadState* const thread = state::current_thread_state();
    const u32 depth = thread != nullptr ? (*thread).shadow.depth : state::current_flags().reserve.depth;
    const usize result = static_cast<usize>(depth);
    return result;
}

ShadowStackState read_shadow_stack(const MutableView<ShadowFrameRecord> frames) noexcept {
    const state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) return read_stack_of((*thread).shadow, frames);
    return read_stack_of(state::current_flags().reserve, frames);
}

Result<ShadowSummary> read_shadow(const ShadowReference reference, const MutableView<ShadowFrameRecord> frames,
                                  const MutableView<uptr> trace) noexcept {
    const state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) return read_record_of((*thread).shadow, reference, frames, trace);
    return read_record_of(state::current_flags().reserve, reference, frames, trace);
}

ShadowReference park_shadow() noexcept {
    const ShadowReference result = shadow_capture::take_for_current_handler();
    return result;
}

void restore_shadow(const ShadowReference reference) noexcept {
    if (!reference.present()) return;
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) {
        restore_on((*thread).shadow, reference);
    } else {
        restore_on(state::current_flags().reserve, reference);
    }
}

} // namespace stxlib
