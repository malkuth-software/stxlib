// stx_minmallocpp: thread heaps and the slow paths (heap.hpp).
//
// The ownership protocol, in one place (DESIGN.md 4.3 argues why no
// release is lost; proofs/REPORT.md lists it as not yet proved):
//
//   owner releases a block      free bit, summary bit, live - 1 (allocator.cpp)
//   non-owner releases a block  remote bit (atomic OR), then pending (atomic
//                               OR into state); if the state it got back
//                               says abandoned, claim (release_remote)
//   collect                     owner: pending away, then every remote word
//                               exchanged for 0 and merged (page.hpp)
//   abandon                     owner: compare-exchange state from "mine,
//                               not pending" to abandoned; if pending was
//                               set, collect and try again (abandon)
//   claim                       compare-exchange state from abandoned to
//                               "mine, not pending", then collect (claim)
//
// A page's owner-only fields are touched only between a successful claim
// (or the page's making) and the abandon or free that ends that ownership.
//
// The abandoned lists (Structure::abandoned_first) hold abandoned pages
// that had a free block when abandoned. PageMeta::list_class and the links
// of a listed page are written only under the structure lock. Every claim
// is followed, before the claimer uses the links, by unlist under that
// lock; a thread that lists a page checks, under the lock, that the page
// is abandoned and not listed. So a page is listed at most once and an
// owned page is unlisted before its owner links it. A list may hold a page
// that has since been claimed, reused and abandoned again in another
// shape; an adopter checks what it claimed and abandons it again if it is
// not what it wanted.

#include "state/stx_minmallocpp/heap.hpp"

#include <new>

