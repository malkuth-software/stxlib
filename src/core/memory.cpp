// stxlib::memory over the host backend (include/stxlib/memory.hpp), ported
// from stx_auxlib (stx_auxlib 08c8132, src/hosted/memory.cpp).
//
// With STXLIB_ALLOCATION_TAGS (CMakeLists.txt: on in Debug and sanitizer
// builds), every block carries a 16-byte header just below the address
// allocate returns:
//
//   backend block ... [offset: 8 bytes][tag: 8 bytes][block of `size` bytes]
//                                                    ^ address
//
// offset is the distance from the backend block's start to the address (a
// power of two, at least 16 and at least the alignment, so the address keeps
// the requested alignment). tag is the address XOR a per-image value whose
// low four bits are set, so a tag never equals an aligned address. release
// looks the address up in this image's registry of live blocks first (below),
// and only for a registered address reads the header, checks the tag, clears
// it and releases the backend block. The release of a block from anywhere
// else, or a second release, is not in the registry and ends the process with
// a report, having read no byte of the block or of the memory around it. A
// foreign free of a tagged block hands that allocator an address none of its
// allocations starts at.
//
// The registry (change from stx_auxlib, whose check read the tag of any
// address it was given): reading the 16 bytes below a foreign address is not
// safe on every backend. An allocator may place a block at the very start of
// a mapping, so the header's bytes are unmapped and the read faults before
// any check runs: ThreadSanitizer's allocator does (CI, linux-glibc TSan,
// stxlib_death_foreign_release: "SEGV on unknown address 0x720ffffffff8" for
// a block at 0x721000000000), and nothing in the C standard or POSIX rules it
// out for malloc. So release touches only memory it knows to be its own: the
// registry, then, for a registered address, the header allocate_tagged wrote
// inside the backend block it was given. The tag stays as a second check
// against a header overwritten in place.
//
// The registry is an open-addressing hash set of addresses (linear probing,
// tombstones, power-of-two capacity, at most three quarters full counting
// tombstones), in a block from the host's C library, under one spin lock. It
// is per image, like the tag value, because the ownership rule it checks is
// per image. It grows by rehashing into a block twice the size; if that block
// cannot be had, the allocation fails (allocate returns null) rather than
// handing out a block release could not recognise. Its storage lives as long
// as the process. Cost: one lock and one probe sequence per allocate and per
// release, in the tag-checking builds only (Debug and sanitizers).

// The per-image value differs between images, because each image linking
// STXlib has its own copy of the static it is derived from: a block
// released in an image other than the one that allocated it fails the tag
// check, which is the ownership rule "a block is released in the image that
// allocated it" made checkable.
//
// The value is drawn from host entropy, as in stx_auxlib. Change from
// stx_auxlib: without entropy it falls back to a mix of addresses (this
// image's static data and the first caller's stack, both placed by the
// host's address-space randomisation) instead of the monotonic clock. It is
// a check against mistakes, not a defence against an attacker; a forged tag
// is out of its scope.
//
// A second release of an address that has since been allocated again is
// indistinguishable from a release of the new block. Without the tags the
// address is the backend block's own and nothing is checked.

#include "stxlib/memory.hpp"

#include "core/checked.hpp"
#include "core/report.hpp"
#include "core/spin_lock.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

#if defined(STXLIB_TAGS_UNDER_ASAN)
#include <sanitizer/asan_interface.h>
#endif
#endif

