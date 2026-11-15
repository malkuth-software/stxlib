#pragma once

// Threads and synchronization (Capability::threads). Ported from
// stx_auxlib's include/stx_auxlib/threads.hpp and src/hosted/threads.cpp at
// b6e330a. No platform type appears here: Mutex and ConditionVariable keep
// their host objects in fixed private storage (the family modules check
// that they fit), and Thread keeps its shared state behind a pointer.
//
// Failure: operations a correct program cannot see fail (a host error in a
// lock, unlock or condition wait, a thread joining itself on destruction)
// are defects, and fail fast (fail_fast.hpp) with internal_failure and the
// host's code; they are not results. Starting a thread can fail for lack of
// resources, and reports it.
//
// Owner decision D11 = b (decisions/2026-11-13-d11-d12-threads-clocks.md):
// - Every thread's stack size is STXlib's, the same on every host: the
//   caller's, or default_thread_stack_bytes (4 MiB) when the caller does
//   not state one; never the host's default (which is 8 MiB from
//   RLIMIT_STACK on glibc, 128 KiB on musl, 512 KiB on macOS and 1 MiB on
//   Windows: the survey's D11 table). Every thread has a guard page below
//   its stack.
// - Locking a Mutex the calling thread already holds, or unlocking one it
//   does not hold, is a defect, trapped in check builds (STXLIB_LOCK_CHECKS,
//   CMakeLists.txt: Debug and sanitizer builds), where it fails fast; in
//   other builds it is undefined (the host's mutex deadlocks or worse).
// - No thread-local storage with a non-trivial destructor in STXlib: a
//   thread's identity lives in stxlib_state's thread state, trivially
//   destructible.
// - Waits take a named predicate (a function, not a lambda), checked under
//   the lock before every wait, so a wait cannot miss a notification or
//   return on a spurious wake-up.
//
// Changes from stx_auxlib: std::thread, std::mutex and
// std::condition_variable are not used (STXlib builds without exceptions,
// so they could not report a failed start); each family module calls the
// host (src/hosted/<family>/threads_<family>.cpp). Results are Result and
// Status. Thread identities and their issuer live in stxlib_state, so every
// module sees one identity per thread. Times are IntervalTime and Duration.
//
// Modules: a Thread's shared state is a block of STXlib's allocator, which
// every module shares (memory.hpp), so a Thread may be started in one of
// STXlib's modules and joined or destroyed in another.
//
// POSIX thread cancellation (owner decision 2026-11-15, notes
// decisions/2026-11-15-audit-e-decisions.md, 2): STXlib acts on a
// cancellation only outside its locked sections. Inside them (the console,
// spawning and reaping children, a thread pool's run, its begin-to-wait
// window, its workers and its destruction, and a trampoline invocation in a
// unit built without exceptions) cancellation is held off, and a
// cancellation requested meanwhile takes effect at the thread's next
// cancellation point after the section. The waits and joins on this page
// stay cancellation points, as POSIX makes them, when the caller holds no
// STXlib lock of its own making: a wait cancelled with the caller's Mutex
// held re-takes it, and the caller's ScopedLock releases it as the stack
// unwinds in a unit built with exceptions (in a check build the lock
// checks then report the release as a defect and fail fast). A gate that
// a cancellation unwinds into fails fast (gate.hpp). Windows has no thread
// cancellation.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/identity.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

struct ThreadTag;
// A thread's identity. Each thread receives a distinct value on first use;
// a value is never reused, even after its thread ends. The default value
// names no thread.
using ThreadIdentity = Identity<ThreadTag>;

// The calling thread's identity, issued on its first call. The issuer is
// process-wide (stxlib_state); exhausting 2^64 - 1 identities fails fast.
[[nodiscard]] ThreadIdentity current_thread() noexcept;

// Blocks the calling thread for at least `interval`, or until `deadline`; a
// past deadline or a non-positive interval returns at once.
void sleep_for(Duration interval) noexcept;
void sleep_until(IntervalTime deadline) noexcept;
// Offers the rest of the calling thread's time slice to other threads.
void yield_thread() noexcept;
// The number of threads the host can run at once; at least 1 (when the host
// reports none).
[[nodiscard]] u32 processor_count() noexcept;

