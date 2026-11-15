// stx_minmallocpp: thread heaps and the slow paths (heap.hpp).
//
// The ownership protocol, in one place (page.hpp gives the state word;
// DESIGN.md 4.3 argues why no release is lost; proofs/REPORT.md lists it as
// not yet proved):
//
//   owner releases a block      free bit, summary bit, live - 1 (allocator.cpp)
//   non-owner releases a block  remote bit (atomic OR), then announce: one
//                               count off the state (atomic subtract),
//                               unless an owner already has releases
//                               pending (release_remote)
//   collect                     owner: state back to "nothing pending", then
//                               every remote word exchanged for 0 and merged
//                               (page.hpp)
//   abandon                     owner: compare-exchange state from "mine,
//                               nothing pending" to "abandoned, live blocks";
//                               if something was pending, collect and try
//                               again (abandon, refill_owned)
//   claim                       compare-exchange state from abandoned to
//                               "mine, nothing pending", then collect
//                               (claim, and its callers)
//
// A page's owner-only fields are touched only between a successful claim
// (or the page's making) and the abandon or free that ends that ownership.
// The count in the state is a hint: a thread acts on it by claiming and
// collecting, and decides from the bits.
//
// The abandoned lists (Structure::abandoned_first) hold abandoned pages
// that have, or had when listed, a free block. PageMeta::listed_in and the
// links of a listed page are written only under the structure lock. Every
// claim is followed, before the claimer uses the links, by unlist under
// that lock; a thread that lists a page checks, under the lock, that the
// page is abandoned and not listed. So a page is listed at most once and
// an owned page is unlisted before its owner links it. A list may hold a
// page that has since been claimed, freed, made again and abandoned in
// another shape; an adopter checks what it claimed and abandons it again if
// it is not what it wanted.
//
// Locks. The orphan lock guards the orphan heap; its holder runs a whole
// allocation or trim of that heap. The structure lock is taken inside it,
// never the other way round.

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
// Where a failed check was (check_failed): the file name's address and the
// line, for a debugger or a core file.
constinit std::atomic<uptr> check_file{0};
constinit std::atomic<u32> check_line{0};

constexpr usize heap_table_bytes = (heap_capacity * sizeof(Heap) + slice_bytes - 1) / slice_bytes * slice_bytes;

// ---- class queues ----------------------------------------------------------------

// Puts a page the heap owns, in no list, first (or last) in its class queue.
void link_first(ClassQueue& queue, PageMeta& page) noexcept {
    page.previous = nullptr;
    page.next = queue.first;
    if (queue.first != nullptr) {
        (*queue.first).previous = &page;
    } else {
        queue.last = &page;
    }
    queue.first = &page;
    queue.pages = queue.pages + 1;
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
    queue.pages = queue.pages + 1;
}

// Takes a page out of the queue it is in.
void unlink(ClassQueue& queue, PageMeta& page) noexcept {
    STX_MINMALLOCPP_CHECK(queue.pages != 0);
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
    queue.pages = queue.pages - 1;
}

// ---- the abandoned lists -----------------------------------------------------------

// Takes a page off the abandoned list it is on, if any. The structure lock
// is held.
void unlist(PageMeta& page) noexcept {
    if (page.listed_in == 0) return;
    const usize class_index = static_cast<usize>(page.listed_in) - 1;
    if (page.previous != nullptr) {
        (*page.previous).next = page.next;
    } else {
        structure.abandoned_first[class_index] = page.next;
    }
    if (page.next != nullptr) (*page.next).previous = page.previous;
    page.next = nullptr;
    page.previous = nullptr;
    page.listed_in = 0;
    static_cast<void>(structure.abandoned_count[class_index].fetch_sub(1, std::memory_order_relaxed));
}

// Lists a page for adoption if it is abandoned and not listed. The caller
// does not own the page: only the state (atomic) and the lock-guarded
// fields are read. Takes the structure lock.
void list_abandoned(PageMeta& page, const usize class_index) noexcept {
    const LockScope scope(structure.lock);
    if (page.listed_in != 0) return;
    if (owner_of_state(page.state.load()) != owner_abandoned) return;
    page.previous = nullptr;
    page.next = structure.abandoned_first[class_index];
    if (page.next != nullptr) (*page.next).previous = &page;
    structure.abandoned_first[class_index] = &page;
    page.listed_in = static_cast<u8>(class_index + 1);
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
    // The page's first state: owned by its maker, or abandoned.
    u64 state{0};
    // True: every block starts free. False: every block starts live.
    bool blocks_free{false};
};