namespace stxlib::memory {

namespace {

[[nodiscard]] bool power_of_two(const usize value) noexcept {
    const bool result = value != 0 && (value & (value - 1)) == 0;
    return result;
}

#if defined(STXLIB_ALLOCATION_TAGS)

constexpr usize header_bytes = 16;
constexpr usize offset_slot = 16; // the offset is at address - 16
constexpr usize tag_slot = 8;     // the tag is at address - 8
constexpr u64 tag_low_bits = 0xf;

// This image's anchor: its address is this image's own.
constinit const unsigned char image_anchor = 0;

// SplitMix64's finaliser (Steele, Lea, Flood, "Fast splittable pseudorandom
// number generators", OOPSLA 2014): mixes an address's bits, modulo 2^64.
STXLIB_MODULAR u64 mix(const u64 value) noexcept {
    u64 z = value + 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    const u64 result = z ^ (z >> 31);
    return result;
}

u64 make_image_value() noexcept {
    u64 drawn = 0;
    Byte bytes[sizeof(drawn)]{};
    if (host::fill_entropy(bytes, sizeof(bytes))) {
        std::memcpy(&drawn, bytes, sizeof(drawn));
        const u64 result = drawn | tag_low_bits;
        return result;
    }
    // No host entropy: the value then only has to differ between images and
    // from aligned addresses, which the address mix and the low bits give.
    const unsigned char stack_anchor = 0;
    const u64 image = static_cast<u64>(reinterpret_cast<std::uintptr_t>(&image_anchor));
    const u64 stack = static_cast<u64>(reinterpret_cast<std::uintptr_t>(&stack_anchor));
    const u64 result = (mix(image) ^ mix(stack)) | tag_low_bits;
    return result;
}

// Initialised once, on the first allocation, by a thread-safe static.
u64 image_value() noexcept {
    static const u64 value = make_image_value();
    return value;
}

u64 tag_for(const unsigned char* const address) noexcept {
    const u64 value = static_cast<u64>(reinterpret_cast<std::uintptr_t>(address));
    const u64 result = value ^ image_value();
    return result;
}

// ---- the registry of live blocks ---------------------------------------------

constexpr uptr empty_slot = 0;
constexpr uptr removed_slot = 1; // addresses are 16-aligned, so never 0 or 1
constexpr usize first_capacity = 1024;

struct Registry final {
    state::SpinLock lock{};
    uptr* slots{nullptr};
    usize capacity{0}; // 0 or a power of two
    usize live{0};
    usize removed{0};
};

// Constant-initialised, so it is ready before any static constructor runs.
constinit Registry registry{};

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

// Makes room for one more address. Holds registry.lock.
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
    void* const block = host::allocate_block(capacity * sizeof(uptr), alignof(uptr));
    if (block == nullptr) return false;
    uptr* const slots = static_cast<uptr*>(block);
    std::memset(slots, 0, capacity * sizeof(uptr));
    for (usize index = 0; index < registry.capacity; ++index) {
        const uptr address = registry.slots[index];
        if (address != empty_slot && address != removed_slot) place(slots, capacity, address);
    }
    host::release_block(registry.slots);
    registry.slots = slots;
    registry.capacity = capacity;
    registry.removed = 0;
    return true;
}

bool register_block(const unsigned char* const address) noexcept {
    const spin::Guard lock(registry.lock);
    if (!make_room()) return false;
    place(registry.slots, registry.capacity, reinterpret_cast<uptr>(address));
    registry.live = registry.live + 1;
    return true;
}

// Removes `address`; false when it is not registered. Reads registry storage
// only.
bool unregister_block(const unsigned char* const address) noexcept {
    const uptr wanted = reinterpret_cast<uptr>(address);
    if (wanted == empty_slot || wanted == removed_slot) return false;
    const spin::Guard lock(registry.lock);
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

[[noreturn]] void reject_release() noexcept {
    report::text("stxlib: requirement failed: stxlib::memory::release: the block is not from memory::allocate "
                 "in this image, or was already released\n");
    host::end_process_abnormally();
}

void* allocate_tagged(const usize size, const usize alignment) noexcept {
    const usize offset = alignment < header_bytes ? header_bytes : alignment;
    if (size > std::numeric_limits<usize>::max() - offset) return nullptr;
    void* const block = host::allocate_block(size + offset, offset);
    if (block == nullptr) return nullptr;
    unsigned char* const address = static_cast<unsigned char*>(block) + offset;
    const u64 offset_value = static_cast<u64>(offset);
    const u64 tag = tag_for(address);
    std::memcpy(address - offset_slot, &offset_value, sizeof(offset_value));
    std::memcpy(address - tag_slot, &tag, sizeof(tag));
    if (!register_block(address)) {
        host::release_block(block);
        return nullptr;
    }
    return address;
}

void release_tagged(void* const block) noexcept {
    unsigned char* const address = static_cast<unsigned char*>(block);
    // Nothing of the block or around it is read unless it is ours.
    if (!unregister_block(address)) reject_release();
    u64 tag = 0;
    std::memcpy(&tag, address - tag_slot, sizeof(tag));
    if (tag != tag_for(address)) reject_release();
    u64 offset_value = 0;
    std::memcpy(&offset_value, address - offset_slot, sizeof(offset_value));
    const bool offset_valid = offset_value >= header_bytes && power_of_two(static_cast<usize>(offset_value));
    if (!offset_valid) reject_release();
    const u64 cleared = 0;
    std::memcpy(address - tag_slot, &cleared, sizeof(cleared));
    host::release_block(address - static_cast<usize>(offset_value));
}

#endif

} // namespace

Backend backend() noexcept { return Backend::host; }

bool tags_checked() noexcept {
#if defined(STXLIB_ALLOCATION_TAGS)
    return true;
#else
    return false;
#endif
}

void* allocate(const usize size, const usize alignment) noexcept {
    if (!power_of_two(alignment)) return nullptr;
#if defined(STXLIB_ALLOCATION_TAGS)
    void* const result = allocate_tagged(size, alignment);
#else
    void* const result = host::allocate_block(size, alignment);
#endif
    return result;
}

void release(void* const address) noexcept {
    if (address == nullptr) return;
#if defined(STXLIB_ALLOCATION_TAGS)
    release_tagged(address);
#else
    host::release_block(address);
#endif
}

} // namespace stxlib::memory