// D11's stack sizes, in bytes. A caller's size is rounded up to the host's
// page size; a size outside [minimum, maximum] is refused by Thread::start.
// The minimum is above every phase I host's PTHREAD_STACK_MIN (glibc on
// arm64 needs 128 KiB, finding 20).
inline constexpr usize default_thread_stack_bytes = 4194304;  // 4 MiB
inline constexpr usize minimum_thread_stack_bytes = 262144;   // 256 KiB
inline constexpr usize maximum_thread_stack_bytes = 1073741824; // 1 GiB

// True when this build traps recursive locking and foreign unlocks.
[[nodiscard]] bool lock_checks_enabled() noexcept;

class ConditionVariable;

// A non-recursive mutual-exclusion lock. Locking it twice on one thread, or
// unlocking it from a thread that does not hold it, is a defect (D11,
// above), trapped in check builds. Prefer ScopedLock.
class Mutex final {
public:
    Mutex() noexcept;
    ~Mutex();
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock() noexcept;
    [[nodiscard]] bool try_lock() noexcept;
    void unlock() noexcept;

private:
    friend class ConditionVariable;
    alignas(16) unsigned char storage_[64];
    // The holder's ThreadIdentity value, 0 when free; kept in check builds
    // only (in every build the field exists, so the layout is one).
    u64 owner_{0};
};

// Holds a Mutex for its own lifetime. A condition wait releases and
// reacquires it, and it is held again when the wait returns.
class STXLIB_EARLY_EXIT_LOCK ScopedLock final {
public:
    explicit ScopedLock(Mutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~ScopedLock() { mutex_.unlock(); }
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;
    ScopedLock(ScopedLock&&) = delete;
    ScopedLock& operator=(ScopedLock&&) = delete;

private:
    friend class ConditionVariable;
    Mutex& mutex_;
};

// A named predicate over the waited-for state: true when the wait is over.
// It is called with the lock held, so it reads state that lock guards. It
// must return (AGENTS.md, early exits): no longjmp out of it.
using WaitPredicate = bool (*)(const void* context) noexcept;

enum class WaitResult : u8 {
    // The predicate returned true.
    satisfied = 0,
    // IntervalTime::now() reached the deadline and the predicate was still
    // false.
    timed_out = 1,
};

// Waits are made under a ScopedLock of the mutex that guards the waited-for
// state. Each wait calls the predicate first and returns as soon as it is
// true; otherwise it releases the lock, sleeps until notified (or until
// spuriously woken, or until the deadline), reacquires the lock and calls
// the predicate again. A null predicate is a failed requirement.
class ConditionVariable final {
public:
    ConditionVariable() noexcept;
    ~ConditionVariable();
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    // Returns once the predicate is true.
    void wait(ScopedLock& lock, WaitPredicate predicate, const void* context) noexcept;
    [[nodiscard]] WaitResult wait_until(ScopedLock& lock, WaitPredicate predicate, const void* context,
                                        IntervalTime deadline) noexcept;
    // The deadline is now plus `interval`, saturating.
    [[nodiscard]] WaitResult wait_for(ScopedLock& lock, WaitPredicate predicate, const void* context,
                                      Duration interval) noexcept;
    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    alignas(16) unsigned char storage_[64];
};

class OnceFlag;
void call_once_slow(OnceFlag& flag, void (*entry)(void* context) noexcept, void* context) noexcept;

// Runs an initialization once per flag. Every caller returns only after the
// one call of `entry` has finished; concurrent callers wait for it. `entry`
// must not call call_once on the same flag. Built on Mutex and an atomic
// rather than the C++ runtime's call_once, so that every synchronizing
// operation is visible to thread sanitizers (stx_auxlib's reason: libc++
// publishes call_once's completion inside its uninstrumented runtime).
class OnceFlag final {
public:
    OnceFlag() noexcept = default;
    OnceFlag(const OnceFlag&) = delete;
    OnceFlag& operator=(const OnceFlag&) = delete;
    OnceFlag(OnceFlag&&) = delete;
    OnceFlag& operator=(OnceFlag&&) = delete;