// `count` slices from the index, under the structure lock (region.hpp,
// take_run).
[[nodiscard]] RunPlace take_run_locked(const usize count, const usize alignment_slices) noexcept {
    const LockScope scope(structure.lock);
    const RunPlace run = take_run(count, alignment_slices);
    return run;
}

// Makes the page at `run`: its words, its PageMeta, then its state and its
// slice_head entries, which publish it. The run is the caller's alone, so
// nothing here needs the lock. listed_in is 0 already (a freed page was
// unlisted; fresh metadata is zero). A remote word left non-zero means a
// block of the page that was here before was released after that page was
// freed.
[[nodiscard]] PageMeta& make_page(const RunPlace run, const PagePlan& plan) noexcept {
    STX_MINMALLOCPP_CHECK(run.slice >= first_data_slice && run.slice + plan.slices <= region_slices);
    STX_MINMALLOCPP_CHECK(plan.words >= 1 && plan.words <= page_words && plan.capacity <= plan.words * word_bits);
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
    page.block_bytes = plan.block_bytes;
    page.divide_magic = plan.divide_magic;
    page.capacity = static_cast<u32>(plan.capacity);
    page.live = plan.blocks_free ? 0 : static_cast<u32>(plan.capacity);
    page.summary = 0;
    if (plan.blocks_free) {
        page.summary = plan.words >= word_bits ? u64_max : (static_cast<u64>(1) << plan.words) - 1;
    }
    page.slices = static_cast<u32>(plan.slices);
    page.class_index = static_cast<u16>(plan.class_index);
    page.words = static_cast<u8>(plan.words);
    page.next = nullptr;
    page.previous = nullptr;
    page.state.store(plan.state);
    std::atomic<u32>* const heads = slice_heads(run.base);
    for (usize slice = 0; slice < plan.slices; ++slice) {
        heads[run.slice + slice].store(static_cast<u32>(slice + 1), std::memory_order_release);
    }
    return page;
}

// Frees a page its caller owns, with no live block and in no heap queue:
// unpublishes its slices, marks its state "no page", and gives its run
// back. Proportional to the page's slices. Takes the structure lock.
void free_page(PageMeta& page) noexcept {
    STX_MINMALLOCPP_CHECK(page.live == 0 && page_whole(page));
    STX_MINMALLOCPP_CHECK(owner_of_state(page.state.load()) > owner_abandoned);
    const PagePlace place = place_of(page);
    const usize slices = page.slices;
    std::atomic<u32>* const heads = slice_heads(place.base);
    const LockScope scope(structure.lock);
    unlist(page);
    for (usize slice = 0; slice < slices; ++slice) {
        heads[place.head + slice].store(0, std::memory_order_release);
    }
    page.state.store(owner_none);
    give_run(place.base, place.head, slices);
}

// ---- claim and abandon -------------------------------------------------------------

// Makes `owner` the owner of an abandoned page; false if the page has an
// owner. The new state has nothing pending: the claimer collects next.
[[nodiscard]] bool claim(PageMeta& page, const u64 owner) noexcept {
    u64 seen = page.state.load();
    while (owner_of_state(seen) == owner_abandoned) {
        if (page.state.compare_exchange_weak(seen, owned_state(owner))) return true;
    }
    return false;
}

// Gives up a page `owner` owns and has in no queue. Frees it if nothing in
// it is live; otherwise abandons it, and lists it if it has a free block.
// Repeats only while other threads keep releasing into it.
void abandon(PageMeta& page, const u64 owner) noexcept {
    for (;;) {
        if (page.state.load(std::memory_order_relaxed) != owned_state(owner)) collect(page, owner);
        if (page.live == 0) {
            free_page(page);
            return;
        }
        const bool has_space = page.live < page.capacity;
        const usize class_index = page.class_index;
        u64 expected = owned_state(owner);
        if (page.state.compare_exchange_strong(expected, abandoned_state(page.live))) {
            // The page is no longer ours: only list_abandoned's checks from here.
            if (has_space && class_index < class_count) list_abandoned(page, class_index);
            return;
        }
    }
}

