#pragma once

// stx_minmallocpp: thread heaps and the slow paths.
//
// A heap is what one thread allocates from without a lock: for each size
// class, the current page's current free word (ClassEntry, all the fast
// path reads) and a queue of the pages the heap owns in that class.
//
// Which heap a thread uses is one trivially destructible thread_local
// pointer (the owner's rule: every thread_local is trivially destructible).
// It names one of:
//   - unbound_heap: the thread has not allocated yet. Its entries all name
//     the empty word, so the first allocation falls to the slow path with
//     no test, and the slow path gives the thread a heap of its own;
//   - a heap of the table: the thread's own, from its first allocation to
//     its exit hook;
//   - ended_heap: the thread's exit hook has run, or the thread could not
//     be given a heap. From then on the thread allocates from the orphan
//     heap, under a lock. Nothing is initialised again and nothing leaks
//     (the mimalloc audit's A4/C2).
// unbound_heap and ended_heap own nothing and are never written.
//
// Heaps live in a table of their own that is never unmapped. A slot is
// used again by a later thread, with a new owner identity: an identity is
// issued once and never again.
//
// Ownership (mimalloc v3's rule: a page is owned only while its owner will
// visit it).
//   - A heap owns the pages it allocates from: fresh ones, and abandoned
//     ones it adopts when it needs a page of the class.
//   - A page with no free block is abandoned by its owner the moment the
//     owner finds it so. Thread exit abandons every page.
//   - A release into an abandoned page claims it for the releasing
//     thread's heap only if that heap is allocating that class and holds
//     fewer than keep_limit pages of it; then the thread's later releases
//     into the page take the fast path. Otherwise the release leaves the
//     page abandoned: it sets the block's remote bit, counts the release in
//     the page's state, puts the page on its class's abandoned list if it
//     was full, and frees the page if that release was its last.
//   So a thread that only releases owns nothing and takes no lock, and what
//   a heap holds without visiting is at most keep_limit pages per class.
//
// Bounds (region.hpp gives the structure's): refill looks at no more than
// 64 words of one page, takes the first page of a queue, pops at most
// adopt_attempts pages from an abandoned list, or takes one run from the
// index. Its retries, and those of abandon and claim, happen only when
// another thread completed a step on the same page in between. Thread exit
// and trim are proportional to the pages the heap owns.

#include "state/stx_minmallocpp/allocator.hpp"
#include "state/stx_minmallocpp/region.hpp"

namespace stxlib::stx_minmallocpp {

inline constexpr u32 adopt_attempts = 4;
inline constexpr u32 keep_limit = 8;

// All the allocate fast path reads for a class. With no current page the
// entry names the empty word and the empty page.
struct ClassEntry final {
    // The free word being allocated from.
    std::atomic<u64>* word{&empty_word};
    // The address of the block that bit 0 of that word stands for.
    uptr word_base{0};
    usize block_bytes{0};
    PageMeta* page{&empty_page};
};

// The pages a heap owns in one class, the current page among them.
struct ClassQueue final {
    PageMeta* first{nullptr};
    PageMeta* last{nullptr};
    u32 pages{0};
};

struct alignas(64) Heap final {
    // The identity pages name as their owner: owner_unbound for the two
    // sentinels, owner_orphan, or an identity issued by acquire (heap.cpp).
    u64 owner{owner_unbound};
    ClassEntry entries[class_count]{};
    ClassQueue queues[class_count]{};
};

extern constinit Heap unbound_heap;
extern constinit Heap ended_heap;
extern constinit thread_local Heap* current_heap;

// The lowest free block of the entry's current word, or null when that word
// is zero. The whole allocate fast path after the thread's heap is known:
// one load, one test, one store, a count of trailing zeros, an increment of
// the page's live count and a multiply; no lock and no atomic
// read-modify-write.
[[nodiscard]] inline void* take_block(ClassEntry& entry) noexcept {
    std::atomic<u64>& cell = *entry.word;
    const u64 word = cell.load(std::memory_order_relaxed);
    if (word == 0) return nullptr;
    cell.store(word & (word - 1), std::memory_order_relaxed);
    PageMeta& page = *entry.page;
    page.live = page.live + 1;
    const uptr bit = static_cast<uptr>(std::countr_zero(word));
    const uptr address = entry.word_base + bit * entry.block_bytes;
    void* const block = reinterpret_cast<void*>(address);
    return block;
}

// A block of class `class_index` when the heap's current word is empty.
// Gives an unbound thread its heap first, and serves a thread without one
// from the orphan heap. Null when the host refuses memory.
[[nodiscard]] void* refill(Heap& heap, usize class_index) noexcept;

// A block of `bytes` in a page of its own inside a region, aligned to
// `alignment` (a power of two up to huge_threshold). Requires
// 1 <= bytes <= huge_threshold. Null when the host refuses memory.
[[nodiscard]] void* allocate_singleton(usize bytes, usize alignment) noexcept;

// False until the first slow path has read the host's page size and
// reserved the heap table, and for ever if the page size is not usable.
[[nodiscard]] bool ensure_started() noexcept;

// Releases block `bit` of the word pair (free_cell, remote_cell) of a page
// the calling heap does not own (heap.hpp's ownership rules say what
// follows). Refused when the block is already free or already released.
[[nodiscard]] Release release_remote(Heap& heap, PageMeta& page, std::atomic<u64>& free_cell,
                                     std::atomic<u64>& remote_cell, u64 bit) noexcept;

// The owner found that a page of its own has no live block. Frees the page
// unless it is the current page of its class and no longer than a medium
// page (512 KiB), which stays for the next allocation.
void release_emptied(Heap& heap, PageMeta& page) noexcept;

// Merges remote releases into every page of the heap and frees its empty
// pages, the current ones included, then returns idle memory to the host.
// Proportional to the pages owned and to the number of regions.
void trim_heap(Heap& heap) noexcept;

// The same for the orphan heap, under its lock: for a thread without a heap.
void trim_orphan() noexcept;

// Abandons every page of the heap and frees its slot. The thread's exit.
void retire_heap(Heap& heap) noexcept;

} // namespace stxlib::stx_minmallocpp
