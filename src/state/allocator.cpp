// STXlib's allocator, owned by stxlib_state (state.hpp, `allocator`), so
// one allocator serves every module STXlib is linked into: a block from
// memory::allocate in one of our modules may be released by memory::release
// in another. The owner's ownership model, as the coordinator applied it on
// 2026-11-13: memory we allocate is released only by us, and "us" is STXlib
// as a whole, not one DLL. It also meets the Windows constraint that each
// module linking the static C runtime has its own malloc heap: every block
// here comes from the one backend inside stxlib_state.
//
// The backend's host calls (malloc and free, or mimalloc's, with the
// cancellation guard around mimalloc's) are made here, in src/state/, not
// under src/hosted/: the one host interface called from the state, recorded
// as an exception in AGENTS ("Hard boundaries"; audit A, S1).
//
// Early exits (AGENTS.md, "Allocators must not unwind"): nothing on this
// path throws, longjmps or calls a new-handler. The one early exit mimalloc
// can take is a POSIX thread cancellation: v3.6.1's _mi_prim_thread_yield is
// sleep(0) (src/prim/unix/prim.c:1147-1149), a cancellation point, inside a
// busy-wait mi_free can reach (src/arena/bitmap-chunk.c:122-125; smi's
// mimalloc audit, E1). On POSIX hosts each mimalloc call therefore runs with
// cancellation disabled (CancellationOff below; the owner's choice for Q3,
// 2026-11-15): pthread_setcancelstate is not itself a cancellation point,
// and a cancellation requested meanwhile stays pending until the thread's
// next cancellation point after the call. A thread that set asynchronous
// cancellation is outside this (STXlib is not async-cancel-safe).
//
// Ported from stx_auxlib b6e330a (src/hosted/memory.cpp, memory_host.cpp,
// memory_mimalloc.cpp) by way of STXlib's src/core/memory.cpp (batch 2a and
// the foreign-release fix), which this file replaces as the owner of the
// backend, the tags and the ownership check.
//
// Backends (STXLIB_ALLOCATOR, CMakeLists.txt):
// - mimalloc v3.5.3 (third_party/mimalloc, PROVENANCE.md), compiled into
//   stxlib_state with hidden symbols and no override, so its heap holds
//   STXlib's blocks only. The default, except in sanitizer builds.
// - the host's C library (malloc, over-allocated for alignment, the base
//   kept in the slot below the block). The default in sanitizer builds,
//   which track the host allocator only.
//
// Tags (STXLIB_ALLOCATION_TAGS: Debug and sanitizer builds): every block
// carries a 16-byte header just below the address allocate returns,
//
//   backend block ... [offset: 8 bytes][tag: 8 bytes][block of `size` bytes]
//                                                    ^ address
//
// offset is the distance from the backend block's start to the address (a
// power of two, at least 16 and at least the alignment); tag is the address
// XOR a per-process value whose low four bits are set, so it never equals an
// aligned address. Release first decides, without reading any byte at or
// around the address, whether the address can be one of ours (the ownership
// check below); only then does it read the header, check the tag, clear it
// and release the backend block. Anything else is refused, and the caller
// (memory.cpp) ends the process with a report.
//
// The ownership check, per backend:
// - mimalloc: mi_is_in_heap_region(address - 16), mimalloc's own query, which
//   looks the address up in mimalloc's page map and returns whether it lies
//   in a mimalloc page (third_party/mimalloc/src/page-map.c, lines 199-210
//   for the flat map and 509-515 for the two-level map: `_mi_safe_ptr_page`
//   "Return NULL for invalid pointers"; heap.c lines 276-289 make
//   mi_any_heap_contains and mi_check_owned the same test). It reads only
//   the page map, never the address. The coordinator's choice (registry
//   option 1, 2026-11-13) replaces the registry with it in mimalloc builds.
//   Release asks it of the header's address only (the address minus 16)
//   and refuses an address that is not 16-aligned or below 16. Asking of
//   the address itself as well wrongly refused zero-size blocks whose
//   address is the end of their mimalloc block at a page end (alignments of
//   128 KiB to 2 MiB, and the last block of a filled small page), and 0- and
//   1-byte blocks at 32 KiB and 64 KiB alignment, whose address falls in a
//   slice v3.6.1 leaves unregistered (smi's mimalloc audit, A1 and A2).
//   Why the header read that follows stays inside committed memory, argued
//   from the source and sampled, not proved (smi's audit, A10): the 16
//   header bytes start on a 16-byte boundary, so they lie in one 64 KiB
//   slice, the slice of the address the query asked about; with mimalloc's
//   default options every slice registered in the page map belongs to a
//   live page and is fully committed: regular pages are allocated committed
//   (page_commit_on_demand is 0: arena-page.c:812-815, 602-613), singleton
//   pages always are (arena-page.c:844, 513-517), and a page is unregistered
//   before its slices are freed and purged (arena-page.c:921). An
//   environment setting that turns on page_commit_on_demand weakens this
//   (mimalloc reads its options from the environment); a stale address
//   racing the release and purge of its page (purge_delay, 1000 ms) is a
//   misuse the check does not cover.
// - host: a registry of live addresses (an open-addressing hash set under
//   one spin lock, in malloc storage), as the foreign-release fix made it:
//   the host allocator gives no query, and reading below a foreign address
//   faults on some allocators (ThreadSanitizer's, CI 2026-11-13).
//
// The per-process value: STXlib's tags no longer name an image (a block may
// cross modules), so the value is drawn once per process inside
// stxlib_state, from a mix of addresses the host's address-space
// randomisation places (this module's static data and the first caller's
// stack), with the low four bits set. STXlib's own code here draws no host
// entropy; mimalloc, linked into stxlib_state, does seed itself from the
// host when it initialises at load (v3.6.1 process.c:561-569, random.c:
// 176-200; smi's audit, D7), which this value does not use. The value is a
// check against mistakes, not a defence against an attacker.
//
// A second release of an address that has since been allocated again is
// indistinguishable from a release of the new block. Without the tags the
// address is the backend block's own and nothing is checked.