// Unlists a page its caller has just claimed. Takes the structure lock.
void unlist_claimed(PageMeta& page) noexcept {
    const LockScope scope(structure.lock);
    unlist(page);
}

// A release brought an abandoned page's count to zero: every block it had
// live has been released. Claims it for no heap, merges, and frees it; if
// the count was off and a block is still live, abandons it again with the
// true count.
void free_released(PageMeta& page) noexcept {
    if (!claim(page, owner_freeing)) return;
    collect(page, owner_freeing);
    unlist_claimed(page);
    abandon(page, owner_freeing);
}

// The heap's oldest page of a class, if it is not the current one: merges
// what other threads released into it and frees it if it is empty. So a
// page the heap holds but does not visit does not stay once it is fully
// released and the heap claims or takes another. At most 64 words.
void sweep_oldest(Heap& heap, const usize class_index) noexcept {
    ClassQueue& queue = heap.queues[class_index];
    PageMeta* const oldest = queue.last;
    if (oldest == nullptr || oldest == heap.entries[class_index].page) return;
    PageMeta& page = *oldest;
    if (page.state.load(std::memory_order_relaxed) != owned_state(heap.owner)) collect(page, heap.owner);
    if (page.live != 0) return;
    unlink(queue, page);
    free_page(page);
}

