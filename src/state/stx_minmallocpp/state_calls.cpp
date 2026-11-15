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

void* allocate_block(const usize size, const usize alignment) noexcept {
    void* const result = stx_minmallocpp::allocate(size, alignment);
    return result;
}

ReleaseOutcome release_block(void* const block) noexcept {
    const stx_minmallocpp::Release outcome = stx_minmallocpp::release(block);
    const ReleaseOutcome result =
        outcome == stx_minmallocpp::Release::released ? ReleaseOutcome::released : ReleaseOutcome::refused;
    return result;
}

// stx_minmallocpp has no heap-corruption report (state.hpp, AllocatorCalls):
// it refuses a foreign release without reading the address, so this stays 0.
constinit const std::atomic<u32> no_heap_failure{0};

constinit const AllocatorCalls calls{
    .allocate = allocate_block,
    .release = release_block,
    .backend = backend_value,
    .tags_checked = true,
    .heap_failure = &no_heap_failure,
};

} // namespace

const AllocatorCalls* allocator(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    const AllocatorCalls* const result = &calls;
    return result;
}

} // namespace stxlib::state
