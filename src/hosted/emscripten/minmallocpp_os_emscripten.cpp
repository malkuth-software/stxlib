// stx_minmallocpp's address space on Emscripten (src/state/stx_minmallocpp/
// os.hpp): reserve, commit, decommit and release over wasm linear memory.
// The rest of the host layer (the page size, yield, stop and the
// thread-exit hook) is the POSIX one, minmallocpp_os_posix.cpp, compiled
// for Emscripten with these four left out: Emscripten's musl has sysconf,
// sched_yield, abort and the pthread key calls, and runs key destructors
// when a pthread exits.
//
// Linear memory has no page protection and no unmapped pages: every byte
// below the memory's current size is readable and writable, memory only
// grows, and nothing can be handed back to the host (WebAssembly core
// specification, memory instructions: memory.grow, and no shrink). So:
//
// Reserving takes the range from the top of linear memory with sbrk
// (unistd.h; system/lib/libc/sbrk.c at 6.0.13: a compare-exchange on the
// break, growing the memory when the new break is past its size, (void*)-1
// when it cannot), at the first multiple of the alignment at or after the
// break. A range the allocator releases goes to a free list here, kept
// sorted and merged with its neighbours, and a later reservation takes the
// first range in it that holds an aligned run of its size, before it moves
// the break; the pieces before and after that run stay in the list.
// Nothing is given back to the host, since linear memory cannot shrink.
// The reservation is then zeroed whole, because a range from the list was
// used before (and memory above the break may have been, if Emscripten's
// dlmalloc ever lowered it), and os.hpp promises that committed pages read
// as zero.
//
// Why not Emscripten's allocator, as this file first did (dlmalloc's
// emscripten_builtin_memalign, which Emscripten's mmap uses): regions and
// huge mappings are aligned to the region size, 64 MiB on wasm32, and
// dlmalloc meets an alignment by taking the request plus the alignment and
// returning the part before the aligned start to its free lists. Its chunk
// header puts the next free byte just past each aligned run, so the next
// reservation's leading part was nearly a whole region, and nothing of that
// size is ever asked of dlmalloc again: each reservation grew the memory by
// about 64 MiB more than it held (measured 2026-11-23: a 17 MiB block cost
// 127.56 MiB of linear memory). From the break, the leading part is empty
// when the previous reservation ended the memory, and otherwise is what
// dlmalloc (which also takes its memory with sbrk, and allows another
// caller to move the break: it then starts a new segment) took since,
// rounded up to the alignment; that part goes to the free list.
//
// The free list holds free_capacity ranges. Merging keeps it short (each
// range lies between two used ones), and a range that finds it full is
// left out of it: memory lost, never memory given twice. A lock of its own
// (a flag, waited on with yield) guards the list and the break; it is
// taken around no other lock's call, and nothing inside it calls back into
// the allocator.
//
// Committing changes nothing and cannot fail: the range is already
// readable and writable, and it reads as zero because the reservation was
// zeroed and decommit zeroes again. It must not zero, because region.cpp
// commits ranges that are partly committed already (commit_units: "a range
// that is already committed is committed again, which changes nothing").
//
// Decommitting zeroes the range, so that a later commit reads zero. The
// memory is not given back; it stays part of the wasm memory, whose size
// never shrinks. The cost is one memset of whole 1 MiB units (region.cpp
// decommits units only), made under the structure lock like every host
// call there.
//
// Releasing puts the reservation in the free list. Emscripten's mmap
// could not stand in for any of this: its munmap refuses a part of a
// mapping (emscripten_mmap.c, __syscall_munmap: "We don't support partial
// munmapping"), which an aligned reservation trims, and mprotect is not
// implemented ("unsupported syscall: __syscall_mprotect" at run time, every
// commit then refused). Page protection is therefore never asked for on
// Emscripten; the guard slices of a region (page.hpp) are plain memory
// there.
//
// Early exit. sbrk is C, compiled without exceptions; it calls
// emscripten_resize_heap, which grows the memory or reports that it cannot,
// and neither is a cancellation point or calls back into STXlib. The free
// list is plain loads and stores, and memset a plain store loop. None can
// unwind.

#include "state/stx_minmallocpp/os.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <unistd.h>

