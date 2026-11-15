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

void NTAPI thread_exit_callback(PVOID, const DWORD reason, PVOID) {
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
