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
// checks the tag, clears it and releases the backend block, so the release
// of a block from anywhere else, or a second release, finds no tag and ends
// the process with a report instead of corrupting a heap. A foreign free of
// a tagged block hands that allocator an address none of its allocations
// starts at.
//
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
#include "hosted/host.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

#if defined(STXLIB_ALLOCATION_TAGS)
#if defined(__SANITIZE_ADDRESS__)
#define STXLIB_TAGS_UNDER_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define STXLIB_TAGS_UNDER_ASAN 1
#endif
#endif
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
    return address;
}

void release_tagged(void* const block) noexcept {
    unsigned char* const address = static_cast<unsigned char*>(block);
#if defined(STXLIB_TAGS_UNDER_ASAN)
    // The header of a foreign block is its allocator's red zone, and the
    // header of a released block is quarantined: both are poisoned, and
    // reading them would end in the sanitizer's report instead of ours.
    if (__asan_region_is_poisoned(address - header_bytes, header_bytes) != nullptr) reject_release();
#endif
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
