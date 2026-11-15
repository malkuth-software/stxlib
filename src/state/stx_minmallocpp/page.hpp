#pragma once

// stx_minmallocpp: a page's metadata, the region tables that hold it, and
// the operations on one page's blocks.
//
// A region's metadata, by slice (each slice is 64 KiB; DESIGN.md 4.1):
//
//   0         RegionHeader (region.hpp)
//   1         slice_head: one u32 per slice. 0: the slice is not part of a
//             page. Otherwise 1 + the index of the page's first slice.
//   2..4      the free-run tables: length (u16), next and previous (u32)
//   5..20     PageMeta, 64 bytes per slice; a page's is at its first slice
//   21..148   free words, 64 per slice
//   149..276  remote words, 64 per slice
//   277..287  never committed: a guard between metadata and data
//   288..     data
//
// The host hands these tables over as zero pages. Zero is the empty value
// of every table (not a page; no owner; on no list; no run; no free
// block), so a new region needs no initialising pass. Assumption, stated
// once (proofs/REPORT.md): zero pages from the host are read as objects of
// the table types holding zero. The atomics' sizes and lock-freedom are
// checked below; that their zero representation is the value zero is part
// of the assumption.
//
// A page's blocks. Block i of a page is at area + i * block_bytes, where
// area is the page's first slice's start. Its state is two bits, both
// outside the page:
//   free[i]    set: the block is free and its owner knows it;
//   remote[i]  set: a thread that does not own the page released the
//              block, and no owner has merged that yet.
// Both clear: the block is live. Both set never holds; finding it means a
// block was released twice, and stops the process.
//
// The allocator never reads or writes a block's memory, free or live.
//
// Who may touch what:
//   - free words, summary, live, and the links while the page is in a heap
//     queue: the page's owner only. Free words are atomics read and written
//     relaxed, so that a thread that does not own the page may read one
//     (to refuse a second release) without a data race.
//   - remote words: anyone sets bits with an atomic OR; the owner takes a
//     word with an atomic exchange.
//   - state: below.
//   - listed_in, and the links while the page is on an abandoned list:
//     under the structure lock (region.hpp).
//   - slices, block_bytes, divide_magic, capacity, words, class_index: set
//     when the page is made, constant until it is freed.
//
// The links (next, previous) have one meaning: the page's neighbours on
// the one list it is on, which is its owner's class queue while it is
// owned and its class's abandoned list while listed_in says so.
//
// state = owner | (count << 48).
//
//   owner (48 bits)
//     0  no page here (what zero metadata holds); never claimable;
//     1  abandoned: a page with no owner, which any thread may claim;
//     2  the orphan heap (heap.hpp);
//     3  a thread freeing a page whose every block has been released;
//     4 and up: thread heaps. An identity is issued once and never again
//       (house style, section 3); at the last one a new thread is served
//       by the orphan heap instead.
//   count (16 bits), a hint that only ever makes someone look:
//     owned      pending_base minus the releases other threads have
//                announced since the owner last collected; so "owned and
//                nothing pending" is one value, owned_state(owner);
//     abandoned  the blocks not yet released, as far as announced: the
//                page's live count when it was abandoned, minus the
//                releases announced since. At 0 every block has been
//                released, and the thread that brought it there frees the
//                page.
//   The count can be off (a release announces after its bit was already
//   merged; a release announces to a page that was freed and made again in
//   between). It is never trusted: whoever acts on it first claims the
//   page and collects, and the bits decide.
//
// The protocol (heap.cpp) and why no release is lost: DESIGN.md 4.3.

#include "state/stx_minmallocpp/layout.hpp"
#include "state/stx_minmallocpp/os.hpp"

#include <atomic>
#include <bit>

