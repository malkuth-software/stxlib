// The library of tests/allocator_exit_tests.cpp (tests/allocator_exit.hpp).
//
// The logic review's L-05 (src/hosted/windows/minmallocpp_os_windows.cpp):
// when a Windows process ends, the host stops every other thread where it
// stands, possibly inside an allocator lock, and then detaches each library.
// A release or slow-path allocation made during that detach would wait for
// ever on the stopped thread's lock. The workers here keep the allocator's
// locks busy until the end; the destructor of `detach_check` runs during this
// library's detach (MinGW runs a library's static destructors from its
// DLL_PROCESS_DETACH) and takes every path that locks.
//
// Once the process is ending the allocator may refuse (a null block) rather
// than take a lock, so the destructor counts refusals instead of failing on
// them: what it checks is that every call returns. It writes
// "ok <refused>" to the marker, and on Windows also what
// RtlDllShutdownInProgress (ntdll, looked up by name) said at the start of
// this library's DLL_PROCESS_DETACH and in the destructor, and whether the
// detach's reserved argument was non-null (Microsoft's DllMain page: non-null
// when the process is terminating), as " shutdown=<at detach>,<in destructor>
// reserved=<0|1>". The STXlib fix for L-05 assumes the first value is 1
// during a normal ExitProcess.

#include "allocator_exit.hpp"

#include "stxlib/memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using stxlib::u32;
using stxlib::usize;

constexpr usize ring_slots = 64;
constexpr usize marker_capacity = 1024;

// Sizes across the small and medium classes, a singleton (the structure
// lock and a host reservation) and zero.
constexpr usize worker_sizes[] = {48, 1000, 70000, 3000000, 16, 0, 520000, 256};
constexpr usize worker_size_count = sizeof(worker_sizes) / sizeof(worker_sizes[0]);

// RtlDllShutdownInProgress's answer: -1 unknown (not Windows, or not found),
// else 0 or 1.
int shutdown_in_progress() noexcept {
#if defined(_WIN32)
    // ntdll exports it (undocumented in the SDK headers): BOOLEAN NTAPI (void).
    using ShutdownQuery = BOOLEAN(NTAPI*)();
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return -1;
    const FARPROC found = GetProcAddress(ntdll, "RtlDllShutdownInProgress");
    if (found == nullptr) return -1;
    ShutdownQuery query = nullptr;
    static_assert(sizeof(query) == sizeof(found), "function pointers share one representation");
    std::memcpy(&query, &found, sizeof(query));
    const int result = query() != 0 ? 1 : 0;
    return result;
#else
    return -1;
#endif
}

// Written by DllMain at DLL_PROCESS_DETACH, read by the destructor after it
// (MinGW's DLL entry calls DllMain before the library's destructors).
int shutdown_at_detach = -1;
int reserved_at_detach = -1;

void worker(const u32 seed) noexcept {
    void* ring[ring_slots] = {};
    usize step = seed;
    for (;;) {
        const usize slot = step % ring_slots;
        stxlib::memory::release(ring[slot]);
        const usize size = worker_sizes[(step / 3) % worker_size_count];
        ring[slot] = stxlib::memory::allocate(size, 16);
        if (ring[slot] != nullptr && size > 0) static_cast<char*>(ring[slot])[0] = 1;
        step = step + 7;
    }
}

struct DetachCheck final {
    char marker[marker_capacity] = {};

    DetachCheck() noexcept = default;
    DetachCheck(const DetachCheck&) = delete;
    DetachCheck& operator=(const DetachCheck&) = delete;

    ~DetachCheck() {
        if (marker[0] == '\0') return;
        const int shutdown_in_destructor = shutdown_in_progress();
        u32 refused = 0;
        for (const usize size : worker_sizes) {
            void* const block = stxlib::memory::allocate(size, 64);
            if (block == nullptr && size > 0) refused = refused + 1;
            stxlib::memory::release(block);
        }
        std::FILE* const file = std::fopen(marker, "wb");
        if (file == nullptr) std::abort();
        static_cast<void>(std::fprintf(file, "ok %u shutdown=%d,%d reserved=%d", refused, shutdown_at_detach,
                                       shutdown_in_destructor, reserved_at_detach));
        static_cast<void>(std::fclose(file));
    }
};

DetachCheck detach_check;

} // namespace

#if defined(_WIN32)
extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    static_cast<void>(module);
    if (reason == DLL_PROCESS_DETACH) {
        shutdown_at_detach = shutdown_in_progress();
        reserved_at_detach = reserved != nullptr ? 1 : 0;
        // Also written at once, before any allocation, so the values are
        // known even when the destructor that follows does not return.
        if (detach_check.marker[0] != '\0') {
            char early[marker_capacity + 8] = {};
            static_cast<void>(std::snprintf(early, sizeof(early), "%s.detach", detach_check.marker));
            std::FILE* const file = std::fopen(early, "wb");
            if (file != nullptr) {
                static_cast<void>(std::fprintf(file, "shutdown=%d reserved=%d", shutdown_at_detach, reserved_at_detach));
                static_cast<void>(std::fclose(file));
            }
        }
    }
    return TRUE;
}
#endif

namespace stxlib_allocator_exit {

u32 start_workers(const u32 count) noexcept {
    u32 started = 0;
    for (u32 index = 0; index < count; ++index) {
        std::thread thread(worker, index * 13 + 1);
        thread.detach();
        started = started + 1;
    }
    return started;
}

void arm_detach_check(const char* const path) noexcept {
    const usize length = std::strlen(path);
    if (length >= marker_capacity) std::abort();
    std::memcpy(detach_check.marker, path, length + 1);
}

} // namespace stxlib_allocator_exit
