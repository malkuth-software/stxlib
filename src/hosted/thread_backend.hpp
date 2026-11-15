#pragma once

// The primitives each family's thread module supplies
// (posix/threads_posix.cpp, windows/threads_windows.cpp); exactly one is
// linked. core/threads.cpp builds include/stxlib/threads.hpp on them.
//
// Storage: a mutex or condition lives in 64 bytes aligned to 16, the public
// types' private storage; each family checks with static_assert that its
// host object fits. Defects (a host error in a lock, an unlock, a wait or a
// notify) are reported by the family through fail_fast; these functions
// return only on success.
//
// A thread handle is the host's thread (pthread_t, or the HANDLE
// _beginthreadex returned) in a u64.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/error.hpp"
#include "stxlib/types.hpp"

namespace stxlib::thread_backend {

inline constexpr usize storage_bytes = 64;
inline constexpr usize storage_alignment = 16;

STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void mutex_create(unsigned char* storage) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void mutex_destroy(unsigned char* storage) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void mutex_lock(unsigned char* storage) noexcept;
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT bool mutex_try_lock(unsigned char* storage) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void mutex_unlock(unsigned char* storage) noexcept;

STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void condition_create(unsigned char* storage) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void condition_destroy(unsigned char* storage) noexcept;
// Waits on the condition with the held mutex, which is held again on return.
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL void condition_wait(unsigned char* condition, unsigned char* mutex) noexcept;
// As condition_wait for at most `interval` (positive). True when the host
// reports a timeout; core decides by the clock whether the deadline passed.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL bool condition_wait_for(unsigned char* condition, unsigned char* mutex, Duration interval) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void condition_notify_one(unsigned char* condition) noexcept;
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT void condition_notify_all(unsigned char* condition) noexcept;

// Starts a host thread running run_started_thread(argument), with a stack
// of `stack_bytes` (within threads.hpp's bounds) rounded up to the host's
// page size, and a guard page below it (owner decision D11). On failure,
// `failure` holds the host's code and nothing was started.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT bool thread_start(void* argument, usize stack_bytes, u64& handle, ForeignCode& failure) noexcept;
// The started thread's body, defined by core/threads.cpp.
void run_started_thread(void* argument) noexcept;
// Waits for the thread to end and releases the handle.
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL void thread_join(u64 handle) noexcept;

// Blocks for at least `interval` (positive), resuming after interruptions.
STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_CANCEL void sleep_for(Duration interval) noexcept;
// The host's count of processors that can run threads; 0 when it cannot say.
[[nodiscard]] STXLIB_EARLY_EXIT_FOREIGN STXLIB_EARLY_EXIT_NO_EXIT u32 processor_count() noexcept;

} // namespace stxlib::thread_backend
