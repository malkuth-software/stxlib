// The Windows thread module (hosted/thread_backend.hpp): slim
// reader/writer locks held exclusively, condition variables, and
// _beginthreadex (the C runtime's thread start, which sets up the runtime's
// per-thread state where CreateThread does not; libc++ starts its
// std::thread with it). stx_auxlib's src/hosted/threads.cpp at b6e330a used
// std::thread, std::mutex and std::condition_variable; these are the host
// objects under them, called directly so a failed start is a result in a
// build without exceptions.
//
// Owner decision D11 = b (threads.hpp): _beginthreadex is given the stack
// size with STACK_SIZE_PARAM_IS_A_RESERVATION, so the size is the stack's
// reservation (rounded by the system to its allocation granularity) rather
// than its initial commit. Windows places a guard page below the committed
// part of every thread stack and moves it as the stack grows; overflowing
// the reservation raises EXCEPTION_STACK_OVERFLOW (Microsoft's "Thread
// Stack Size" and CreateThread pages; from the published pages, not re-read
// in this batch).
//
// An SRW lock and a condition variable need no destruction (Microsoft's
// documentation of InitializeSRWLock and InitializeConditionVariable: there
// is no delete call), and their initialization cannot fail. Waits take
// milliseconds, rounded up so a wait never ends before its interval.
// processor_count is libc++'s std::thread::hardware_concurrency on Windows:
// GetNativeSystemInfo's dwNumberOfProcessors (libc++ src/thread.cpp; from
// memory of that file, not re-read). (Microsoft statements here are from
// the published pages, not re-read in this batch.)

#include "hosted/thread_backend.hpp"

#include "stxlib/fail_fast.hpp"

#include <cstdint>
#include <new>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>

namespace stxlib::thread_backend {

namespace {

static_assert(sizeof(SRWLOCK) <= storage_bytes && alignof(SRWLOCK) <= storage_alignment,
              "the public Mutex storage is too small for SRWLOCK");
static_assert(sizeof(CONDITION_VARIABLE) <= storage_bytes && alignof(CONDITION_VARIABLE) <= storage_alignment,
              "the public ConditionVariable storage is too small for CONDITION_VARIABLE");
static_assert(sizeof(HANDLE) <= sizeof(u64), "a thread handle does not fit a u64");

[[noreturn]] void defect(const TextView message, const DWORD error) noexcept {
    const ErrorRecord record = ErrorRecord::make(
        ErrorKind::internal_failure, "stxlib.threads",
        ErrorDetail{.foreign = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error)),
                    .message = message});
    fail_fast(record);
}

SRWLOCK* lock_of(unsigned char* const storage) noexcept {
    SRWLOCK* const result = std::launder(reinterpret_cast<SRWLOCK*>(storage));
    return result;
}

CONDITION_VARIABLE* condition_of(unsigned char* const storage) noexcept {
    CONDITION_VARIABLE* const result = std::launder(reinterpret_cast<CONDITION_VARIABLE*>(storage));
    return result;
}

unsigned __stdcall thread_entry(void* const argument) {
    run_started_thread(argument);
    return 0;
}

// Milliseconds of `interval`, rounded up, below INFINITE (which would never
// time out); a longer wait ends early and core waits again.
DWORD milliseconds_of(const Duration interval) noexcept {
    const i64 nanoseconds = interval.count_nanoseconds();
    const i64 whole = nanoseconds / 1000000;
    const i64 rounded = nanoseconds % 1000000 == 0 ? whole : whole + 1;
    constexpr i64 longest = static_cast<i64>(INFINITE) - 1;
    const DWORD result = static_cast<DWORD>(rounded > longest ? longest : rounded);
    return result;
}

} // namespace

void mutex_create(unsigned char* const storage) noexcept {
    SRWLOCK* const lock = ::new (static_cast<void*>(storage)) SRWLOCK;
    InitializeSRWLock(lock);
}

void mutex_destroy(unsigned char* const storage) noexcept { static_cast<void>(storage); }

void mutex_lock(unsigned char* const storage) noexcept { AcquireSRWLockExclusive(lock_of(storage)); }

bool mutex_try_lock(unsigned char* const storage) noexcept {
    const bool result = TryAcquireSRWLockExclusive(lock_of(storage)) != 0;
    return result;
}

void mutex_unlock(unsigned char* const storage) noexcept { ReleaseSRWLockExclusive(lock_of(storage)); }

void condition_create(unsigned char* const storage) noexcept {
    CONDITION_VARIABLE* const condition = ::new (static_cast<void*>(storage)) CONDITION_VARIABLE;
    InitializeConditionVariable(condition);
}

void condition_destroy(unsigned char* const storage) noexcept { static_cast<void>(storage); }

void condition_wait(unsigned char* const condition, unsigned char* const mutex) noexcept {
    if (!SleepConditionVariableSRW(condition_of(condition), lock_of(mutex), INFINITE, 0)) {
        defect("SleepConditionVariableSRW failed", GetLastError());
    }
}

bool condition_wait_for(unsigned char* const condition, unsigned char* const mutex, const Duration interval) noexcept {
    if (SleepConditionVariableSRW(condition_of(condition), lock_of(mutex), milliseconds_of(interval), 0)) return false;
    const DWORD error = GetLastError();
    if (error == ERROR_TIMEOUT) return true;
    defect("SleepConditionVariableSRW failed", error);
}

void condition_notify_one(unsigned char* const condition) noexcept { WakeConditionVariable(condition_of(condition)); }

void condition_notify_all(unsigned char* const condition) noexcept {
    WakeAllConditionVariable(condition_of(condition));
}

bool thread_start(void* const argument, const usize stack_bytes, u64& handle, ForeignCode& failure) noexcept {
    // stack_bytes is at most 1 GiB (threads.hpp), so it fits unsigned.
    const uintptr_t started = _beginthreadex(nullptr, static_cast<unsigned>(stack_bytes), thread_entry, argument,
                                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (started == 0) {
        // _beginthreadex sets errno (EAGAIN, EINVAL, EACCES) and leaves the
        // Win32 error from CreateThread (Microsoft's _beginthreadex page).
        failure = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError()));
        return false;
    }
    handle = static_cast<u64>(started);
    return true;
}

void thread_join(const u64 handle) noexcept {
    const HANDLE thread = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    if (WaitForSingleObject(thread, INFINITE) != WAIT_OBJECT_0) defect("WaitForSingleObject failed", GetLastError());
    if (!CloseHandle(thread)) defect("CloseHandle failed", GetLastError());
}

void sleep_for(const Duration interval) noexcept { Sleep(milliseconds_of(interval)); }

u32 processor_count() noexcept {
    SYSTEM_INFO information{};
    GetNativeSystemInfo(&information);
    const u32 result = static_cast<u32>(information.dwNumberOfProcessors);
    return result;
}

} // namespace stxlib::thread_backend