namespace stxlib::stx_minmallocpp {

constinit Heap unbound_heap{};
constinit Heap ended_heap{};
constinit thread_local Heap* current_heap = &unbound_heap;

namespace {

constinit Heap orphan_heap{.owner = owner_orphan};
constinit Lock orphan_lock{};
constinit std::atomic<u32> integrity_found{static_cast<u32>(Integrity::whole)};

inline constexpr usize heap_table_bytes = (heap_capacity * sizeof(Heap) + slice_bytes - 1) / slice_bytes * slice_bytes;

// ---- class queues ----------------------------------------------------------------

void link_first(ClassQueue& queue, PageMeta& page) noexcept {
    page.previous = nullptr;
    page.next = queue.first;
    if (queue.first != nullptr) {
        (*queue.first).previous = &page;
    } else {
        queue.last = &page;
    }
    queue.first = &page;
}

void link_last(ClassQueue& queue, PageMeta& page) noexcept {
    page.next = nullptr;
    page.previous = queue.last;
    if (queue.last != nullptr) {
        (*queue.last).next = &page;
    } else {
        queue.first = &page;
    }
    queue.last = &page;
}

void unlink(ClassQueue& queue, PageMeta& page) noexcept {
    if (page.previous != nullptr) {
        (*page.previous).next = page.next;
    } else {
        queue.first = page.next;
    }
    if (page.next != nullptr) {
        (*page.next).previous = page.previous;
    } else {
        queue.last = page.previous;
    }
    page.next = nullptr;
    page.previous = nullptr;
}

// ---- the abandoned lists (structure lock held) ------------------------------------

void unlist(PageMeta& page) noexcept {
    if (page.list_class == not_listed) return;
    const usize class_index = page.list_class;
    if (page.previous != nullptr) {
        (*page.previous).next = page.next;
    } else {
        structure.abandoned_first[class_index] = page.next;
    }
    if (page.next != nullptr) (*page.next).previous = page.previous;
    page.next = nullptr;
    page.previous = nullptr;
    page.list_class = not_listed;
    static_cast<void>(structure.abandoned_count[class_index].fetch_sub(1, std::memory_order_relaxed));
}

// Lists a page its caller has just abandoned, if it is still abandoned and
// not listed. The caller no longer owns the page: only the state (atomic)
// and the lock-guarded fields are read.
void list_abandoned(PageMeta& page, const usize class_index) noexcept {
    const LockScope scope(structure.lock);
    if (page.list_class != not_listed) return;
    if (owner_of_state(page.state.load()) != owner_abandoned) return;
    page.previous = nullptr;
    page.next = structure.abandoned_first[class_index];
    if (page.next != nullptr) (*page.next).previous = &page;
    structure.abandoned_first[class_index] = &page;
    page.list_class = static_cast<u8>(class_index);
    static_cast<void>(structure.abandoned_count[class_index].fetch_add(1, std::memory_order_relaxed));
}

// ---- making and freeing pages ----------------------------------------------------

// The fixed facts of a page to be made.
struct PagePlan final {
    usize slices{0};
    usize block_bytes{0};
    u64 divide_magic{0};
    usize capacity{0};
    usize words{0};
    usize class_index{0};
    u32 owner{owner_none};
    // True: every block starts free. False: every block starts live.
    bool blocks_free{false};
};

// Makes the page at `run`: its words, its PageMeta, then its slice_head
// entries, which publish it. The run is the caller's alone, so nothing here
// needs the lock. A remote word left non-zero means a block of the page
// that was here before was released after that page was freed.
PageMeta& make_page(const RunPlace run, const PagePlan& plan) noexcept {
    std::atomic<u64>* const free_cells = free_words(run.base, run.slice);
    std::atomic<u64>* const remote_cells = remote_words(run.base, run.slice);
    for (usize word = 0; word < plan.words; ++word) {
        if (remote_cells[word].load() != 0) integrity_stop(Integrity::released_into_freed_page);
        u64 free_bits = 0;
        if (plan.blocks_free) {
            const usize remaining = plan.capacity - word * word_bits;
            free_bits = remaining >= word_bits ? u64_max : (static_cast<u64>(1) << remaining) - 1;
        }
        free_cells[word].store(free_bits, std::memory_order_relaxed);
    }
    PageMeta& page = page_metas(run.base)[run.slice];
    page.slices = static_cast<u32>(plan.slices);
    page.block_bytes = plan.block_bytes;
    page.divide_magic = plan.divide_magic;
    page.capacity = static_cast<u32>(plan.capacity);
    page.live = plan.blocks_free ? 0 : static_cast<u32>(plan.capacity);
    page.summary = 0;
    if (plan.blocks_free) {
        page.summary = plan.words >= word_bits ? u64_max : (static_cast<u64>(1) << plan.words) - 1;
    }
    page.class_index = static_cast<u16>(plan.class_index);
    page.words = static_cast<u8>(plan.words);
    page.list_class = not_listed;
    page.next = nullptr;
    page.previous = nullptr;
    page.state.store(state_of_owner(plan.owner));
    std::atomic<u32>* const heads = slice_heads(run.base);
    for (usize slice = 0; slice < plan.slices; ++slice) {
        heads[run.slice + slice].store(static_cast<u32>(slice + 1), std::memory_order_release);
    }
    return page;
}

// Frees a page its caller owns, with no live block and in no heap queue:
// unpublishes its slices, marks its state "no page", and gives its run
// back. Proportional to the page's slices.
void free_page(PageMeta& page) noexcept {
    const PagePlace place = place_of(page);
    const usize slices = page.slices;
    std::atomic<u32>* const heads = slice_heads(place.base);
    const LockScope scope(structure.lock);
    unlist(page);
    for (usize slice = 0; slice < slices; ++slice) {
        heads[place.head + slice].store(0, std::memory_order_release);
    }
    page.state.store(state_of_owner(owner_none));
    give_run(place.base, place.head, slices);
}

// ---- claim and abandon -------------------------------------------------------------

// Makes `owner` the owner of an abandoned page; false if the page has an
// owner. The new state has pending clear: the claimer collects next.
[[nodiscard]] bool claim(PageMeta& page, const u32 owner) noexcept {
    u32 seen = page.state.load();
    while (owner_of_state(seen) == owner_abandoned) {
        if (page.state.compare_exchange_weak(seen, state_of_owner(owner))) return true;
    }
    return false;
}

// Gives up a page `owner` owns and has taken out of its queue. Frees it if
// nothing in it is live; otherwise abandons it, and lists it if it has a
// free block. Repeats only while other threads keep releasing into it.
void abandon(PageMeta& page, const u32 owner) noexcept {
    for (;;) {
        if ((page.state.load(std::memory_order_relaxed) & pending_bit) != 0) collect(page);
        if (page.live == 0) {
            free_page(page);
            return;
        }
        const bool has_space = page.live < page.capacity;
        const usize class_index = page.class_index;
        u32 expected = state_of_owner(owner);
        if (page.state.compare_exchange_strong(expected, state_of_owner(owner_abandoned))) {
            // The page is no longer ours: only list_abandoned's checks from here.
            if (has_space && class_index < class_count) list_abandoned(page, class_index);
            return;
        }
    }
}

// ---- refill ------------------------------------------------------------------------

// Points the entry at the page's lowest non-zero free word; false when the
// page has none. At most 64 steps.
[[nodiscard]] bool point_at_free_word(ClassEntry& entry, PageMeta& page) noexcept {
    const PagePlace place = place_of(page);
    std::atomic<u64>* const cells = free_words(place.base, place.head);
    while (page.summary != 0) {
        const usize word = static_cast<usize>(std::countr_zero(page.summary));
        if (cells[word].load(std::memory_order_relaxed) != 0) {
            entry.word = &cells[word];
            entry.word_base = area_of(place) + static_cast<uptr>(word * word_bits) * page.block_bytes;
            return true;
        }
        page.summary = page.summary & (page.summary - 1);
    }
    return false;
}

// An abandoned page of the class with a free block, claimed and linked
// first in the heap's queue; null when there is none within
// adopt_attempts pops.
[[nodiscard]] PageMeta* adopt(Heap& heap, const usize class_index) noexcept {
    for (u32 attempt = 0; attempt < adopt_attempts; ++attempt) {
        if (structure.abandoned_count[class_index].load(std::memory_order_relaxed) == 0) return nullptr;
        PageMeta* candidate = nullptr;
        bool claimed = false;
        {
            const LockScope scope(structure.lock);
            candidate = structure.abandoned_first[class_index];
            if (candidate == nullptr) return nullptr;
            unlist(*candidate);
            claimed = claim(*candidate, heap.owner);
        }
        if (!claimed) continue;
        PageMeta& page = *candidate;
        collect(page);
        if (page.live == 0) {
            free_page(page);
            continue;
        }
        if (page.class_index != class_index || page.live == page.capacity) {
            abandon(page, heap.owner);
            continue;
        }
        link_first(heap.queues[class_index], page);
        return &page;
    }
    return nullptr;
}

// A new page of the class, every block free, linked first in the heap's
// queue; null when the host refuses.
[[nodiscard]] PageMeta* fresh_page(Heap& heap, const usize class_index) noexcept {
    const ClassShape& shape = class_table.shapes[class_index];
    RunPlace run{};
    {
        const LockScope scope(structure.lock);
        run = take_run(shape.page_slices, 1);
    }
    if (run.base == 0) return nullptr;
    const PagePlan plan{
        .slices = shape.page_slices,
        .block_bytes = shape.block_bytes,
        .divide_magic = shape.divide_magic,
        .capacity = shape.capacity,
        .words = shape.words,
        .class_index = class_index,
        .owner = heap.owner,
        .blocks_free = true,
    };
    PageMeta& page = make_page(run, plan);
    link_first(heap.queues[class_index], page);
    return &page;
}

// The slow path for a heap that owns pages (a thread heap, or the orphan
// heap under its lock).
[[nodiscard]] void* refill_owned(Heap& heap, const usize class_index) noexcept {
    ClassEntry& entry = heap.entries[class_index];
    ClassQueue& queue = heap.queues[class_index];
    for (;;) {
        if (entry.page != &empty_page) {
            PageMeta& page = *entry.page;
            if (point_at_free_word(entry, page)) {
                void* const block = take_block(entry);
                return block;
            }
            if ((page.state.load(std::memory_order_relaxed) & pending_bit) != 0) {
                collect(page);
                if (page.summary != 0) continue;
            }
            // Full. Take it out of the queue first: once abandoned, its
            // links are the next owner's.
            unlink(queue, page);
            entry = ClassEntry{};
            u32 expected = state_of_owner(heap.owner);
            if (!page.state.compare_exchange_strong(expected, state_of_owner(owner_abandoned))) {
                // A release arrived in between: the page is still ours.
                link_first(queue, page);
                entry.page = &page;
                entry.block_bytes = static_cast<usize>(page.block_bytes);
                continue;
            }
        }
        PageMeta* next = queue.first;
        if (next == nullptr) next = adopt(heap, class_index);
        if (next == nullptr) next = fresh_page(heap, class_index);
        if (next == nullptr) return nullptr;
        entry.page = next;
        entry.block_bytes = class_table.shapes[class_index].block_bytes;
    }
}

[[nodiscard]] void* allocate_in(Heap& heap, const usize class_index) noexcept {
    void* const ready = take_block(heap.entries[class_index]);
    if (ready != nullptr) return ready;
    void* const block = refill_owned(heap, class_index);
    return block;
}

// ---- heap slots ----------------------------------------------------------------------

// A free heap slot, constructed; null when the table is full or the host
// refuses to commit a new slot.
[[nodiscard]] Heap* acquire_heap() noexcept {
    const LockScope scope(structure.lock);
    if (structure.free_heap != nullptr) {
        Heap* const reused = structure.free_heap;
        structure.free_heap = (*reused).next_free;
        (*reused).next_free = nullptr;
        return reused;
    }
    if (structure.heap_slots_made == heap_capacity) return nullptr;
    const usize slot = structure.heap_slots_made;
    const uptr address = reinterpret_cast<uptr>(structure.heap_table) + slot * sizeof(Heap);
    const uptr page_mask = static_cast<uptr>(structure.os_page - 1);
    const uptr commit_start = address & ~page_mask;
    const uptr commit_end = (address + sizeof(Heap) + page_mask) & ~page_mask;
    if (!os::commit(reinterpret_cast<void*>(commit_start), static_cast<usize>(commit_end - commit_start))) return nullptr;
    Heap* const made = new (reinterpret_cast<void*>(address)) Heap{};
    (*made).owner = first_heap_owner + static_cast<u32>(slot);
    structure.heap_slots_made = slot + 1;
    return made;
}

void free_heap_slot(Heap& heap) noexcept {
    const LockScope scope(structure.lock);
    heap.next_free = structure.free_heap;
    structure.free_heap = &heap;
}

// The slow path for a thread whose pointer names a sentinel. An unbound
// thread gets a heap and its exit hook; if either cannot be had, or the
// thread has ended, it is served from the orphan heap.
[[nodiscard]] void* refill_unbound(Heap& given, const usize class_index) noexcept {
    if (!ensure_started()) return nullptr;
    if (&given == &unbound_heap) {
        Heap* const fresh = acquire_heap();
        if (fresh != nullptr) {
            if (os::arm_thread_exit(fresh)) {
                current_heap = fresh;
                void* const block = allocate_in(*fresh, class_index);
                return block;
            }
            free_heap_slot(*fresh);
        }
        current_heap = &ended_heap;
    }
    const LockScope scope(orphan_lock);
    void* const block = allocate_in(orphan_heap, class_index);
    return block;
}

// Claims an abandoned page for `heap` after a release into it, and keeps
// it: frees it if it is now empty, else queues it last.
void claim_into(Heap& heap, PageMeta& page) noexcept {
    if (!claim(page, heap.owner)) return;
    collect(page);
    {
        const LockScope scope(structure.lock);
        unlist(page);
    }
    if (page.live == 0) {
        free_page(page);
        return;
    }
    // A page of its own with its block still live has nothing to allocate
    // from: give it up again (it cannot be reached here by a release of its
    // one block, which leaves it empty).
    if (page.class_index >= class_count) {
        abandon(page, heap.owner);
        return;
    }
    link_last(heap.queues[page.class_index], page);
}

} // namespace

// ---- the interface -------------------------------------------------------------------

void integrity_stop(const Integrity what) noexcept {
    integrity_found.store(static_cast<u32>(what));
    os::stop();
}

bool ensure_started() noexcept {
    const u32 seen = structure.started.load(std::memory_order_acquire);
    if (seen == start_done) return true;
    if (seen == start_refused) return false;
    const LockScope scope(structure.lock);
    if (structure.started.load(std::memory_order_relaxed) != start_pending) {
        const bool done = structure.started.load(std::memory_order_relaxed) == start_done;
        return done;
    }
    const usize page = os::page_bytes();
    if (!std::has_single_bit(page) || page > slice_bytes) {
        structure.started.store(start_refused, std::memory_order_release);
        return false;
    }
    // The table's address space only; each slot is committed when first used.
    void* const table = os::reserve(heap_table_bytes, slice_bytes);
    if (table == nullptr) return false;
    structure.os_page = page;
    structure.heap_table = static_cast<Heap*>(table);
    structure.started.store(start_done, std::memory_order_release);
    return true;
}

void* refill(Heap& heap, const usize class_index) noexcept {
    if (heap.owner == owner_unbound) {
        void* const served = refill_unbound(heap, class_index);
        return served;
    }
    void* const block = refill_owned(heap, class_index);
    return block;
}

void* allocate_singleton(const usize bytes, const usize alignment) noexcept {
    if (!ensure_started()) return nullptr;
    const usize slices = round_up(bytes, slice_bytes) >> slice_shift;
    const usize alignment_slices = alignment <= slice_bytes ? 1 : alignment >> slice_shift;
    RunPlace run{};
    {
        const LockScope scope(structure.lock);
        run = take_run(slices, alignment_slices);
    }
    if (run.base == 0) return nullptr;
    // One live block, the whole page; abandoned from the start, so its
    // release claims it and finds it empty.
    const PagePlan plan{
        .slices = slices,
        .block_bytes = slices * slice_bytes,
        .divide_magic = 0,
        .capacity = 1,
        .words = 1,
        .class_index = singleton_class,
        .owner = owner_abandoned,
        .blocks_free = false,
    };
    const PageMeta& page = make_page(run, plan);
    void* const block = reinterpret_cast<void*>(area_of(place_of(page)));
    return block;
}

Released release_remote(Heap& heap, PageMeta& page, std::atomic<u64>& free_cell, std::atomic<u64>& remote_cell,
                        const u64 bit) noexcept {
    if ((free_cell.load(std::memory_order_relaxed) & bit) != 0) return Released::refused;
    const u64 before = remote_cell.fetch_or(bit);
    if ((before & bit) != 0) return Released::refused;
    const u32 state = page.state.fetch_or(pending_bit);
    if (owner_of_state(state) != owner_abandoned) return Released::released;
    if (heap.owner == owner_unbound) {
        const LockScope scope(orphan_lock);
        claim_into(orphan_heap, page);
        return Released::released;
    }
    claim_into(heap, page);
    return Released::released;
}

void release_emptied(Heap& heap, PageMeta& page) noexcept {
    const usize class_index = page.class_index;
    ClassEntry& entry = heap.entries[class_index];
    if (entry.page == &page) {
        // An empty one-slice page stays current, so a thread that allocates
        // and releases one block does not make and free a page each time.
        if (page.slices == small_page_slices) return;
        entry = ClassEntry{};
    }
    unlink(heap.queues[class_index], page);
    free_page(page);
}

void trim_heap(Heap& heap) noexcept {
    for (usize class_index = 0; class_index < class_count; ++class_index) {
        ClassQueue& queue = heap.queues[class_index];
        PageMeta* cursor = queue.first;
        while (cursor != nullptr) {
            PageMeta& page = *cursor;
            cursor = page.next;
            if ((page.state.load(std::memory_order_relaxed) & pending_bit) != 0) collect(page);
            if (page.live != 0) continue;
            if (heap.entries[class_index].page == &page) heap.entries[class_index] = ClassEntry{};
            unlink(queue, page);
            free_page(page);
        }
    }
    const LockScope scope(structure.lock);
    decommit_idle_units();
}

void retire_heap(Heap& heap) noexcept {
    for (usize class_index = 0; class_index < class_count; ++class_index) {
        heap.entries[class_index] = ClassEntry{};
        ClassQueue& queue = heap.queues[class_index];
        while (queue.first != nullptr) {
            PageMeta& page = *queue.first;
            unlink(queue, page);
            abandon(page, heap.owner);
        }
    }
    free_heap_slot(heap);
}

} // namespace stxlib::stx_minmallocpp
