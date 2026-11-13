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
// Backend: the host's C library (the interface modules under src/hosted/).
// stx_auxlib's mimalloc backend moves with the third-party code in a later
// batch; backend() says which is linked. Each image STXlib is linked into
// has its own allocator state, so a block is released in the image that
// allocated it.
//
// Ownership tags (STXLIB_ALLOCATION_TAGS, CMakeLists.txt: on in Debug and
// sanitizer builds): release ends the process with a report when given a
// block that is not from allocate in this image or was already released,
// and a foreign free of a block from allocate is handed an address no
// allocation starts at. tags_checked() says whether this build checks.
//
// The block contents are indeterminate; Buffer (buffer.hpp) is the owner
// that initialises them.

#include "stxlib/types.hpp"

namespace stxlib::memory {

enum class Backend : u8 {
    host = 1,
    mimalloc = 2,
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
