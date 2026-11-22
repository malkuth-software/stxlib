#pragma once

// The allocator component's inputs (src/state/stx_minmallocpp/allocator.ic.toml):
// kept blocks released through stx_minmallocpp::release, and an address
// inside a kept block, which release must refuse without touching it.

#include "ic/engine/ic_inputs.hpp"
#include "state/stx_minmallocpp/allocator.hpp"

namespace stxlib_ic::inputs {

inline void release_allocated(void* const address) noexcept {
    static_cast<void>(stxlib::stx_minmallocpp::release(address));
}

using AllocatorBlocks = Blocks<&release_allocated>;

// Releases an address inside a kept block (not its start): true when the
// allocator refused it, or when no block is long enough to have one.
inline bool interior_refused(const AllocatorBlocks& blocks, const u32 which) noexcept {
    void* const inside = blocks.interior(which);
    if (inside == nullptr) return true;
    const bool result = stxlib::stx_minmallocpp::release(inside) == stxlib::stx_minmallocpp::Release::refused;
    return result;
}

} // namespace stxlib_ic::inputs
