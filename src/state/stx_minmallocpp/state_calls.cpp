// stxlib_state's allocator (state.hpp, AllocatorCalls) over stx_minmallocpp.
// Compiled in place of src/state/allocator.cpp when STXLIB_ALLOCATOR is
// stx_minmallocpp (CMakeLists.txt).
//
// No tag header and no registry: stx_minmallocpp itself refuses a release
// of anything that is not the start of a live block of its own, without
// reading the address (allocator.hpp), in every build. So tags_checked is
// true whatever STXLIB_ALLOCATION_TAGS says, and the block's address is the
// backend's own.

#include "state/state.hpp"

#include "state/stx_minmallocpp/allocator.hpp"

namespace stxlib::state {

namespace {

constexpr u8 backend_value = 3; // memory::Backend::stx_minmallocpp

// The two result types name the same two outcomes with the same values, so
// the conversion below is the value itself and the call can be the
// function's last step.
static_assert(static_cast<u8>(stx_minmallocpp::Release::released) == static_cast<u8>(ReleaseOutcome::released) &&
                  static_cast<u8>(stx_minmallocpp::Release::refused) == static_cast<u8>(ReleaseOutcome::refused),
              "stx_minmallocpp's release outcomes are stxlib_state's");

ReleaseOutcome release_block(void* const block) noexcept {
    const stx_minmallocpp::Release outcome = stx_minmallocpp::release(block);
    const ReleaseOutcome result = static_cast<ReleaseOutcome>(static_cast<u8>(outcome));
    return result;
}

constinit const AllocatorCalls calls{
    // The same signature: the allocator's own function, with no step between.
    .allocate = stx_minmallocpp::allocate,
    .release = release_block,
    .backend = backend_value,
    .tags_checked = true,
};

} // namespace

const AllocatorCalls* allocator(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    const AllocatorCalls* const result = &calls;
    return result;
}

} // namespace stxlib::state
