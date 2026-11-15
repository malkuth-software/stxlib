#pragma once

// stx_minmallocpp: the process-wide structure. The directory of our
// address space, the regions, the index of free slice runs, commit units,
// huge mappings, and the abandoned-page lists.
//
// Everything in Structure is guarded by Structure::lock unless it is an
// atomic. What a holder does while holding it: table edits of a fixed
// number of steps, and host calls (os.hpp: none can unwind or be
// cancelled), which are: three commits per contiguous range of
// uncommitted units a run touches; one decommit per contiguous range of
// units that go idle past the limit; one region reservation with one
// commit when no run fits; and, in trim only, one decommit per idle unit
// of every region. Every table is whole whenever control leaves this
// module.
//
// Bounded costs (the owner, 2026-11-15: no slow path with an unpredictable
// cost). Free runs of slices are filed by length in a two-level segregated
// index (TLSF: Masmano, Ripoll, Crespo and Real, "TLSF: a new dynamic
// memory allocator for real-time systems", ECRTS 2004). Finding a run that
// fits, splitting it, and coalescing a freed run with its neighbours each
// take a fixed number of steps, whatever the heap's history:
//
//   first level   floor(log2(length)), 0 to 13 (a run has 1 to 16,096
//                 slices);
//   second level  for lengths of 4 and more, the next two bits of the
//                 length (4 lists); below that one list per length (1; 2
//                 and 3).
//
// A request is rounded up to the next list's smallest length, so any run in
// the list found is long enough and no list is searched. Pages of 1, 8 and
// 64 slices are exactly a list's smallest length, so they lose nothing.
//
// Boundary tags. run_length[s] is the run's length at the first and at the
// last slice of every free run, and 0 at the first and last slice of every
// allocated run (take_run clears them). Free runs are always coalesced, so
// "the slice before mine has a non-zero tag" is exactly "a free run ends
// there". Tags inside a run mean nothing and are never read.
//
// Commit units. A unit (1 MiB of data) is committed when its first slice
// is allocated. When its last slice is freed it stays committed, as an
// idle unit, unless the region already has idle_unit_limit of them; then
// it is decommitted at once. No clock and no schedule: the decision is made
// under the lock at the moment the unit goes idle.

#include "state/stx_minmallocpp/page.hpp"

namespace stxlib::stx_minmallocpp {

struct Heap;

// ---- the directory ---------------------------------------------------------------

// A cell's value: not ours; a standard region occupying exactly this GiB;
// or cell_huge_first + the index of the huge mapping that covers it.
inline constexpr u32 cell_none = 0;
inline constexpr u32 cell_region = 1;
inline constexpr u32 cell_huge_first = 2;

// One cell per GiB below 2^48. Written with release after what it names is
// ready; read with acquire. The cells are plain integers reached through
// std::atomic_ref: an array of 262,144 std::atomic objects costs the
// compiler one constructor evaluation per element, which passes Clang's
// limit for a constant initialiser with libc++, and the array must be
// constant-initialised (a run-time initialiser could zero cells already
// set).
extern constinit u32 directory[directory_cells];

static_assert(std::atomic_ref<u32>::is_always_lock_free && std::atomic_ref<u32>::required_alignment <= alignof(u32),
              "stx_minmallocpp: a directory cell is a lock-free atomic in place");

[[nodiscard]] inline u32 directory_cell(const usize index) noexcept {
    const std::atomic_ref<u32> cell(directory[index]);
    const u32 result = cell.load(std::memory_order_acquire);
    return result;
}

inline void set_directory_cell(const usize index, const u32 value) noexcept {
    const std::atomic_ref<u32> cell(directory[index]);
    cell.store(value, std::memory_order_release);
}

// ---- regions ---------------------------------------------------------------------

inline constexpr usize region_capacity = 1024; // 1 TiB of regions
inline constexpr u32 idle_unit_limit = 8;

// Slice 0 of a region. It is never constructed: zero pages from the host
// are its initial value, which is what the initialisers below say.
struct RegionHeader final {
    // Allocated slices in each unit, 0 to 16.
    u8 unit_used[region_units]{};
    // Bit set: the unit's data is committed.
    u64 unit_committed[region_units / word_bits]{};
    // Committed units with no slice allocated.
    u32 idle_units{0};
    // This region's index in Structure::region_bases.
    u32 index{0};
};

static_assert(sizeof(RegionHeader) <= slice_bytes, "the region header fits its slice");

[[nodiscard]] inline RegionHeader& header_of(const uptr base) noexcept {
    RegionHeader* const header = reinterpret_cast<RegionHeader*>(base + header_slice * slice_bytes);
    return *header;
}

// ---- free runs (TLSF) ----------------------------------------------------------------

inline constexpr usize run_first_levels = 14;
inline constexpr usize run_second_levels = 4;

// A free run's name in the index: (region index << 14) | first slice. A
// data slice is never slice 0, so 0 names no run.
inline constexpr u32 no_run = 0;
inline constexpr usize run_slice_bits = 14;
static_assert((static_cast<usize>(1) << run_slice_bits) == region_slices, "a run name holds a slice index");

struct RunList final {
    usize first{0};
    usize second{0};
};

// The list that holds free runs of `length` slices. Requires
// 1 <= length < 2^14.
[[nodiscard]] constexpr RunList run_list_of(const usize length) noexcept {
    const usize first = static_cast<usize>(std::bit_width(length)) - 1;
    usize second = 0;
    if (first < 2) {
        second = length - (static_cast<usize>(1) << first);
    } else {
        second = (length >> (first - 2)) & 3;
    }
    const RunList result{.first = first, .second = second};
    return result;
}

// The smallest length a list holds.
[[nodiscard]] constexpr usize run_list_floor(const RunList list) noexcept {
    if (list.first < 2) {
        const usize exact = (static_cast<usize>(1) << list.first) + list.second;
        return exact;
    }
    const usize result = (4 + list.second) << (list.first - 2);
    return result;
}

static_assert(run_list_floor(run_list_of(1)) == 1 && run_list_floor(run_list_of(8)) == 8 &&
                  run_list_floor(run_list_of(64)) == 64,
              "page lengths are a list's smallest length");

// A run of slices: `base` is its region, 0 when there is none.
struct RunPlace final {
    uptr base{0};
    usize slice{0};
};

// ---- huge mappings -------------------------------------------------------------------

inline constexpr usize huge_capacity = 4096;

// One block with a mapping to itself. `block` is the block's address while
// it is live and 0 otherwise; a release takes it with a compare-exchange,
// so a second release is refused.
struct HugeRecord final {
    std::atomic<uptr> block{0};
    usize reserved_bytes{0};
};

// ---- heaps ---------------------------------------------------------------------------

// Thread heaps alive at once (heap.hpp). A thread beyond this is served by
// the orphan heap.
inline constexpr usize heap_capacity = 1024;

// ---- the structure -------------------------------------------------------------------

inline constexpr u32 start_pending = 0;
inline constexpr u32 start_done = 1;
inline constexpr u32 start_refused = 2;

struct Structure final {
    Lock lock{};
    // start_pending until the first slow path; then start_done, or
    // start_refused when the host's page size is not usable.
    std::atomic<u32> started{start_pending};
    usize os_page{0};