// Claims an abandoned page for `heap` after a release into it, and keeps
// it for the heap's later releases: frees it if it is now empty, gives it
// up again if it has no free block or is not a class page, else queues it
// last.
void claim_into(Heap& heap, PageMeta& page) noexcept {
    if (!claim(page, heap.owner)) return;
    collect(page, heap.owner);
    unlist_claimed(page);
    if (page.live == 0 || page.live == page.capacity || page.class_index >= class_count) {
        abandon(page, heap.owner);
        return;
    }
    sweep_oldest(heap, page.class_index);
    link_last(heap.queues[page.class_index], page);
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

// The first page of the class's abandoned list, taken off it and claimed
// for `owner`; null when the list is empty or that page has an owner again.
[[nodiscard]] PageMeta* pop_and_claim(const usize class_index, const u64 owner) noexcept {
    const LockScope scope(structure.lock);
    PageMeta* const candidate = structure.abandoned_first[class_index];
    if (candidate == nullptr) return nullptr;
    unlist(*candidate);
    if (!claim(*candidate, owner)) return nullptr;
    return candidate;
}

// An abandoned page of the class with a free block, claimed and linked
// first in the heap's queue; null when there is none within
// adopt_attempts pops.
[[nodiscard]] PageMeta* adopt(Heap& heap, const usize class_index) noexcept {
    for (u32 attempt = 0; attempt < adopt_attempts; ++attempt) {
        if (structure.abandoned_count[class_index].load(std::memory_order_relaxed) == 0) return nullptr;
        PageMeta* const claimed = pop_and_claim(class_index, heap.owner);
        if (claimed == nullptr) continue;
        PageMeta& page = *claimed;
        collect(page, heap.owner);
        if (page.live == 0 || page.class_index != class_index || page.live == page.capacity) {
            abandon(page, heap.owner);
            continue;
        }
        link_first(heap.queues[class_index], page);
        return claimed;
    }
    return nullptr;
}

// A new page of the class, every block free, linked first in the heap's
// queue; null when the host refuses.
[[nodiscard]] PageMeta* fresh_page(Heap& heap, const usize class_index) noexcept {
    const ClassShape& shape = class_table.shapes[class_index];
    const RunPlace run = take_run_locked(shape.page_slices, 1);
    if (run.base == 0) return nullptr;
    const PagePlan plan{
        .slices = shape.page_slices,
        .block_bytes = shape.block_bytes,
        .divide_magic = shape.divide_magic,
        .capacity = shape.capacity,
        .words = shape.words,
        .class_index = class_index,
        .state = owned_state(heap.owner),
        .blocks_free = true,
    };
    PageMeta& page = make_page(run, plan);
    link_first(heap.queues[class_index], page);
    return &page;
}

// The slow path for a heap that owns pages (a thread heap, or the orphan
// heap under its lock). Each turn of the loop returns, abandons one full
// page, or was caused by another thread's release into the current page.
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
            if (page.state.load(std::memory_order_relaxed) != owned_state(heap.owner)) {
                collect(page, heap.owner);
                if (page.summary != 0) continue;
            }
            // Full. Take it out of the queue first: once abandoned, its
            // links are the next owner's.
            STX_MINMALLOCPP_CHECK(page.live == page.capacity);
            unlink(queue, page);
            entry = ClassEntry{};
            u64 expected = owned_state(heap.owner);
            if (!page.state.compare_exchange_strong(expected, abandoned_state(page.live))) {
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

// A block of the class from a heap that owns pages: the fast path, then
// the slow one.
[[nodiscard]] void* allocate_in(Heap& heap, const usize class_index) noexcept {
    void* const ready = take_block(heap.entries[class_index]);
    if (ready != nullptr) return ready;
    void* const block = refill_owned(heap, class_index);
    return block;
}

// ---- heap slots ----------------------------------------------------------------------

// A heap slot, constructed, with a new owner identity; null when the table
// is full, the identities are used up, or the host refuses to commit a new
// slot. Takes the structure lock.
[[nodiscard]] Heap* acquire_heap() noexcept {
    const LockScope scope(structure.lock);
    if (structure.next_heap_owner == owner_unbound) return nullptr;
    usize slot = 0;
    if (structure.free_heap_count != 0) {
        slot = structure.free_heap_slots[structure.free_heap_count - 1];
    } else if (structure.heap_slots_made < heap_capacity) {
        slot = structure.heap_slots_made;
        const uptr start = reinterpret_cast<uptr>(structure.heap_table) + slot * sizeof(Heap);
        const uptr page_mask = static_cast<uptr>(structure.os_page - 1);
        const uptr commit_start = start & ~page_mask;
        const uptr commit_end = (start + sizeof(Heap) + page_mask) & ~page_mask;
        if (!os::commit(reinterpret_cast<void*>(commit_start), static_cast<usize>(commit_end - commit_start))) return nullptr;
    } else {
        return nullptr;
    }
    if (structure.free_heap_count != 0) {
        structure.free_heap_count = structure.free_heap_count - 1;
    } else {
        structure.heap_slots_made = structure.heap_slots_made + 1;
    }
    const uptr address = reinterpret_cast<uptr>(structure.heap_table) + slot * sizeof(Heap);
    Heap* const made = new (reinterpret_cast<void*>(address)) Heap{};
    (*made).owner = structure.next_heap_owner;
    structure.next_heap_owner = structure.next_heap_owner + 1;
    return made;
}

// Returns a heap that owns no page to the free slots. Takes the structure
// lock.
void free_heap_slot(Heap& heap) noexcept {
    const usize slot =
        static_cast<usize>(reinterpret_cast<uptr>(&heap) - reinterpret_cast<uptr>(structure.heap_table)) / sizeof(Heap);
    const LockScope scope(structure.lock);
    structure.free_heap_slots[structure.free_heap_count] = static_cast<u16>(slot);
    structure.free_heap_count = structure.free_heap_count + 1;
}

// The slow path for a thread whose pointer names a sentinel. A thread the
// host says has ended, or that can get no heap, is served from the orphan
// heap. A thread that armed a heap whose pointer the host has since
// dropped (os.hpp, armed_context) gets that heap back. Any other unbound
// thread gets a new heap and its exit hook.
[[nodiscard]] void* refill_unbound(Heap& given, const usize class_index) noexcept {
    if (!ensure_started()) return nullptr;
    if (&given == &unbound_heap && !os::thread_has_ended()) {
        void* const armed = os::armed_context();
        if (armed != nullptr) {
            Heap* const mine = static_cast<Heap*>(armed);
            current_heap = mine;
            void* const block = allocate_in(*mine, class_index);
            return block;
        }
        Heap* const fresh = acquire_heap();
        if (fresh != nullptr) {
            if (os::arm_thread_exit(fresh)) {
                current_heap = fresh;
                void* const block = allocate_in(*fresh, class_index);
                return block;
            }
            free_heap_slot(*fresh);
        }
    }
    current_heap = &ended_heap;
    const LockScope scope(orphan_lock);
    void* const block = allocate_in(orphan_heap, class_index);
    sweep_oldest(orphan_heap, class_index);
    return block;
}

// Whether a release into an abandoned page should claim the page for the
// heap: the heap is allocating that class and holds few pages of it.
[[nodiscard]] bool keeps(const Heap& heap, const usize class_index) noexcept {
    const bool result = heap.owner != owner_unbound && heap.entries[class_index].page != &empty_page &&
                        heap.queues[class_index].pages < keep_limit;
    return result;
}

} // namespace

// ---- the interface -------------------------------------------------------------------

void integrity_stop(const Integrity what) noexcept {
    integrity_found.store(static_cast<u32>(what));
    os::stop();
}

void check_failed(const char* const file, const int line) noexcept {
    check_file.store(reinterpret_cast<uptr>(file));
    check_line.store(static_cast<u32>(line));
    integrity_stop(Integrity::check);
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
    STX_MINMALLOCPP_CHECK(bytes >= 1 && bytes <= huge_threshold && alignment <= huge_threshold);
    if (!ensure_started()) return nullptr;
    const usize slices = round_up(bytes, slice_bytes) >> slice_shift;
    const usize alignment_slices = alignment <= slice_bytes ? 1 : alignment >> slice_shift;
    const RunPlace run = take_run_locked(slices, alignment_slices);
    if (run.base == 0) return nullptr;
    // One live block, the whole page; abandoned from the start with a count
    // of one, so its release frees it.
    const PagePlan plan{
        .slices = slices,
        .block_bytes = slices * slice_bytes,
        .divide_magic = 0,
        .capacity = 1,
        .words = 1,
        .class_index = singleton_class,
        .state = abandoned_state(1),
        .blocks_free = false,
    };
    const PageMeta& page = make_page(run, plan);
    void* const block = reinterpret_cast<void*>(area_of(place_of(page)));
    return block;
}

Release release_remote(Heap& heap, PageMeta& page, std::atomic<u64>& free_cell, std::atomic<u64>& remote_cell,
                       const u64 bit) noexcept {
    // The page's constants are read while the block is still live: until
    // this release is merged the page cannot be freed.
    const u64 capacity = page.capacity;
    const usize class_index = page.class_index;
    if ((free_cell.load(std::memory_order_relaxed) & bit) != 0) return Release::refused;
    const u64 before = remote_cell.fetch_or(bit);
    if ((before & bit) != 0) return Release::refused;
    // Announce. An owner that already has releases pending will collect
    // before it abandons, and that collect reads the bit just set.
    const u64 seen = page.state.load();
    if (owner_of_state(seen) > owner_abandoned && count_of_state(seen) != pending_base) return Release::released;
    const u64 prior = page.state.fetch_sub(count_unit);
    if (owner_of_state(prior) != owner_abandoned) return Release::released;
    // The page was abandoned, with `remaining` blocks not yet released
    // before this one.
    const u64 remaining = count_of_state(prior);
    if (remaining == 1) {
        free_released(page);
        return Release::released;
    }
    if (class_index >= class_count) return Release::released;
    if (keeps(heap, class_index)) {
        claim_into(heap, page);
        return Release::released;
    }
    // It was full and now has a free block: make it adoptable.
    if (remaining == capacity) list_abandoned(page, class_index);
    return Release::released;
}

void release_emptied(Heap& heap, PageMeta& page) noexcept {
    const usize class_index = page.class_index;
    ClassEntry& entry = heap.entries[class_index];
    if (entry.page == &page) {
        // An empty small or medium page stays current, so a thread that
        // allocates and releases one block does not make and free a page
        // each time. A large page (4 MiB) goes back at once.
        if (page.slices <= medium_page_slices) return;
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
            if (page.state.load(std::memory_order_relaxed) != owned_state(heap.owner)) collect(page, heap.owner);
            if (page.live != 0) continue;
            if (heap.entries[class_index].page == &page) heap.entries[class_index] = ClassEntry{};
            unlink(queue, page);
            free_page(page);
        }
    }
    const LockScope scope(structure.lock);
    decommit_idle_units();
}

void trim_orphan() noexcept {
    if (!ensure_started()) return;
    const LockScope scope(orphan_lock);
    trim_heap(orphan_heap);
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
