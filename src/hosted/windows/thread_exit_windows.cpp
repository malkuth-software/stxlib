// The Windows thread-exit hook (hosted/thread_exit_backend.hpp), compiled
// into stxlib_state: a TLS callback that receives DLL_THREAD_DETACH, in the
// shared and the static build alike, and the layout refusal's write and
// abort.
//
// Microsoft's DllMain page: DLL_THREAD_DETACH means "A thread is exiting
// cleanly", the call is made in the context of the exiting thread, and the
// entry point is called for a terminating thread even if it never had
// DLL_THREAD_ATTACH (threads that existed when the DLL loaded); no
// DLL_THREAD_DETACH is sent at process termination or for TerminateThread,
// so termination releases nothing (rule 1 as reworded). A module's TLS
// callbacks receive the same reasons. The callback sits in .CRT$XLY, as
// mimalloc v3.6.1 (upstream src/prim/windows/prim.c, no longer vendored)
// places its own detach callback.
//
// The hook runs under the loader lock. Microsoft's "Dynamic-Link Library
// Best Practices" lists what never to do there, among it calling functions
// in User32.dll or Gdi32.dll; consumers whose release callbacks need them
// call end_thread_records first (thread_records.hpp). STXlib's own work
// here clears slots, edits the registry under a lock that calls nothing,
// and frees memory.
//
// The shared stxlib_state pins itself on its first armed block, so it is
// never unloaded under a thread whose block it still holds.
//
// Process end (the security audit's A-stxlib-allocator-state-01-F2).
// ExitProcess stops every other thread where it stands and then runs the
// modules' DLL_PROCESS_DETACH code on the exiting thread; a stopped thread
// may hold the registry's or the type table's lock, or the host backend's
// registry lock, for ever. process_ending() says when that has begun, from
// ntdll's RtlDllShutdownInProgress (Microsoft Learn, devnotes: exported by
// ntdll.dll, declared by no SDK header; minimum Windows 10), looked up once
// in this callback's DLL_PROCESS_ATTACH with GetModuleHandleW and
// GetProcAddress (ntdll is always loaded; neither call loads a module).
// It is taken to be true before the first module is detached, so a module
// that depends on stxlib_state and is detached before it is covered
// (unconfirmed: the loader's order is not documented; a Windows run is to
// show it). Where the lookup finds nothing, this module's own
// DLL_PROCESS_DETACH with a non-null lpReserved (DllMain's page: non-null
// when the process is terminating) sets a flag that says the same from
// then on. stx_minmallocpp asks the same question for its own locks
// (minmallocpp_os_windows.cpp).

#include "hosted/thread_exit_backend.hpp"

#include "state/state.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace stxlib {

namespace {

#if defined(STXLIB_STATE_SHARED)
constinit std::atomic<bool> pinned{false};
#endif
constinit thread_local void* armed_block{nullptr};

// RtlDllShutdownInProgress: BOOLEAN NTAPI (void), declared here because no
// SDK header does.
using ShutdownQuery = BOOLEAN(NTAPI*)();

// Set once at process attach; null when ntdll gave nothing.
constinit std::atomic<ShutdownQuery> shutdown_query{nullptr};
// Set at this module's DLL_PROCESS_DETACH when the process is ending.
constinit std::atomic<bool> process_detached{false};

void find_shutdown_query() noexcept {
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) return;
    const FARPROC found = ::GetProcAddress(ntdll, "RtlDllShutdownInProgress");
    if (found == nullptr) return;
    // Through void*, which Windows' function and data pointers share.
    const ShutdownQuery query = reinterpret_cast<ShutdownQuery>(reinterpret_cast<void*>(found));
    shutdown_query.store(query, std::memory_order_release);
}

void NTAPI thread_exit_callback(PVOID, const DWORD reason, PVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        find_shutdown_query();
        return;
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (reserved != nullptr) process_detached.store(true, std::memory_order_release);
        return;
    }
    if (reason != DLL_THREAD_DETACH) return;
    void* const block = armed_block;
    if (block == nullptr) return;
    armed_block = nullptr;
    state::detail::end_thread(block);
}

} // namespace

extern "C" {
#if defined(_MSC_VER)
#if defined(_WIN64)
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:stxlib_thread_exit_callback")
#else
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_stxlib_thread_exit_callback")
#endif
#pragma const_seg(".CRT$XLY")
extern const PIMAGE_TLS_CALLBACK stxlib_thread_exit_callback;
const PIMAGE_TLS_CALLBACK stxlib_thread_exit_callback = &thread_exit_callback;
#pragma const_seg()
#elif defined(__MINGW32__)
extern const IMAGE_TLS_DIRECTORY _tls_used;
__attribute__((used)) static const void* const stxlib_tls_used_reference = &_tls_used;
__attribute__((used, section(".CRT$XLY"))) PIMAGE_TLS_CALLBACK stxlib_thread_exit_callback = &thread_exit_callback;
#endif
}

namespace thread_exit_backend {

bool arm(void* const block) noexcept {
#if defined(STXLIB_STATE_SHARED)
    if (!pinned.load(std::memory_order_acquire)) {
        HMODULE self = nullptr;
        const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        if (::GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&thread_exit_callback), &self) == 0) return false;
        pinned.store(true, std::memory_order_release);
    }
#endif
    armed_block = block;
    return true;
}

// The anchor's storage is live through the DLL_THREAD_DETACH callbacks
// (this file's callback reads armed_block from the same TLS), so after the
// hook the anchor itself says ended; nothing here frees it early, as macOS
// dyld does (state.cpp, end_thread).
bool ended() noexcept { return false; }

void* armed() noexcept {
    void* const result = armed_block;
    return result;
}

bool process_ending() noexcept {
    if (process_detached.load(std::memory_order_acquire)) return true;
    const ShutdownQuery query = shutdown_query.load(std::memory_order_acquire);
    const bool result = query != nullptr && query() != FALSE;
    return result;
}

} // namespace thread_exit_backend

namespace state {

void refuse_layout(const char* const message) noexcept {
    const HANDLE error = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error != nullptr && error != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        static_cast<void>(::WriteFile(error, message, static_cast<DWORD>(std::strlen(message)), &written, nullptr));
    }
    std::abort();
}

} // namespace state

} // namespace stxlib
