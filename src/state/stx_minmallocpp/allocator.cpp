// stx_minmallocpp: allocate, release, trim and the thread-exit entry
// (allocator.hpp states the contract).
//
// The two fast paths are here in full.
//
// allocate, for a block of up to 512 KiB aligned to at most 64 KiB: round
// the size to the alignment, take its class by arithmetic, load the class's
// current free word, and if it is not zero store it back without its
// lowest bit and return word_base + bit * block size. One thread_local
// read; no lock; no atomic read-modify-write; no call.
//
// release, of a block of a page the calling thread owns: one directory
// cell, one slice_head entry, the block's index by a multiply and a shift
// with an exactness check, then test and set its free bit. Every step that
// can refuse does so before anything is written.

#include "state/stx_minmallocpp/allocator.hpp"

#include "state/stx_minmallocpp/heap.hpp"

#include <bit>

namespace stxlib::stx_minmallocpp {

void* allocate(const usize size, const usize alignment) noexcept {
    if (!std::has_single_bit(alignment) || alignment > alignment_limit || size > largest_request) return nullptr;
    const usize request = size == 0 ? 1 : size;
    if (alignment <= class_alignment_limit) {
        // The class of the size rounded up to the alignment has a block
        // size that is a multiple of the alignment (layout.hpp), so the
        // block is aligned with no further step.
        const usize unit = alignment < block_alignment ? block_alignment : alignment;
        const usize rounded = round_up(request, unit);
        if (rounded <= largest_class_bytes) {
            const usize class_index = class_of(rounded);
            Heap& heap = *current_heap;
            void* const ready = take_block(heap.entries[class_index]);
            if (ready != nullptr) return ready;
            void* const block = refill(heap, class_index);
            return block;
        }
    }
    if (request <= huge_threshold && alignment <= huge_threshold) {
        void* const alone = allocate_singleton(request, alignment);
        return alone;
    }
    if (!ensure_started()) return nullptr;
    // A mapping starts on a 1 GiB boundary, which meets every alignment
    // allocate accepts.
    void* const mapped = allocate_huge(request);
    return mapped;
}

Release release(void* const address) noexcept {
    const uptr value = reinterpret_cast<uptr>(address);
    if ((value >> address_bits) != 0) return Release::refused;
    const u32 cell = directory[value >> region_shift].load(std::memory_order_acquire);
    if (cell != cell_region) {
        if (cell == cell_none) return Release::refused;
        const Release huge = release_huge(cell, value) ? Release::released : Release::refused;
        return huge;
    }
    const uptr base = region_base_of(value);
    const usize slice = static_cast<usize>(value - base) >> slice_shift;
    const u32 head_code = slice_heads(base)[slice].load(std::memory_order_acquire);
    if (head_code == 0) return Release::refused;
    const usize head = slice - (head_code - 1);
    PageMeta& page = page_metas(base)[head];
    const u64 offset = value - (base + (static_cast<uptr>(head) << slice_shift));
    const u64 index = block_index(offset, page.divide_magic);
    // Only a block's first byte passes: an address inside a block, or in
    // the unused tail of a page, is not a block.
    if (index * page.block_bytes != offset || index >= page.capacity) return Release::refused;
    const usize word = static_cast<usize>(index / word_bits);
    const u64 bit = static_cast<u64>(1) << (index % word_bits);
    std::atomic<u64>& free_cell = free_words(base, head)[word];
    Heap& heap = *current_heap;
    const u32 state = page.state.load(std::memory_order_relaxed);
    // One test for the fast path: the page is mine and nothing is pending.
    if (state != state_of_owner(heap.owner)) {
        if (owner_of_state(state) != heap.owner) {
            const Released remote = release_remote(heap, page, free_cell, remote_words(base, head)[word], bit);
            const Release outcome = remote == Released::released ? Release::released : Release::refused;
            return outcome;
        }
        // Mine, with releases from other threads pending: merge them first,
        // so that a block another thread already released is seen as free
        // below and its second release is refused.
        collect(page);
    }
    const u64 known = free_cell.load(std::memory_order_relaxed);
    if ((known & bit) != 0) return Release::refused;
    free_cell.store(known | bit, std::memory_order_relaxed);
    page.summary = page.summary | (static_cast<u64>(1) << word);
    page.live = page.live - 1;
    if (page.live == 0) release_emptied(heap, page);
    return Release::released;
}

void trim() noexcept {
    Heap& heap = *current_heap;
    if (heap.owner == owner_unbound) return;
    trim_heap(heap);
}

void thread_ending(void* const context) noexcept {
    // The host layer passes the heap the thread armed, or null where the
    // host's hook runs for every thread (Windows); then the thread's
    // pointer says whether it has one.
    Heap* const heap = context != nullptr ? static_cast<Heap*>(context) : current_heap;
    if ((*heap).owner == owner_unbound) return;
    current_heap = &ended_heap;
    retire_heap(*heap);
}

} // namespace stxlib::stx_minmallocpp
