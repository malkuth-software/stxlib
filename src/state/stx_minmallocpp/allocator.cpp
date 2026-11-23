// stx_minmallocpp: allocate, release, trim and the thread-exit entries
// (allocator.hpp states the contract).
//
// Each of allocate and release is a short common path, compiled into the
// public function itself, that hands every other case to a general routine
// kept out of line. The common paths call nothing but that hand-over, so
// they keep no value across a call and need no saved registers.
//
// allocate's common path: a request with no alignment beyond a block's own
// and a size the classes hold. Take its class by arithmetic, load the
// class's current free word, and if it is not zero store it back without
// its lowest bit, count the block live, and return word_base + bit * block
// size. No lock and no atomic read-modify-write.
//
// release's common path: a block of a page the calling thread owns with
// nothing pending from other threads, which is not the page's last live
// block. One directory cell, one slice_head entry, the block's index by one
// multiply (its low bits say whether the address is a block's first byte),
// then test and set its free bit. Every step that can refuse does so before
// anything is written.
//
// Both read the calling thread's heap pointer once; in a shared library
// that is a call into the host's loader unless the build chooses otherwise
// (DESIGN.md 8, item 1). Measured on the M4: that read is not what a small
// call's time is made of (tests/stx_minmallocpp_floor_bench.cpp).

#include "state/stx_minmallocpp/allocator.hpp"

#include "state/stx_minmallocpp/allocator_ic_checks.hpp"
#include "state/stx_minmallocpp/heap.hpp"

#include <bit>

