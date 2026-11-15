#pragma once

// The capture of a shadow record, shared by the scopes (shadow_frames.cpp)
// and the gates (exception_shadow.cpp). Every build; the native trace is
// taken in development builds only, and only into a store that keeps one.
//
// Since layout 10 a thread's shadow state is in one of two stores
// (state.hpp): the block's FullShadow, or, with no block (its allocation
// failed, or the thread-exit hook has released it), the anchor's
// ReserveShadow. The functions below work on either; take_for_current_handler
// picks the calling thread's. The record numbers come from ThreadFlags in
// both cases, so a reference never names a record of the other store.

#include "stxlib/detail/utf8.hpp"
#include "stxlib/error.hpp"
#include "stxlib/shadow.hpp"

#include "hosted/host.hpp"
#include "state/state.hpp"

namespace stxlib::shadow_capture {

// The ring slot a new record takes: the next after the newest, which is
// the oldest once the ring is full.
template <typename Store>
[[nodiscard]] usize next_slot(const Store& store) noexcept {
    const usize result = store.shadow_count == 0 ? 0 : (store.shadow_newest + 1) % Store::records_kept;
    return result;
}

// Makes the record just filled at `slot` the newest and the pending
// capture, and returns its reference.
template <typename Store>
ShadowReference publish(Store& store, const usize slot) noexcept {
    store.shadow_newest = slot;
    if (store.shadow_count < Store::records_kept) store.shadow_count = store.shadow_count + 1;
    store.pending_shadow = store.shadows[slot].sequence;
    store.pending_pushes = store.pushes;
    const ShadowReference result{store.shadows[slot].sequence};
    return result;
}

// The next shadow sequence of the calling thread, or 0 when it is exhausted
// (2^64 - 1 records).
[[nodiscard]] inline u64 next_sequence() noexcept {
    state::ThreadFlags& flags = state::current_flags();
    if (flags.last_shadow_sequence == u64_max) return 0;
    flags.last_shadow_sequence = flags.last_shadow_sequence + 1;
    return flags.last_shadow_sequence;
}

// Files the thread's logical stack (and, where the store keeps one, a
// native trace) as a new shadow record and makes it the pending capture.
// Returns its reference, or none when the thread's shadow sequence is
// exhausted (2^64 - 1 records).
template <typename Store>
ShadowReference capture(Store& store, const CapturePoint point) noexcept {
    const u64 sequence = next_sequence();
    if (sequence == 0) return ShadowReference{};
    const usize slot = next_slot(store);
    typename Store::Record& record = store.shadows[slot];
    record.sequence = sequence;
    record.point = point;
    const usize depth = static_cast<usize>(store.depth);
    const usize stored = depth < Store::frame_capacity ? depth : Store::frame_capacity;
    for (usize index = 0; index < stored; ++index) record.frames[index] = store.frames[index];
    record.frame_count = stored;
    record.frames_lost = depth - stored;
    if constexpr (Store::trace_capacity > 0) {
        record.trace_count = host::capture_return_addresses(record.trace, Store::trace_capacity);
    } else {
        record.trace_count = 0; // a release build, or the reserve, takes no trace (S5 = 1)
    }
    const ShadowReference result = publish(store, slot);
    return result;
}

// Files frames and a trace a caller copied out (park_shadow_into) as a new
// shadow record and makes it the pending capture (restore_shadow_from).
// Keeps what the store can hold: frames past its capacity are counted as
// lost, and the trace is kept only where the store keeps one. Returns its
// reference, or none when the thread's shadow sequence is exhausted.
// A frame handed back through restore_shadow_from is input from its
// carrier: its text counts are cut to the arrays and to their well-formed
// UTF-8 prefix, since subject_text() and name_text() adopt the bytes as
// validated text (security audit 2026-11-15, A-stxlib-include-01, "needs a
// check": restored shadow frames keep the caller's text counts).
inline void keep_text_whole(ShadowFrameRecord& frame) noexcept {
    const usize subject = frame.subject_count < shadow_subject_capacity ? frame.subject_count : shadow_subject_capacity;
    const usize name = frame.name_count < shadow_name_capacity ? frame.name_count : shadow_name_capacity;
    frame.subject_count = static_cast<u32>(detail::utf8_prefix(frame.subject, subject, subject));
    frame.name_count = static_cast<u32>(detail::utf8_prefix(frame.name, name, name));
}

template <typename Store>
ShadowReference file_copy(Store& store, const CapturePoint point, const View<ShadowFrameRecord> frames,
                          const usize frames_lost, const View<uptr> trace) noexcept {
    const u64 sequence = next_sequence();
    if (sequence == 0) return ShadowReference{};
    const usize slot = next_slot(store);
    typename Store::Record& record = store.shadows[slot];
    record.sequence = sequence;
    record.point = point;
    const usize stored = frames.count() < Store::frame_capacity ? frames.count() : Store::frame_capacity;
    for (usize index = 0; index < stored; ++index) {
        record.frames[index] = frames[index];
        keep_text_whole(record.frames[index]);
    }
    record.frame_count = stored;
    record.frames_lost = frames_lost + (frames.count() - stored);
    if constexpr (Store::trace_capacity > 0) {
        const usize traced = trace.count() < Store::trace_capacity ? trace.count() : Store::trace_capacity;
        for (usize index = 0; index < traced; ++index) record.trace[index] = trace[index];
        record.trace_count = traced;
    } else {
        static_cast<void>(trace); // a release build, or the reserve, keeps no trace (S5 = 1)
        record.trace_count = 0;
    }
    const ShadowReference result = publish(store, slot);
    return result;
}

// True when the pending capture belongs to the exception unwinding now: one
// was taken and no scope has been entered since (normal execution has not
// resumed).
template <typename Store>
[[nodiscard]] bool pending_is_current(const Store& store) noexcept {
    const bool result = store.pending_shadow != 0 && store.pending_pushes == store.pushes;
    return result;
}

// For a handler: the pending capture if current, else a capture at the
// handler (CapturePoint::gate); the pending capture is cleared.
template <typename Store>
ShadowReference take_for_handler(Store& store) noexcept {
    ShadowReference result{};
    if (pending_is_current(store)) {
        result.sequence = store.pending_shadow;
    } else {
        result = capture(store, CapturePoint::gate);
    }
    store.pending_shadow = 0;
    return result;
}

// take_for_handler on the calling thread's store.
inline ShadowReference take_for_current_handler() noexcept {
    state::ThreadState* const thread = state::current_thread_state();
    if (thread != nullptr) return take_for_handler((*thread).shadow);
    return take_for_handler(state::current_flags().reserve);
}

} // namespace stxlib::shadow_capture
