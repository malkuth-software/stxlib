// The C standard library's part of the interface modules (host.hpp): the
// host allocator and abnormal termination, the same calls on every family.
//
// The host allocator is ported from stx_auxlib's host backend (stx_auxlib
// 08c8132, src/hosted/memory_host.cpp). Aligned blocks over-allocate from
// malloc and keep the malloc result in the pointer-sized slot just below
// the aligned address:
//
//   base ... [slot: base] [aligned block of `size` bytes] ... base + total
//
// total = size + alignment - 1 + sizeof(void*), checked for overflow, so
// total is never zero and malloc(0)'s host-defined result never arises
// (survey D8's table). The slot may be unaligned for alignments below a
// pointer's, so it is read and written with memcpy. Sanitizer builds see
// every block, since malloc is the host's own.

#include "hosted/host.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace stxlib::host {

void* allocate_block(const usize size, const usize alignment) noexcept {
    constexpr usize slot = sizeof(void*);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return nullptr;
    const usize padding = alignment - 1 + slot;
    if (size > std::numeric_limits<usize>::max() - padding) return nullptr;
    const usize total = size + padding;
    void* const base = std::malloc(total);
    if (base == nullptr) return nullptr;
    unsigned char* const bytes = static_cast<unsigned char*>(base);
    const std::uintptr_t first_usable = reinterpret_cast<std::uintptr_t>(bytes + slot);
    const usize misalignment = static_cast<usize>(first_usable & static_cast<std::uintptr_t>(alignment - 1));
    const usize shift = misalignment == 0 ? 0 : alignment - misalignment;
    unsigned char* const aligned = bytes + slot + shift;
    std::memcpy(aligned - slot, &base, slot);
    return aligned;
}

void release_block(void* const block) noexcept {
    if (block == nullptr) return;
    void* base = nullptr;
    const unsigned char* const aligned = static_cast<const unsigned char*>(block);
    std::memcpy(&base, aligned - sizeof(void*), sizeof(void*));
    std::free(base);
}

void end_process_abnormally() noexcept { std::abort(); }

} // namespace stxlib::host