namespace stxlib::stx_minmallocpp {

static_assert(sizeof(std::atomic<u32>) == sizeof(u32) && sizeof(std::atomic<u64>) == sizeof(u64),
              "stx_minmallocpp: atomics have the integer's size");
static_assert(std::atomic<u32>::is_always_lock_free && std::atomic<u64>::is_always_lock_free &&
                  std::atomic<uptr>::is_always_lock_free,
              "stx_minmallocpp: atomics are lock free");

// ---- checks ------------------------------------------------------------------------

// The allocator's entry and exit conditions, compiled in when
// STXLIB_STX_MINMALLOCPP_CHECKS is defined (the direct tests always; a
// Debug stxlib_state). A failed check records where and stops the process.
// The conditions are the ones the exit tables state (proofs/
// stx_minmallocpp_graph.md).
[[noreturn]] void check_failed(const char* file, int line) noexcept;

#if defined(STXLIB_STX_MINMALLOCPP_CHECKS)
#define STX_MINMALLOCPP_CHECK(condition)                                                         \
    ((condition) ? static_cast<void>(0) : ::stxlib::stx_minmallocpp::check_failed(__FILE__, __LINE__))
#else
#define STX_MINMALLOCPP_CHECK(condition) static_cast<void>(0)
#endif

// ---- steps -------------------------------------------------------------------------

// A mark before each operation on memory that another thread may touch at
// the same time (the atomic steps of proofs/stx_minmallocpp_graph.md,
// section 5). In every ordinary build it is nothing. In the interleaving
// checker's build (tests/stx_minmallocpp_interleave.cpp,
// STXLIB_STX_MINMALLOCPP_INTERLEAVE) it hands control to the checker, which
// decides which thread takes its next step, so that every ordering of the
// steps can be run through this code itself.
#if defined(STXLIB_STX_MINMALLOCPP_INTERLEAVE)
// The calling thread is about to take a step.
void interleave_step() noexcept;
// The calling thread cannot go on until another thread has taken a step
// (it found a lock held).
void interleave_blocked() noexcept;
#define STX_MINMALLOCPP_STEP() ::stxlib::stx_minmallocpp::interleave_step()
#else
#define STX_MINMALLOCPP_STEP() static_cast<void>(0)
#endif

// ---- the lock --------------------------------------------------------------------

// A spin lock for the allocator's structure (region.hpp) and the orphan
// heap (heap.cpp). Zero is unlocked. What a holder may do while holding
// each is stated where the lock is declared; a waiter's wait is the
// holder's bound, not a constant. Waiters yield, which is not a
// cancellation point. Not reentrant. Lock order, wherever both are held:
// the orphan lock first, then the structure lock.
struct Lock final {
    std::atomic<u32> held{0};
};

class LockScope final {
public:
    explicit LockScope(Lock& target) noexcept : target_(target) {
#if defined(STXLIB_STX_MINMALLOCPP_INTERLEAVE)
        // Taking the lock is a step; finding it held hands the turn on.
        for (;;) {
            interleave_step();
            if (target_.held.exchange(1, std::memory_order_acquire) == 0) break;
            interleave_blocked();
        }
#else
        while (target_.held.exchange(1, std::memory_order_acquire) != 0) {
            while (target_.held.load(std::memory_order_relaxed) != 0) {
                os::yield();
            }
        }
#endif
    }
    ~LockScope() { target_.held.store(0, std::memory_order_release); }
    LockScope(const LockScope&) = delete;
    LockScope& operator=(const LockScope&) = delete;

private:
    Lock& target_;
};

// ---- owners and state --------------------------------------------------------------

inline constexpr u64 owner_none = 0;
inline constexpr u64 owner_abandoned = 1;
inline constexpr u64 owner_orphan = 2;
inline constexpr u64 owner_freeing = 3;
inline constexpr u64 first_heap_owner = 4;
inline constexpr u64 owner_bits = 48;
inline constexpr u64 owner_mask = (static_cast<u64>(1) << owner_bits) - 1;
// The identity of a heap that owns nothing (heap.hpp: the unbound and the
// ended sentinels), and the end of the identities: no page ever names it,
// so "this page is mine" is false for every page such a heap looks at.
inline constexpr u64 owner_unbound = owner_mask;
inline constexpr u64 count_unit = static_cast<u64>(1) << owner_bits;
// An owned page's count with nothing pending. A page has at most 4,096
// blocks and each can be announced once between two collects, so the count
// stays above zero.
inline constexpr u64 pending_base = 8191;
static_assert(page_blocks_limit <= pending_base, "an owned page's count cannot reach zero");

[[nodiscard]] constexpr u64 owned_state(const u64 owner) noexcept {
    const u64 result = owner | (pending_base << owner_bits);
    return result;
}

[[nodiscard]] constexpr u64 abandoned_state(const u64 live) noexcept {
    const u64 result = owner_abandoned | (live << owner_bits);
    return result;
}

[[nodiscard]] constexpr u64 owner_of_state(const u64 state) noexcept {
    const u64 result = state & owner_mask;
    return result;
}

[[nodiscard]] constexpr u64 count_of_state(const u64 state) noexcept {
    const u64 result = state >> owner_bits;
    return result;
}

// ---- PageMeta ------------------------------------------------------------------------

struct alignas(64) PageMeta final {
    std::atomic<u64> state{0};
    // The block size, and ceil(2^42 / block_bytes) (1 for a page of one
    // block, which starts the page).
    u64 block_bytes{0};
    u64 divide_magic{0};
    // Blocks in the page, and how many of them are live as far as the owner
    // has merged.
    u32 capacity{0};
    u32 live{0};
    // Bit w clear: free word w is zero. A set bit promises nothing.
    u64 summary{0};
    // The page's length in slices.
    u32 slices{0};
    // The size class, or singleton_class.
    u16 class_index{0};
    // Bitmap words in use, 1 to 64.
    u8 words{0};
    // 0: on no abandoned list. Otherwise 1 + the class whose list holds it.
    u8 listed_in{0};
    // The page's neighbours on the one list it is on.
    PageMeta* next{nullptr};
    PageMeta* previous{nullptr};
};

static_assert(sizeof(PageMeta) == 64 && alignof(PageMeta) == 64, "stx_minmallocpp: PageMeta is one cache line");

// ---- the region's tables -----------------------------------------------------------

inline constexpr usize header_slice = 0;
inline constexpr usize slice_head_slice = 1;
inline constexpr usize run_length_slice = 2;
inline constexpr usize run_next_slice = 3;
inline constexpr usize run_previous_slice = 4;
inline constexpr usize page_meta_slice = 5;
inline constexpr usize free_words_slice = 21;
inline constexpr usize remote_words_slice = 149;
inline constexpr usize guard_slice = 277;
// The first data slice, on a unit boundary (288 = 18 units).
inline constexpr usize first_data_slice = 288;
// Slices committed when a region is made: 0 up to the free words.
inline constexpr usize eager_meta_slices = free_words_slice;

static_assert(region_slices * sizeof(u32) == slice_bytes, "slice_head fills one slice");
static_assert(region_slices * sizeof(PageMeta) == (free_words_slice - page_meta_slice) * slice_bytes, "PageMeta table");
static_assert(region_slices * page_words * sizeof(u64) == (remote_words_slice - free_words_slice) * slice_bytes,
              "free words table");
static_assert(region_slices * page_words * sizeof(u64) == (guard_slice - remote_words_slice) * slice_bytes,
              "remote words table");
static_assert(guard_slice < first_data_slice && first_data_slice % unit_slices == 0, "data starts on a unit");

// The region that holds `address`, which is inside one: its 1 GiB base.
[[nodiscard]] constexpr uptr region_base_of(const uptr address) noexcept {
    const uptr result = address & ~static_cast<uptr>(region_bytes - 1);
    return result;
}

[[nodiscard]] inline std::atomic<u32>* slice_heads(const uptr base) noexcept {
    std::atomic<u32>* const result = reinterpret_cast<std::atomic<u32>*>(base + slice_head_slice * slice_bytes);
    return result;
}

[[nodiscard]] inline PageMeta* page_metas(const uptr base) noexcept {
    PageMeta* const result = reinterpret_cast<PageMeta*>(base + page_meta_slice * slice_bytes);
    return result;
}

// The 64 free words, and the 64 remote words, of the page whose first slice
// is `head`.
[[nodiscard]] inline std::atomic<u64>* free_words(const uptr base, const usize head) noexcept {
    const uptr address = base + free_words_slice * slice_bytes + head * page_words * sizeof(u64);
    std::atomic<u64>* const result = reinterpret_cast<std::atomic<u64>*>(address);
    return result;
}

[[nodiscard]] inline std::atomic<u64>* remote_words(const uptr base, const usize head) noexcept {
    const uptr address = base + remote_words_slice * slice_bytes + head * page_words * sizeof(u64);
    std::atomic<u64>* const result = reinterpret_cast<std::atomic<u64>*>(address);
    return result;
}

// Where a page is, from its PageMeta's own address: the tables are at fixed
// places in the region, so nothing is stored.
struct PagePlace final {
    uptr base{0};
    usize head{0};
};

[[nodiscard]] inline PagePlace place_of(const PageMeta& page) noexcept {
    const uptr address = reinterpret_cast<uptr>(&page);
    const uptr base = region_base_of(address);
    const usize head = static_cast<usize>(address - base - page_meta_slice * slice_bytes) / sizeof(PageMeta);
    const PagePlace result{.base = base, .head = head};
    return result;
}

// The address of the page's first block.
[[nodiscard]] constexpr uptr area_of(const PagePlace place) noexcept {
    const uptr result = place.base + (static_cast<uptr>(place.head) << slice_shift);
    return result;
}

// ---- block arithmetic ---------------------------------------------------------------

// The index of the block that contains byte `offset` of a page:
// floor(offset / block_bytes), without a division. Exact for
// offset < 2^22 and 16 <= block_bytes <= 2^19: with
// magic = ceil(2^42 / block_bytes), offset * (magic - 2^42 / block_bytes)
// < 2^22 <= 2^42 / block_bytes, and offset * magic < 2^61. A page of one
// block has magic 1 and offsets below 2^25, so its index is 0 for every
// offset.
[[nodiscard]] constexpr u64 block_index(const u64 offset, const u64 divide_magic) noexcept {
    const u64 result = (offset * divide_magic) >> divide_shift;
    return result;
}

// Whether a byte offset is a block's first byte, from the same product
// `scaled` = offset * magic: the low 42 bits are below the magic exactly
// when block_bytes divides the offset. Why, with magic * block_bytes =
// 2^42 + r, 0 <= r < block_bytes, and offset = q * block_bytes + s:
// scaled = q * 2^42 + q * r + s * magic. For s = 0 the low bits are q * r,
// below 2^22 and so below the magic (at least 2^23). For s > 0 they are at
// least the magic and cannot wrap past 2^42 back below it, because
// q * r + s * magic < 2^42 + (q + 1) * r and (q + 1) * r is below the
// magic. A page of one block (magic 1): the low bits are the offset, below
// 1 only at 0. tests/stx_minmallocpp_tests.cpp checks it against the
// remainder for every class and every offset of its page.
inline constexpr u64 divide_mask = (static_cast<u64>(1) << divide_shift) - 1;

[[nodiscard]] constexpr bool block_starts_at(const u64 scaled, const u64 divide_magic) noexcept {
    const bool result = (scaled & divide_mask) < divide_magic;
    return result;
}

// ---- the owner's operations on one page ------------------------------------------

// What the allocator found contradicted before it stopped the process;
// kept for a debugger or a core file (integrity_stop, heap.cpp).
enum class Integrity : u32 {
    whole = 0,
    // A remote bit and a free bit name the same block: released twice.
    released_twice = 1,
    // A page being made found remote bits left by a release of a block of
    // the page that was there before.
    released_into_freed_page = 2,
    // More blocks were merged than were live.
    live_count = 3,
    // An entry or exit check failed (check_failed).
    check = 4,
};

[[noreturn]] void integrity_stop(Integrity what) noexcept;

// The owner's view of a page is whole: every block is free or counted
// live, and the summary covers every non-zero free word. At most 64 words.
// For the checks; the owner only.
[[nodiscard]] inline bool page_whole(const PageMeta& page) noexcept {
    const PagePlace place = place_of(page);
    const std::atomic<u64>* const free_cells = free_words(place.base, place.head);
    usize free_blocks = 0;
    bool covered = true;
    for (usize word = 0; word < page.words; ++word) {
        const u64 bits = free_cells[word].load(std::memory_order_relaxed);
        free_blocks = free_blocks + static_cast<usize>(std::popcount(bits));
        if (bits != 0 && ((page.summary >> word) & 1) == 0) covered = false;
    }
    const bool result = covered && free_blocks + page.live == page.capacity;
    return result;
}

// Merges every release other threads made into the page's free words. The
// caller owns the page as `owner`. The state is set back to "nothing
// pending" first, so a release that sets its bit after its word is taken
// announces again and is merged by a later collect. At most 64 words.
inline void collect(PageMeta& page, const u64 owner) noexcept {
    STX_MINMALLOCPP_CHECK(owner_of_state(page.state.load()) == owner);
    STX_MINMALLOCPP_STEP();
    page.state.store(owned_state(owner));
    const PagePlace place = place_of(page);
    std::atomic<u64>* const free_cells = free_words(place.base, place.head);
    std::atomic<u64>* const remote_cells = remote_words(place.base, place.head);
    const usize words = page.words;
    for (usize word = 0; word < words; ++word) {
        STX_MINMALLOCPP_STEP();
        if (remote_cells[word].load(std::memory_order_relaxed) == 0) continue;
        STX_MINMALLOCPP_STEP();
        const u64 released = remote_cells[word].exchange(0);
        const u64 known = free_cells[word].load(std::memory_order_relaxed);
        if ((known & released) != 0) integrity_stop(Integrity::released_twice);
        const u32 count = static_cast<u32>(std::popcount(released));
        if (count > page.live) integrity_stop(Integrity::live_count);
        free_cells[word].store(known | released, std::memory_order_relaxed);
        page.summary = page.summary | (static_cast<u64>(1) << word);
        page.live = page.live - count;
    }
    STX_MINMALLOCPP_CHECK(page_whole(page));
}

// ---- the empty sentinels -----------------------------------------------------------

// What a heap's class entry names when the class has no current page: a
// word that is always zero, so the fast path falls to the slow path with no
// test of its own, and a page nobody writes (the fast path writes a page
// only after it found a non-zero word).
inline constinit std::atomic<u64> empty_word{0};
inline constinit PageMeta empty_page{};

} // namespace stxlib::stx_minmallocpp
