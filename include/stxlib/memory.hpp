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
// Backend (STXLIB_ALLOCATOR, CMakeLists.txt): STXlib's own allocator,
// stx_minmallocpp (src/state/stx_minmallocpp), by default, Emscripten
// included; the host's C library in sanitizer builds, or when chosen.
// backend() says which is linked.
//
// Ownership tags (STXLIB_ALLOCATION_TAGS, CMakeLists.txt: by default on in
// Debug and sanitizer builds and in every build with the host backend, so
// a release is checked whichever backend is linked; security audit
// 2026-11-15, A-stxlib-include-01-F11): release ends the process with a report when given a
// block that is not from allocate or was already released, and a foreign
// free of a block from allocate is handed an address no allocation starts
// at. Release decides whether an address can be one of its blocks before it
// reads any byte at or around it, so a foreign address is refused without
// touching that memory: by a registry of live blocks with the host backend
// (allocator.cpp states the argument). stx_minmallocpp needs no tag: it
// refuses anything that is not a live block of its own, in every build.
// tags_checked() says whether this build checks.
//
// Process end (Windows; the security audit's A-stxlib-allocator-state-01-F2,
// confirmed on Windows hardware at 03a7630). Once ExitProcess has stopped
// the other threads (DLL_PROCESS_DETACH time), the allocator takes none of
// its locks, which a stopped thread may hold for ever. What allocate and
// release then do depends on the backend:
// - host (always tagged by default): every allocation returns null and
//   every release returns at once, whatever the size, leaving the block in
//   place (src/state/allocator.cpp, "Process end");
// - stx_minmallocpp (the default): an allocation its lock-free common path
//   can serve from the thread's own cached blocks still succeeds, any other
//   returns null; a release its common path can finish still frees the
//   block, any other returns at once and leaves the block in place
//   (src/state/stx_minmallocpp/allocator.hpp, "Process end");
// - host with the tags turned OFF: no lock of STXlib's is taken, and malloc
//   and free behave as the C runtime makes them.
// Either way nothing waits, and nothing more is given back: termination
// releases nothing. On POSIX hosts and Emscripten none of this happens.
//
// The block contents are indeterminate; Buffer (buffer.hpp) is the owner
// that initialises them.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/types.hpp"

namespace stxlib::memory {

enum class Backend : u8 {
    host = 1,
    // 2 was the C mimalloc, removed (the owner, 2026-11-15); not reused.
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
[[nodiscard]] STXLIB_NO_UNWIND void* allocate(usize size, usize alignment) noexcept;

// Releases a block from allocate; null is a no-op. A block from anywhere
// else, or released twice, ends the process with stx_minmallocpp and in the
// tag-checking mode, which is every default build; it is undefined
// behaviour only in a host-backend build whose tags were turned OFF.
STXLIB_NO_UNWIND void release(void* address) noexcept;

} // namespace stxlib::memory
