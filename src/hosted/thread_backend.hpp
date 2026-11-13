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

#include "stxlib/clocks.hpp"
#include "stxlib/error.hpp"
#include "stxlib/types.hpp"

namespace stxlib::thread_backend {

inline constexpr usize storage_bytes = 64;
inline constexpr usize storage_alignment = 16;

void mutex_create(unsigned char* storage) noexcept;
void mutex_destroy(unsigned char* storage) noexcept;
void mutex_lock(unsigned char* storage) noexcept;
[[nodiscard]] bool mutex_try_lock(unsigned char* storage) noexcept;
void mutex_unlock(unsigned char* storage) noexcept;

void condition_create(unsigned char* storage) noexcept;
void condition_destroy(unsigned char* storage) noexcept;
// Waits on the condition with the held mutex, which is held again on return.
void condition_wait(unsigned char* condition, unsigned char* mutex) noexcept;
// As condition_wait for at most `interval` (positive). True when the host
// reports a timeout; core decides by the clock whether the deadline passed.
[[nodiscard]] bool condition_wait_for(unsigned char* condition, unsigned char* mutex, Duration interval) noexcept;
void condition_notify_one(unsigned char* condition) noexcept;
void condition_notify_all(unsigned char* condition) noexcept;

// Starts a host thread running run_started_thread(argument), with a stack
// of `stack_bytes` (within threads.hpp's bounds) rounded up to the host's
// page size, and a guard page below it (owner decision D11). On failure,
// `failure` holds the host's code and nothing was started.
[[nodiscard]] bool thread_start(void* argument, usize stack_bytes, u64& handle, ForeignCode& failure) noexcept;
// The started thread's body, defined by core/threads.cpp.
void run_started_thread(void* argument) noexcept;
// Waits for the thread to end and releases the handle.
void thread_join(u64 handle) noexcept;

// Blocks for at least `interval` (positive), resuming after interruptions.
void sleep_for(Duration interval) noexcept;
// The host's count of processors that can run threads; 0 when it cannot say.
[[nodiscard]] u32 processor_count() noexcept;

} // namespace stxlib::thread_backend
