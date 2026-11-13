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
// Survey D11 (threads) is open; this is stx_auxlib's current behaviour,
// flagged where D11 would change it:
// - Threads take the host's default stack size (pthread_create without
//   attributes; _beginthreadex with 0): 8 MiB from RLIMIT_STACK on glibc,
//   128 KiB on musl, 512 KiB for macOS's secondary threads, 1 MiB reserved
//   on Windows (D11's table). D11 a and b would name a size.
// - Locking a Mutex its thread already holds is a defect that is not
//   detected: the host's default mutex (PTHREAD_MUTEX_DEFAULT, an SRW lock
//   on Windows) deadlocks or is undefined. D11 a would trap it in check
//   builds.
// - No thread-local storage with a destructor: a thread's identity lives in
//   stxlib_state's thread state, trivially destructible (as D11 a states).
//
// Changes from stx_auxlib: std::thread, std::mutex and
// std::condition_variable are not used (STXlib builds without exceptions,
// so they could not report a failed start); each family module calls the
// host (src/hosted/<family>/threads_<family>.cpp). Results are Result and
// Status. Thread identities and their issuer live in stxlib_state, so every
// module sees one identity per thread. Times are MonotonicTime and Duration.
//
// Modules: a Thread's shared state is a block of STXlib's allocator, so a
// Thread is destroyed (or joined and destroyed) in the module that started
// it, as every allocator block is released in the module that made it
// (memory.hpp).

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
void sleep_until(MonotonicTime deadline) noexcept;
// Offers the rest of the calling thread's time slice to other threads.
void yield_thread() noexcept;
// The number of threads the host can run at once; at least 1 (when the host
// reports none).
[[nodiscard]] u32 processor_count() noexcept;

class ConditionVariable;

// A non-recursive mutual-exclusion lock. Locking it twice on one thread, or
// unlocking it from a thread that does not hold it, is a defect (D11,
// above). Prefer ScopedLock.
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
};

// Holds a Mutex for its own lifetime. A condition wait releases and
// reacquires it, and it is held again when the wait returns.
class ScopedLock final {
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

enum class WaitResult : u8 {
    // Woken by a notification or spuriously: recheck the waited-for state.
    woken = 0,
    timed_out = 1,
};

// Waits are always made under a ScopedLock of the mutex that guards the
// waited-for state, in a loop that rechecks that state, since a wait may end
// spuriously. A timed wait reports timed_out only once MonotonicTime::now()
// has reached the deadline.
class ConditionVariable final {
public:
    ConditionVariable() noexcept;
    ~ConditionVariable();
    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;
    ConditionVariable(ConditionVariable&&) = delete;
    ConditionVariable& operator=(ConditionVariable&&) = delete;

    void wait(ScopedLock& lock) noexcept;
    [[nodiscard]] WaitResult wait_until(ScopedLock& lock, MonotonicTime deadline) noexcept;
    // The deadline is now plus `interval`, saturating.
    [[nodiscard]] WaitResult wait_for(ScopedLock& lock, Duration interval) noexcept;
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
// Failures of start: invalid_argument (a null entry); out_of_memory (the
// shared state); limit_exceeded with the host's code as the foreign code
// when the host refuses another thread; limit_exceeded when thread
// identities are exhausted.
class Thread final {
public:
    using Entry = void (*)(const StopSignal& stop, void* context) noexcept;

    [[nodiscard]] static Result<Thread> start(Entry entry, void* context) noexcept;
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
