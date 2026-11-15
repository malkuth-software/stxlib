// stx_minmallocpp's host layer on Windows (src/state/stx_minmallocpp/
// os.hpp). The whole host surface of the allocator is here: VirtualAlloc,
// VirtualFree, GetSystemInfo, SwitchToThread, abort, and a TLS callback for
// the thread-exit hook. kernel32 and the C runtime's abort only.
//
// Reserving. VirtualAlloc with MEM_RESERVE and PAGE_NOACCESS. Windows
// cannot trim a reservation, so an aligned one is made by reserving
// alignment bytes more, noting the aligned address inside, releasing, and
// reserving exactly there. Another thread can take the address in between;
// then the steps repeat, at most reserve_attempts times, so the cost is
// bounded.
//
// Committing is VirtualAlloc with MEM_COMMIT and PAGE_READWRITE; committed
// pages read as zero. Windows charges commit here, so exhaustion arrives as
// false and reaches the caller as a null allocation. Decommitting is
// VirtualFree with MEM_DECOMMIT.
//
// The thread-exit hook is a TLS callback in .CRT$XLY, which the loader
// calls with DLL_THREAD_DETACH in the context of each thread that exits
// cleanly, as DllMain would be (notes planning/stages/E-thread-records.md,
// section 3, gives the sources). It runs under the loader lock; the
// allocator's work there is its own table edits and VirtualFree, none of
// which waits on a thread that waits on the loader lock. Process
// termination and TerminateThread send no DLL_THREAD_DETACH, which is the
// owner's rule that termination frees nothing.
//
// Open (the logic review's L-05, suspected): when a process ends, Windows
// stops every other thread where it stands, and one of them may hold an
// allocator lock; a release or slow-path allocation made after that, during
// process detach, would then wait for ever. Not handled here; to be
// settled on a Windows run.
//
// Not yet built. The MinGW form of the callback (MSYS2 CLANG64) is the one
// written; no other Windows toolchain is supported.

#include "state/stx_minmallocpp/os.hpp"

#include <cstdlib>

#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib::stx_minmallocpp::os {

namespace {

constexpr u32 reserve_attempts = 8;

void NTAPI thread_callback(PVOID module, const DWORD reason, PVOID reserved) {
    static_cast<void>(module);
    static_cast<void>(reserved);
    if (reason == DLL_THREAD_DETACH) thread_ending(nullptr);
}

} // namespace

usize page_bytes() noexcept {
    SYSTEM_INFO information{};
    ::GetSystemInfo(&information);
    const usize result = static_cast<usize>(information.dwPageSize);
    return result;
}

void* reserve(const usize bytes, const usize alignment) noexcept {
    if (bytes > usize_max - alignment) return nullptr;
    for (u32 attempt = 0; attempt < reserve_attempts; ++attempt) {
        void* const probe = ::VirtualAlloc(nullptr, bytes + alignment, MEM_RESERVE, PAGE_NOACCESS);
        if (probe == nullptr) return nullptr;
        const uptr start = reinterpret_cast<uptr>(probe);
        const uptr aligned = (start + (alignment - 1)) & ~static_cast<uptr>(alignment - 1);
        static_cast<void>(::VirtualFree(probe, 0, MEM_RELEASE));
        void* const exact = ::VirtualAlloc(reinterpret_cast<void*>(aligned), bytes, MEM_RESERVE, PAGE_NOACCESS);
        if (exact != nullptr) return exact;
    }
    return nullptr;
}

bool commit(void* const address, const usize bytes) noexcept {
    const bool result = ::VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    return result;
}

void decommit(void* const address, const usize bytes) noexcept {
    static_cast<void>(::VirtualFree(address, bytes, MEM_DECOMMIT));
}

void release(void* const address, const usize bytes) noexcept {
    static_cast<void>(bytes);
    static_cast<void>(::VirtualFree(address, 0, MEM_RELEASE));
}

void yield() noexcept { static_cast<void>(::SwitchToThread()); }

void stop() noexcept { std::abort(); }

bool arm_thread_exit(void* const context) noexcept {
    // The callback below runs for every thread; nothing to arm per thread.
    static_cast<void>(context);
    return true;
}

// The allocator's thread_local pointer is reliable here until the callback
// has run, so there is nothing more to say about a thread.
void* armed_context() noexcept { return nullptr; }

bool thread_has_ended() noexcept { return false; }

} // namespace stxlib::stx_minmallocpp::os

#if defined(__MINGW32__)
extern "C" {
extern const IMAGE_TLS_DIRECTORY _tls_used;
// Keeps the C runtime's TLS directory, which holds the callback table, in
// the image.
__attribute__((used)) static const void* const stx_minmallocpp_tls_used = &_tls_used;
__attribute__((used, section(".CRT$XLY"))) PIMAGE_TLS_CALLBACK stx_minmallocpp_thread_callback =
    &stxlib::stx_minmallocpp::os::thread_callback;
}
#else
#error "stx_minmallocpp on Windows is written for MinGW (MSYS2 CLANG64) only"
#endif
