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

#include "stxlib/error.hpp"
#include "stxlib/shadow.hpp"

#include "hosted/host.hpp"
#include "state/state.hpp"

namespace stxlib::shadow_capture {

// Files the thread's logical stack (and, where the store keeps one, a
// native trace) as a new shadow record and makes it the pending capture.
// Returns its reference, or none when the thread's shadow sequence is
// exhausted (2^64 - 1 records).
template <typename Store>
ShadowReference capture(Store& store, const CapturePoint point) noexcept {
    state::ThreadFlags& flags = state::current_flags();
    if (flags.last_shadow_sequence == u64_max) return ShadowReference{};
    flags.last_shadow_sequence = flags.last_shadow_sequence + 1;
    const usize slot = store.shadow_count == 0 ? 0 : (store.shadow_newest + 1) % Store::records_kept;
    typename Store::Record& record = store.shadows[slot];
    record.sequence = flags.last_shadow_sequence;
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
    store.shadow_newest = slot;
    if (store.shadow_count < Store::records_kept) store.shadow_count = store.shadow_count + 1;
    store.pending_shadow = record.sequence;
    store.pending_pushes = store.pushes;
    const ShadowReference result{record.sequence};
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