namespace stxlib::stx_minmallocpp {

namespace {

// Every allocate the common path does not take: a bad argument, a size of
// zero, an alignment above a block's own, a size above the classes.
[[gnu::noinline]] void* allocate_general(Heap& heap, const usize size, const usize alignment) noexcept {
    if (!ic::admits_allocate(size, alignment)) return nullptr;
    const usize request = size == 0 ? 1 : size;
    if (alignment <= class_alignment_limit) {
        // The class of the size rounded up to the alignment has a block
        // size that is a multiple of the alignment (layout.hpp), so the
        // block is aligned with no further step.
        const usize unit = alignment < block_alignment ? block_alignment : alignment;
        const usize rounded = round_up(request, unit);
        if (rounded <= largest_class_bytes) {
            void* const block = allocate_class_with(heap, class_of(rounded));
            return block;
        }
    }
    if (singleton_admits(request, alignment)) {
        void* const alone = allocate_singleton(request, alignment);
        return alone;
    }
    // allocate_huge takes a lock (allocator.hpp, "Process end"); refill and
    // allocate_singleton make the same test themselves.
    if (os::process_ending()) return nullptr;
    if (!ensure_started()) return nullptr;
    // A mapping starts on a region boundary (1 GiB; 64 MiB on wasm32),
    // which meets every alignment allocate accepts.
    void* const mapped = allocate_huge(request);
    return mapped;
}

// allocate's common path. Class sizes are multiples of 16, so the class of
// the size itself is the class of the size rounded up to 16. Forced inline:
// it is the body of both allocate and allocate_with.
[[gnu::always_inline]] inline void* allocate_common(Heap& heap, const usize size, const usize alignment) noexcept {
    if (alignment <= block_alignment && size != 0 && size <= largest_class_bytes && std::has_single_bit(alignment)) {
        const usize class_index = class_of(size);
        ClassEntry& entry = heap.entries[class_index];
        std::atomic<u64>& cell = *entry.word;
        const u64 word = cell.load(std::memory_order_relaxed);
        if (word != 0) {
            void* const block = take_from(entry, cell, word);
            return block;
        }
        void* const refilled = refill(heap, class_index);
        return refilled;
    }
    void* const other = allocate_general(heap, size, alignment);
    return other;
}

// Every release the common path does not finish: an address in a huge
// mapping, a page that is not the caller's or has releases pending, a block
// that is already free. It decides everything again from the address, so
// the common path hands over nothing but the address.
[[gnu::noinline]] Release release_general(Heap& heap, void* const address) noexcept {
    // A huge release, a release into another thread's page and an emptied
    // page may each take a lock: from the process's end on, the block stays
    // where it is (allocator.hpp, "Process end").
    if (os::process_ending()) return Release::released;
    const uptr value = reinterpret_cast<uptr>(address);
    if ((value >> address_bits) != 0) return Release::refused;
    const u32 cell = directory_cell(static_cast<usize>(value >> region_shift));
    if (cell != cell_region) {
        if (cell == cell_none) return Release::refused;
        const Release huge = release_huge(cell, value) ? Release::released : Release::refused;
        return huge;
    }
    const uptr base = region_base_of(value);
    const usize slice = static_cast<usize>(value - base) >> slice_shift;
    STX_MINMALLOCPP_STEP();
    const u32 head_code = slice_heads(base)[slice].load(std::memory_order_acquire);
    if (head_code == 0) return Release::refused;
    const usize head = static_cast<usize>(head_code) - 1;
    PageMeta& page = page_metas(base)[head];
    const u64 offset = value - (base + (static_cast<uptr>(head) << slice_shift));
    const u64 scaled = offset * page.divide_magic;
    const u64 index = scaled >> divide_shift;
    // Only a block's first byte passes: an address inside a block, or in
    // the unused tail of a page, is not a block.
    if (!block_starts_at(scaled, page.divide_magic) || index >= page.capacity) return Release::refused;
    const usize word = static_cast<usize>(index / word_bits);
    const u64 bit = static_cast<u64>(1) << (index % word_bits);
    std::atomic<u64>& free_cell = free_words(base, head)[word];
    STX_MINMALLOCPP_STEP();
    const u64 state = page.state.load(std::memory_order_relaxed);
    if (state != heap.clean_state) {
        if (owner_of_state(state) != heap.owner) {
            const Release remote = release_remote(heap, page, free_cell, remote_words(base, head)[word], bit);
            return remote;
        }
        // Mine, with releases from other threads pending: merge them first,
        // so that a block another thread already released is seen as free
        // below and its second release is refused.
        collect(page, heap.owner);
    }
    const u64 known = free_cell.load(std::memory_order_relaxed);
    if ((known & bit) != 0) {
        // Refused. The merge above may have emptied the page.
        if (page.live == 0) release_emptied(heap, page);
        return Release::refused;
    }
    STX_MINMALLOCPP_STEP();
    free_cell.store(known | bit, std::memory_order_relaxed);
    page.summary = page.summary | (static_cast<u64>(1) << word);
    page.live = page.live - 1;
    if (page.live == 0) release_emptied(heap, page);
    return Release::released;
}

// The common path released a page's last live block.
[[gnu::noinline]] Release release_last(Heap& heap, PageMeta& page) noexcept {
    // The block is free already; only the page's return, which may take a
    // lock, is left out at the process's end.
    if (os::process_ending()) return Release::released;
    release_emptied(heap, page);
    return Release::released;
}

// release's common path. Forced inline: it is the body of both release and
// release_with. It writes nothing before its last test.
[[gnu::always_inline]] inline Release release_common(Heap& heap, void* const address) noexcept {
    const uptr value = reinterpret_cast<uptr>(address);
    if ((value >> address_bits) != 0) return Release::refused;
    const u32 cell = directory_cell(static_cast<usize>(value >> region_shift));
    if (cell != cell_region) {
        const Release other = release_general(heap, address);
        return other;
    }
    const uptr base = region_base_of(value);
    const usize slice = static_cast<usize>(value - base) >> slice_shift;
    STX_MINMALLOCPP_STEP();
    const u32 head_code = slice_heads(base)[slice].load(std::memory_order_acquire);
    if (head_code == 0) return Release::refused;
    const usize head = static_cast<usize>(head_code) - 1;
    PageMeta& page = page_metas(base)[head];
    const u64 offset = value - (base + (static_cast<uptr>(head) << slice_shift));
    const u64 scaled = offset * page.divide_magic;
    const u64 index = scaled >> divide_shift;
    if (!block_starts_at(scaled, page.divide_magic) || index >= page.capacity) return Release::refused;
    const usize word = static_cast<usize>(index / word_bits);
    const u64 bit = static_cast<u64>(1) << (index % word_bits);
    std::atomic<u64>& free_cell = free_words(base, head)[word];
    STX_MINMALLOCPP_STEP();
    // One test: the page is mine and nothing is pending.
    if (page.state.load(std::memory_order_relaxed) != heap.clean_state) {
        const Release other = release_general(heap, address);
        return other;
    }
    const u64 known = free_cell.load(std::memory_order_relaxed);
    if ((known & bit) != 0) {
        const Release twice = release_general(heap, address);
        return twice;
    }
    STX_MINMALLOCPP_STEP();
    free_cell.store(known | bit, std::memory_order_relaxed);
    page.summary = page.summary | (static_cast<u64>(1) << word);
    page.live = page.live - 1;
    if (page.live == 0) {
        const Release last = release_last(heap, page);
        return last;
    }
    return Release::released;
}

} // namespace

void* allocate_class_with(Heap& heap, const usize class_index) noexcept {
    void* const ready = take_block(heap.entries[class_index]);
    if (ready != nullptr) return ready;
    void* const block = refill(heap, class_index);
    return block;
}

void* allocate_with(Heap& heap, const usize size, const usize alignment) noexcept {
    void* const block = allocate_common(heap, size, alignment);
    return block;
}

Release release_with(Heap& heap, void* const address) noexcept {
    const Release outcome = release_common(heap, address);
    return outcome;
}

void* allocate(const usize size, const usize alignment) noexcept {
    void* const block = allocate_common(*current_heap, size, alignment);
    return block;
}

Release release(void* const address) noexcept {
    const Release outcome = release_common(*current_heap, address);
    return outcome;
}

void trim() noexcept {
    if (os::process_ending()) return;
    Heap& heap = *current_heap;
    if (heap.owner == owner_unbound) {
        trim_orphan();
        return;
    }
    trim_heap(heap);
}

void end_thread() noexcept {
    Heap* heap = current_heap;
    if (heap == &unbound_heap) {
        // The pointer reads as a fresh thread's either because the thread
        // never allocated or because the host freed its thread_local
        // storage while the thread still has a heap (os.hpp,
        // armed_context; macOS dyld, from a key destructor of its own).
        // The host still holds the heap the thread armed: give that up, so
        // that marking the thread ended below does not lose it (the
        // security audit's A-stxlib-allocator-state-01-F4).
        void* const armed = os::armed_context();
        if (armed != nullptr) heap = static_cast<Heap*>(armed);
    }
    current_heap = &ended_heap;
    os::mark_thread_ended();
    if ((*heap).owner == owner_unbound) return;
    // retire_heap takes locks (allocator.hpp, "Process end").
    if (os::process_ending()) return;
    retire_heap(*heap);
}

void thread_ending(void* const context) noexcept {
    // The host layer passes the heap the thread armed, or null where the
    // host's hook runs for every thread (Windows); then the thread's
    // pointer says whether it has one. A thread with no heap of its own
    // has nothing to give up.
    Heap* const heap = context != nullptr ? static_cast<Heap*>(context) : current_heap;
    if ((*heap).owner == owner_unbound) {
        current_heap = &ended_heap;
        return;
    }
    current_heap = &ended_heap;
    if (os::process_ending()) return;
    retire_heap(*heap);
}

} // namespace stxlib::stx_minmallocpp