#include "state/state.hpp"

#include "core/checked.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(STXLIB_ALLOCATOR_MIMALLOC)
// Sanitizers track the host allocator only (stx_auxlib's rule,
// memory_mimalloc.cpp; CMakeLists.txt picks the host backend for them).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "the mimalloc backend is not admitted in sanitizer builds"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#error "the mimalloc backend is not admitted in sanitizer builds"
#endif
#endif
#include <mimalloc.h>
#if !defined(_WIN32)
#include <pthread.h>
#endif
#endif

#include "stxlib/detail/early_exit.hpp"

namespace stxlib::state {

namespace {

[[nodiscard]] bool power_of_two(const usize value) noexcept {
    const bool result = value != 0 && (value & (value - 1)) == 0;
    return result;
}

// ---- the backend ---------------------------------------------------------------

#if defined(STXLIB_ALLOCATOR_MIMALLOC)

constexpr u8 backend_value = 2; // memory::Backend::mimalloc

// mimalloc refuses requests above PTRDIFF_MAX itself; refusing them here
// keeps its error message out of the output.
constexpr usize largest_request = static_cast<usize>(PTRDIFF_MAX);

// Disables POSIX thread cancellation for its lifetime and restores the
// previous state (see the file comment). Windows has no thread
// cancellation, so there it does nothing.
class STXLIB_EARLY_EXIT_NO_CANCEL CancellationOff final {
public:
    CancellationOff() noexcept {
#if !defined(_WIN32)
        static_cast<void>(::pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous_));
#endif
    }
    ~CancellationOff() {
#if !defined(_WIN32)
        int ignored = 0;
        static_cast<void>(::pthread_setcancelstate(previous_, &ignored));
#endif
    }
    CancellationOff(const CancellationOff&) = delete;
    CancellationOff& operator=(const CancellationOff&) = delete;

private:
#if !defined(_WIN32)
    int previous_{0};
#endif
};

STXLIB_NO_UNWIND void* backend_allocate(const usize size, const usize alignment) noexcept {
    // Checked so the subtraction cannot wrap: an alignment above
    // PTRDIFF_MAX (2^63, a power of two memory::allocate accepts) is refused
    // here (smi's audit, A6).
    if (alignment > largest_request || size > largest_request - alignment) return nullptr;
    const CancellationOff no_cancel;
    void* const result = mi_malloc_aligned(size, alignment);
    return result;
}

STXLIB_NO_UNWIND void backend_release(void* const block) noexcept {
    const CancellationOff no_cancel;
    mi_free(block);
}

#else

constexpr u8 backend_value = 1; // memory::Backend::host

// Ported from stx_auxlib's host backend (memory_host.cpp) by way of
// STXlib's c_library.cpp: over-allocate from malloc and keep malloc's
// result in the pointer-sized slot just below the aligned address. total =
// size + alignment - 1 + sizeof(void*), checked, so malloc(0) never arises.
STXLIB_NO_UNWIND void* backend_allocate(const usize size, const usize alignment) noexcept {
    constexpr usize slot = sizeof(void*);
    const usize padding = alignment - 1 + slot;
    if (size > std::numeric_limits<usize>::max() - padding) return nullptr;
    void* const base = std::malloc(size + padding);
    if (base == nullptr) return nullptr;
    unsigned char* const bytes = static_cast<unsigned char*>(base);
    const std::uintptr_t first_usable = reinterpret_cast<std::uintptr_t>(bytes + slot);
    const usize misalignment = static_cast<usize>(first_usable & static_cast<std::uintptr_t>(alignment - 1));
    const usize shift = misalignment == 0 ? 0 : alignment - misalignment;
    unsigned char* const aligned = bytes + slot + shift;
    std::memcpy(aligned - slot, &base, slot);
    return aligned;
}

STXLIB_NO_UNWIND void backend_release(void* const block) noexcept {
    void* base = nullptr;
    const unsigned char* const aligned = static_cast<const unsigned char*>(block);
    std::memcpy(&base, aligned - sizeof(void*), sizeof(void*));
    std::free(base);
}

#endif

#if defined(STXLIB_ALLOCATION_TAGS)

constexpr usize header_bytes = 16;
constexpr usize offset_slot = 16; // the offset is at address - 16
constexpr usize tag_slot = 8;     // the tag is at address - 8
constexpr u64 tag_low_bits = 0xf;

// This module's anchor: its address is placed by the host.
constinit const unsigned char process_anchor = 0;

// SplitMix64's finaliser (Steele, Lea, Flood, "Fast splittable pseudorandom
// number generators", OOPSLA 2014): mixes an address's bits, modulo 2^64.
STXLIB_MODULAR u64 mix(const u64 value) noexcept {
    u64 z = value + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    const u64 result = z ^ (z >> 31);
    return result;
}

u64 make_process_value() noexcept {
    const unsigned char stack_anchor = 0;
    const u64 module = static_cast<u64>(reinterpret_cast<std::uintptr_t>(&process_anchor));
    const u64 stack = static_cast<u64>(reinterpret_cast<std::uintptr_t>(&stack_anchor));
    const u64 result = (mix(module) ^ mix(stack)) | tag_low_bits;
    return result;
}

// Initialised once, on the first allocation, by a thread-safe static.
u64 process_value() noexcept {
    static const u64 value = make_process_value();
    return value;
}

u64 tag_for(const unsigned char* const address) noexcept {
    const u64 value = static_cast<u64>(reinterpret_cast<std::uintptr_t>(address));
    const u64 result = value ^ process_value();
    return result;
}

#if defined(STXLIB_ALLOCATOR_MIMALLOC)

bool track_block(const unsigned char* const address) noexcept {
    static_cast<void>(address);
    return true;
}

// Whether `address` may be one of ours, deciding by mimalloc's page map
// only (see the file comment); release then checks the tag.
bool forget_block(const unsigned char* const address) noexcept {
    const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(address);
    if ((value & (header_bytes - 1)) != 0 || value < header_bytes) return false;
    // The header's slice only (smi's audit, A1 and A2; see the file comment).
    const bool result = mi_is_in_heap_region(address - header_bytes);
    return result;
}

#else

// ---- the registry of live blocks (host backend) ---------------------------------

constexpr uptr empty_slot = 0;
constexpr uptr removed_slot = 1; // addresses are 16-aligned, so never 0 or 1
constexpr usize first_capacity = 1024;

struct Registry final {
    SpinLock lock{};
    uptr* slots{nullptr};
    usize capacity{0}; // 0 or a power of two
    usize live{0};
    usize removed{0};
};

// Constant-initialised, so it is ready before any static constructor runs.
constinit Registry registry{};

// The registry's lock: stxlib_state cannot use the boundary's spin::Guard,
// so it spins on the flag itself. Unlike spin_lock.hpp it neither yields nor
// sleeps (audit A, S2): the state calls no host interface but the
// allocator's backend (AGENTS, "Hard boundaries"), so it cannot reach
// host::yield_thread, and the registry serves only the host backend
// (sanitizer builds and the malloc fallback), not the default mimalloc one.
// The cost, a waiter spinning through another thread's rehash, is stated
// here rather than removed.
class STXLIB_EARLY_EXIT_LOCK RegistryGuard final {
public:
    RegistryGuard() noexcept {
        while (registry.lock.held.exchange(1, std::memory_order_acquire) != 0) {
            while (registry.lock.held.load(std::memory_order_relaxed) != 0) {
            }
        }
    }
    ~RegistryGuard() { registry.lock.held.store(0, std::memory_order_release); }
    RegistryGuard(const RegistryGuard&) = delete;
    RegistryGuard& operator=(const RegistryGuard&) = delete;
};

usize home_slot(const uptr address, const usize capacity) noexcept {
    const usize result = static_cast<usize>(mix(static_cast<u64>(address))) & (capacity - 1);
    return result;
}

// Inserts into `slots` (capacity a power of two, at least one empty slot).
void place(uptr* const slots, const usize capacity, const uptr address) noexcept {
    usize index = home_slot(address, capacity);
    while (slots[index] != empty_slot && slots[index] != removed_slot) index = (index + 1) & (capacity - 1);
    slots[index] = address;
}

// Makes room for one more address. Holds the registry's lock.
bool make_room() noexcept {
    const usize used = registry.live + registry.removed + 1;
    if (registry.capacity != 0 && used <= registry.capacity / 4 * 3) return true;
    usize capacity = registry.capacity == 0 ? first_capacity : registry.capacity;
    // Grow when live addresses alone would pass half; otherwise rehashing at
    // the same size clears the tombstones.
    if (registry.live + 1 > capacity / 2) {
        if (capacity > std::numeric_limits<usize>::max() / 2 / sizeof(uptr)) return false;
        capacity = capacity * 2;
    }
    void* const block = backend_allocate(capacity * sizeof(uptr), alignof(uptr));
    if (block == nullptr) return false;
    uptr* const slots = static_cast<uptr*>(block);
    std::memset(slots, 0, capacity * sizeof(uptr));
    for (usize index = 0; index < registry.capacity; ++index) {
        const uptr address = registry.slots[index];
        if (address != empty_slot && address != removed_slot) place(slots, capacity, address);
    }
    // Let go before releasing (AGENTS.md, rule 1).
    uptr* const previous = registry.slots;
    registry.slots = slots;
    registry.capacity = capacity;
    registry.removed = 0;
    if (previous != nullptr) backend_release(previous);
    return true;
}

bool track_block(const unsigned char* const address) noexcept {
    const RegistryGuard lock;
    if (!make_room()) return false;
    place(registry.slots, registry.capacity, reinterpret_cast<uptr>(address));
    registry.live = registry.live + 1;
    return true;
}

// Removes `address`; false when it is not registered. Reads registry storage
// only.
bool forget_block(const unsigned char* const address) noexcept {
    const uptr wanted = reinterpret_cast<uptr>(address);
    if (wanted == empty_slot || wanted == removed_slot) return false;
    const RegistryGuard lock;
    if (registry.capacity == 0) return false;
    usize index = home_slot(wanted, registry.capacity);
    // At most capacity probes; make_room keeps an empty slot, so the loop
    // ends at one before that.
    for (usize probes = 0; probes < registry.capacity; ++probes) {
        const uptr found = registry.slots[index];
        if (found == empty_slot) return false;
        if (found == wanted) {
            registry.slots[index] = removed_slot;
            registry.live = registry.live - 1;
            registry.removed = registry.removed + 1;
            return true;
        }
        index = (index + 1) & (registry.capacity - 1);
    }
    return false;
}

#endif

STXLIB_NO_UNWIND void* allocate_block(const usize size, const usize alignment) noexcept {
    if (!power_of_two(alignment)) return nullptr;
    const usize offset = alignment < header_bytes ? header_bytes : alignment;
    if (size > std::numeric_limits<usize>::max() - offset) return nullptr;
    void* const block = backend_allocate(size + offset, offset);
    if (block == nullptr) return nullptr;
    unsigned char* const address = static_cast<unsigned char*>(block) + offset;
    const u64 offset_value = static_cast<u64>(offset);
    const u64 tag = tag_for(address);
    std::memcpy(address - offset_slot, &offset_value, sizeof(offset_value));
    std::memcpy(address - tag_slot, &tag, sizeof(tag));
    if (!track_block(address)) {
        backend_release(block);
        return nullptr;
    }
    return address;
}

STXLIB_NO_UNWIND ReleaseOutcome release_block(void* const block) noexcept {
    unsigned char* const address = static_cast<unsigned char*>(block);
    // Nothing of the block or around it is read unless it may be ours.
    if (!forget_block(address)) return ReleaseOutcome::refused;
    u64 tag = 0;
    std::memcpy(&tag, address - tag_slot, sizeof(tag));
    if (tag != tag_for(address)) return ReleaseOutcome::refused;
    u64 offset_value = 0;
    std::memcpy(&offset_value, address - offset_slot, sizeof(offset_value));
    const bool offset_valid = offset_value >= header_bytes && power_of_two(static_cast<usize>(offset_value));
    if (!offset_valid) return ReleaseOutcome::refused;
    const u64 cleared = 0;
    std::memcpy(address - tag_slot, &cleared, sizeof(cleared));
    backend_release(address - static_cast<usize>(offset_value));
    return ReleaseOutcome::released;
}

constexpr bool tags_value = true;

#else

STXLIB_NO_UNWIND void* allocate_block(const usize size, const usize alignment) noexcept {
    if (!power_of_two(alignment)) return nullptr;
    void* const result = backend_allocate(size, alignment);
    return result;
}

STXLIB_NO_UNWIND ReleaseOutcome release_block(void* const block) noexcept {
    backend_release(block);
    return ReleaseOutcome::released;
}

constexpr bool tags_value = false;

#endif

constinit const AllocatorCalls calls{
    .allocate = allocate_block,
    .release = release_block,
    .backend = backend_value,
    .tags_checked = tags_value,
};

} // namespace

const AllocatorCalls* allocator(const u32 expected_layout) noexcept {
    if (expected_layout != layout_key) return nullptr;
    const AllocatorCalls* const result = &calls;
    return result;
}

} // namespace stxlib::state
