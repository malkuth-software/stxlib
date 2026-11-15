#pragma once

// STXlib's allocator, used by name, ported from stx_auxlib's memory module
// (stx_auxlib 08c8132, include/stx_auxlib/memory.hpp,
// docs/ALLOCATOR_OWNERSHIP.md). Code that wants its storage from STXlib
// (owned buffers, arenas, pools held by explicit owners) calls allocate and
// release here. Nothing is redirected to it: operator new and delete,
// malloc and free stay the toolchain's, in this program and in every module
// it loads (AGENTS.md: no global allocator is imposed on foreign code).
//
// Ownership boundary (owner, 2026-11-02): only our code allocates from this
// allocator, and only our code releases to it. A block from here is never
// handed to free, operator delete or a foreign library's release call, and
// a block from anywhere else is never passed to release.
//
// "Us" is STXlib as a whole, not one module: the allocator lives in
// stxlib_state (src/state/allocator.cpp), so a block allocated by STXlib's
// code in one module (a shared library, a DLL, the executable) may be
// released by STXlib's code in any other (the coordinator's application of
// the owner's model, 2026-11-13). On Windows that also keeps every block in
// one heap, where each module linking the static C runtime has its own.
//
// Backend (STXLIB_ALLOCATOR, CMakeLists.txt): mimalloc v3.6.1 by default,
// the host's C library in sanitizer builds; backend() says which is linked.
//
// Ownership tags (STXLIB_ALLOCATION_TAGS, CMakeLists.txt: on in Debug and
// sanitizer builds): release ends the process with a report when given a
// block that is not from allocate or was already released, and a foreign
// free of a block from allocate is handed an address no allocation starts
// at. Release decides whether an address can be one of its blocks before it
// reads any byte at or around it, so a foreign address is refused without
// touching that memory: by mimalloc's page-map query with mimalloc, by a
// registry of live blocks with the host backend (allocator.cpp states the
// argument for each). tags_checked() says whether this build checks.
//
// The block contents are indeterminate; Buffer (buffer.hpp) is the owner
// that initialises them.

#include "stxlib/types.hpp"

namespace stxlib::memory {

enum class Backend : u8 {
    host = 1,
    mimalloc = 2,
    // STXlib's own allocator (src/state/stx_minmallocpp), chosen with
    // STXLIB_ALLOCATOR=stx_minmallocpp. It refuses a release of anything
    // that is not a live block of its own in every build, so tags_checked()
    // is true with it.
    stx_minmallocpp = 3,
};

[[nodiscard]] Backend backend() noexcept;

// True when this build tags its blocks and checks them on release.
[[nodiscard]] bool tags_checked() noexcept;

// Allocates `size` bytes aligned to `alignment`, a power of two. A size of
// zero yields a unique non-null block. Returns null when alignment is not a
// power of two, when the size cannot be represented with the alignment's
// bookkeeping, or when memory is exhausted. Never throws and never calls the
// new-handler.
[[nodiscard]] void* allocate(usize size, usize alignment) noexcept;

// Releases a block from allocate; null is a no-op. A block from anywhere
// else, or released twice, ends the process in the tag-checking mode and is
// undefined behaviour otherwise.
void release(void* address) noexcept;

} // namespace stxlib::memory
