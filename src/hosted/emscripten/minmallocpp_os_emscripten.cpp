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
// Reserving takes the range from Emscripten's own allocator (dlmalloc,
// src/settings.js MALLOC at 6.0.13) with emscripten_builtin_memalign,
// declared in system/include/emscripten/heap.h, which is how Emscripten's
// mmap makes an anonymous mapping (system/lib/libc/emscripten_mmap.c,
// __syscall_mmap2: emscripten_builtin_memalign, then memset to zero; both
// read 2026-11-15). The reservation is then zeroed whole, because a range
// dlmalloc hands out may have been used before, and os.hpp promises that
// committed pages read as zero. dlmalloc gives the alignment's slack back
// to its own free lists, so nothing is lost to the alignment. With
// ALLOW_MEMORY_GROWTH=1 and ABORTING_MALLOC=0 (CMakeLists.txt, the
// Emscripten link settings) a range that memory cannot grow to is a null
// result, not an abort.
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
// Releasing returns the reservation to dlmalloc with
// emscripten_builtin_free. Emscripten's mmap could not stand in for any of
// this: its munmap refuses a part of a mapping (emscripten_mmap.c,
// __syscall_munmap: "We don't support partial munmapping"), which an
// aligned reservation trims, and mprotect is not implemented ("unsupported
// syscall: __syscall_mprotect" at run time, every commit then refused).
// Page protection is therefore never asked for on Emscripten; the guard
// slices of a region (page.hpp) are plain memory there.
//
// Early exit. emscripten_builtin_memalign and emscripten_builtin_free are
// C (dlmalloc), compiled without exceptions; under -pthread they hold a
// pthread mutex (dlmalloc.c, the __EMSCRIPTEN__ block: USE_LOCKS with
// shared memory, USE_SPIN_LOCKS 0), which is not a cancellation point, and
// they never call back into STXlib, so the structure lock held around
// them cannot be met again inside. memset is a plain store loop. None can
// unwind.

#include "state/stx_minmallocpp/os.hpp"

#include <cstring>

#include <emscripten/heap.h>

namespace stxlib::stx_minmallocpp::os {

void* reserve(const usize bytes, const usize alignment) noexcept {
    if (bytes == 0 || bytes > usize_max - alignment) return nullptr;
    void* const result = ::emscripten_builtin_memalign(alignment, bytes);
    if (result == nullptr) return nullptr;
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
    static_cast<void>(bytes);
    ::emscripten_builtin_free(address);
}

} // namespace stxlib::stx_minmallocpp::os