namespace stxlib::stx_minmallocpp::os {

namespace {

// A run of linear memory taken from sbrk and not in use: [begin, end).
struct FreeRange final {
    uptr begin{0};
    uptr end{0};
};

inline constexpr usize free_capacity = 256;

// The free list, sorted by begin, no two ranges touching, and its lock.
struct Space final {
    std::atomic<bool> locked{false};
    FreeRange ranges[free_capacity]{};
    usize count{0};
};

constinit Space space{};

class SpaceLock final {
public:
    SpaceLock() noexcept {
        while (space.locked.exchange(true, std::memory_order_acquire)) yield();
    }
    ~SpaceLock() { space.locked.store(false, std::memory_order_release); }
    SpaceLock(const SpaceLock&) = delete;
    SpaceLock& operator=(const SpaceLock&) = delete;
};

// The first multiple of `alignment` (a power of two) at or after `value`;
// 0 when there is none below the address limit.
[[nodiscard]] uptr aligned_at_or_after(const uptr value, const usize alignment) noexcept {
    const uptr mask = static_cast<uptr>(alignment) - 1;
    if (value > ~mask) return 0;
    return (value + mask) & ~mask;
}

void remove_range(const usize index) noexcept {
    for (usize next = index + 1; next < space.count; ++next) space.ranges[next - 1] = space.ranges[next];
    space.count = space.count - 1;
}

// Puts [begin, end) in the free list, merged with the ranges it touches;
// left out when the list is full and it touches none. Under the lock.
void give_range(const uptr begin, const uptr end) noexcept {
    if (begin >= end) return;
    usize index = 0;
    while (index < space.count && space.ranges[index].begin < begin) index = index + 1;
    const bool joins_before = index > 0 && space.ranges[index - 1].end == begin;
    const bool joins_after = index < space.count && space.ranges[index].begin == end;
    if (joins_before && joins_after) {
        space.ranges[index - 1].end = space.ranges[index].end;
        remove_range(index);
        return;
    }
    if (joins_before) {
        space.ranges[index - 1].end = end;
        return;
    }
    if (joins_after) {
        space.ranges[index].begin = begin;
        return;
    }
    if (space.count == free_capacity) return;
    for (usize slot = space.count; slot > index; --slot) space.ranges[slot] = space.ranges[slot - 1];
    space.ranges[index] = FreeRange{.begin = begin, .end = end};
    space.count = space.count + 1;
}

// An aligned run of `bytes` from the free list, the rest of its range left
// there; 0 when no range holds one. Under the lock.
[[nodiscard]] uptr take_listed(const usize bytes, const usize alignment) noexcept {
    for (usize index = 0; index < space.count; ++index) {
        const FreeRange range = space.ranges[index];
        const uptr start = aligned_at_or_after(range.begin, alignment);
        if (start == 0 || start > range.end || range.end - start < bytes) continue;
        remove_range(index);
        give_range(range.begin, start);
        give_range(start + bytes, range.end);
        return start;
    }
    return 0;
}

// An aligned run of `bytes` past the break, the break moved past it; 0
// when the memory cannot grow. Another thread's sbrk between reading the
// break and moving it leaves what this one took misplaced: it goes to the
// free list and the attempt is made again (at most four).
[[nodiscard]] uptr take_from_break(const usize bytes, const usize alignment) noexcept {
    for (usize attempt = 0; attempt < 4; ++attempt) {
        const uptr top = reinterpret_cast<uptr>(::sbrk(0));
        const uptr start = aligned_at_or_after(top, alignment);
        if (start == 0 || bytes > usize_max - (start - top)) return 0;
        const usize grow = static_cast<usize>(start - top) + bytes;
        if (grow > static_cast<usize>(INTPTR_MAX)) return 0;
        void* const taken = ::sbrk(static_cast<intptr_t>(grow));
        if (taken == reinterpret_cast<void*>(-1)) return 0;
        const uptr first = reinterpret_cast<uptr>(taken);
        const uptr last = first + grow;
        const uptr placed = aligned_at_or_after(first, alignment);
        if (placed != 0 && placed <= last && last - placed >= bytes) {
            give_range(first, placed);
            give_range(placed + bytes, last);
            return placed;
        }
        give_range(first, last);
    }
    return 0;
}

} // namespace

void* reserve(const usize bytes, const usize alignment) noexcept {
    if (bytes == 0 || bytes > usize_max - alignment) return nullptr;
    uptr start = 0;
    {
        const SpaceLock scope;
        start = take_listed(bytes, alignment);
        if (start == 0) start = take_from_break(bytes, alignment);
    }
    if (start == 0) return nullptr;
    void* const result = reinterpret_cast<void*>(start);
    std::memset(result, 0, bytes);
    return result;
}

bool commit(void* const address, const usize bytes) noexcept {
    static_cast<void>(address);
    static_cast<void>(bytes);
    return true;
}

void decommit(void* const address, const usize bytes) noexcept { std::memset(address, 0, bytes); }

void release(void* const address, const usize bytes) noexcept {
    const uptr begin = reinterpret_cast<uptr>(address);
    const SpaceLock scope;
    give_range(begin, begin + bytes);
}

} // namespace stxlib::stx_minmallocpp::os