    usize region_count{0};
    uptr region_bases[region_capacity]{};

    // The free-run index: bit f of run_first_level set when some list of
    // first level f is not empty; bit s of run_second_level[f] set when list
    // (f, s) is not empty; run_heads the first run of each list.
    u32 run_first_level{0};
    u8 run_second_level[run_first_levels]{};
    u32 run_heads[run_first_levels][run_second_levels]{};

    // Abandoned pages that have free blocks, by class, linked through
    // PageMeta::next and previous (heap.cpp). abandoned_count mirrors each
    // list's length so that an empty list costs no lock.
    PageMeta* abandoned_first[class_count]{};
    std::atomic<u32> abandoned_count[class_count]{};

    // Thread heaps (heap.cpp): the table's reservation, how many slots were
    // ever committed, the free slots (a stack of slot indices), and the
    // next owner identity. An identity is issued once and never again; at
    // owner_unbound there are no more, and a new thread gets no heap.
    Heap* heap_table{nullptr};
    usize heap_slots_made{0};
    u16 free_heap_slots[heap_capacity]{};
    usize free_heap_count{0};
    u64 next_heap_owner{first_heap_owner};

    // Huge mappings: the records, how many were ever used, and the free
    // ones (a stack of record indices).
    HugeRecord huge[huge_capacity]{};
    usize huge_made{0};
    u16 free_huge_records[huge_capacity]{};
    usize free_huge_count{0};
};

extern constinit Structure structure;

// ---- operations ----------------------------------------------------------------------

// Takes `count` slices whose first slice index is a multiple of
// `alignment_slices` (a power of two; 1 for no alignment), committing the
// units they touch and making a new region if no run fits. The structure
// lock is held. A RunPlace with base 0 when the host refuses memory or
// address space. A fixed number of table steps plus count / 16 + 1 unit
// steps, and three host commits per contiguous range of uncommitted units
// (one range for a run in fresh address space).
[[nodiscard]] RunPlace take_run(usize count, usize alignment_slices) noexcept;

// Gives back a run taken by take_run, whole. The structure lock is held.
// A fixed number of steps per unit touched, plus one host decommit per
// contiguous range of units that go idle past the limit.
void give_run(uptr base, usize slice, usize count) noexcept;

// Decommits every idle unit of every region. The structure lock is held.
// Proportional to the number of regions and their idle units; called only
// by trim.
void decommit_idle_units() noexcept;

// A block of `bytes` in a mapping of its own, aligned to 1 GiB; null when
// the host refuses or the records are all in use. Takes the lock.
[[nodiscard]] void* allocate_huge(usize bytes) noexcept;

// Releases the huge block that directory cell value `cell` names, if
// `address` is its start and it is live; false otherwise. Takes the lock.
[[nodiscard]] bool release_huge(u32 cell, uptr address) noexcept;

} // namespace stxlib::stx_minmallocpp
