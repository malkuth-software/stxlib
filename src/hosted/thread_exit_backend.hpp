#pragma once

// The thread-exit hook stxlib_state uses to release a thread's block
// (src/state/state.cpp; notes planning/stages/E-thread-records.md, section
// 3). Each family supplies it in posix/thread_exit_posix.cpp or
// windows/thread_exit_windows.cpp, compiled into stxlib_state, not the
// boundary: the hook and the state it releases live in one module.
//
// The hook calls state::detail::end_thread(block) on the exiting thread, at
// a normal thread exit only: never at process termination (rule 1 as
// reworded), never for a thread killed by TerminateThread.

namespace stxlib::thread_exit_backend {

// Arms the hook for the calling thread's new block. False when the host
// gave no hook (POSIX: pthread_key_create failed), in which case the caller
// does not keep the block. Calls nothing that can exit early
// (pthread_key_create, pthread_setspecific, GetModuleHandleExW: none is a
// cancellation point or takes a callback).
[[nodiscard]] bool arm(void* block) noexcept;

// True on a thread whose hook has run (its block is released) while the
// host may still run more thread-exit code on it. POSIX hosts that may
// free thread_local storage early (macOS dyld, see state.cpp, end_thread):
// the key's value is a mark from then on, for the remaining destructor
// rounds, so a later key destructor that reaches STXlib finds the thread
// ended even when the anchor's storage was freed and made again. glibc and
// Windows: false; the anchor's storage outlives the hook, so the anchor
// itself says ended (thread_exit_posix.cpp says why glibc sets no mark).
// Calls nothing that can exit early.
[[nodiscard]] bool ended() noexcept;

// The block the calling thread armed whose hook has not yet run, or null.
// The anchor is a cache of this: where the host frees thread_local storage
// before the hook (macOS dyld, from a key destructor of its own; state.cpp,
// end_thread), the anchor reads as a fresh thread's while the thread still
// has a block, and block_for_thread adopts this one instead of making a
// second (the security audit's A-stxlib-allocator-state-01-F3). POSIX: the
// key's value unless it is the ended mark. Windows: the armed block, kept
// in the same thread-local storage as the anchor. Calls nothing that can
// exit early.
[[nodiscard]] void* armed() noexcept;

// True once the process has begun to end in a way that stops its other
// threads where they stand (Windows: ExitProcess, before any module's
// DLL_PROCESS_DETACH). A stopped thread may hold one of stxlib_state's
// locks for ever, so from then on the state takes none (state.cpp,
// "Process end"). False for the whole run on POSIX hosts and Emscripten,
// whose exit leaves the other threads running. Takes no lock; calls
// nothing that can wait or exit early.
#if defined(_WIN32)
[[nodiscard]] bool process_ending() noexcept;
#else
[[nodiscard]] inline bool process_ending() noexcept { return false; }
#endif

} // namespace stxlib::thread_exit_backend

namespace stxlib::state::detail {

void end_thread(void* block) noexcept;

} // namespace stxlib::state::detail
