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
// host may still run more thread-exit code on it. POSIX: the key's value
// is a mark from then on, for the remaining destructor rounds, so a later
// key destructor that reaches STXlib finds the thread ended even when the
// host has freed and made again the anchor's storage (macOS dyld, see
// state.cpp, end_thread). Windows: false; the anchor's storage outlives the
// TLS callback. Calls nothing that can exit early.
[[nodiscard]] bool ended() noexcept;

} // namespace stxlib::thread_exit_backend

namespace stxlib::state::detail {

void end_thread(void* block) noexcept;

} // namespace stxlib::state::detail