    // True once the initialization has run: call_once's inline fast path,
    // one acquire load (the compiler's atomic built-in, so this header
    // needs no standard header).
    [[nodiscard]] bool done() const noexcept { return __atomic_load_n(&done_, __ATOMIC_ACQUIRE) != 0; }

private:
    friend void call_once_slow(OnceFlag& flag, void (*entry)(void* context) noexcept, void* context) noexcept;
    // 1 once `entry` has returned; stored with release ordering under mutex_.
    u32 done_{0};
    Mutex mutex_{};
};

// Runs `entry` once per flag. `entry` runs under the flag's mutex and must
// return (AGENTS.md, early exits): no longjmp out and no thread exit or
// cancellation inside it, or every later caller waits for ever.
inline void call_once(OnceFlag& flag, void (*const entry)(void* context) noexcept, void* const context) noexcept {
    if (flag.done()) return;
    call_once_slow(flag, entry, context);
}

// A permanent request for a thread to stop, observed cooperatively by its
// entry. A request never interrupts a blocking call. Observing it acquires
// the writes made before the request.
class StopSignal final {
public:
    StopSignal() noexcept = default;
    StopSignal(const StopSignal&) = delete;
    StopSignal& operator=(const StopSignal&) = delete;

    [[nodiscard]] bool requested() const noexcept { return __atomic_load_n(&requested_, __ATOMIC_ACQUIRE) != 0; }
    void request() noexcept { __atomic_store_n(&requested_, 1U, __ATOMIC_RELEASE); }

private:
    u32 requested_{0};
};

enum class JoinResult : u8 {
    joined = 0,
    // An empty thread, or one already joined.
    not_joinable = 1,
    // The thread tried to join itself; nothing was done.
    self_join = 2,
};

// One started thread, owned. Its entry receives the thread's StopSignal and
// the context it was started with, and must not throw (it is noexcept: an
// exception ends the process). The context is borrowed until the thread is
// joined: declare the Thread after what its context points to, so it is
// joined first.
//
// Destruction and move assignment request a stop and join, like
// std::jthread; a thread never outlives its owner. Moving transfers the
// thread and leaves the source empty. identity() is known before the thread
// runs and equals current_thread() inside it.
//
// Failures of start: invalid_argument with a ThreadFailure code (a null
// entry; a stack size outside D11's bounds); out_of_memory (the shared
// state); limit_exceeded with the host's code as the foreign code when the
// host refuses another thread or its stack; limit_exceeded when thread
// identities are exhausted.
enum class ThreadFailure : u32 {
    no_entry = 1,
    stack_size_out_of_range = 2,
};

class Thread final {
public:
    // The thread's body. No longjmp out of it (AGENTS.md, early exits); a
    // thread exit inside it ends the thread as a return would.
    using Entry = void (*)(const StopSignal& stop, void* context) noexcept;

    // The thread's stack is `stack_bytes` rounded up to the host's page
    // size, with a guard page below it (D11).
    [[nodiscard]] static Result<Thread> start(Entry entry, void* context,
                                              usize stack_bytes = default_thread_stack_bytes) noexcept;
    Thread() noexcept = default;
    ~Thread();
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
    Thread& operator=(Thread&& other) noexcept;

    // Empty and joined threads report no identity.
    [[nodiscard]] ThreadIdentity identity() const noexcept;
    // Safe from any thread while the owner keeps the Thread alive.
    void request_stop() noexcept;
    // Blocks until the thread's entry returns. Only the owner calls it.
    [[nodiscard]] JoinResult join() noexcept;

    // The started thread's shared state, defined in src/core/threads.cpp.
    struct State;

private:
    explicit Thread(State* const state) noexcept : state_(state) {}
    State* state_{nullptr};
};

} // namespace stxlib
