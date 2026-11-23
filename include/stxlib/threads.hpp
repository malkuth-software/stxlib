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
// host (src/hosted/<family>/threads_<family>.cpp). Results are OperationResult and
// OperationStatus. Thread identities and their issuer live in stxlib_state, so every
// module sees one identity per thread. Times are IntervalTime and Duration.
//
// Thread records (thread_records.hpp): a thread STXlib starts (a Thread, so
// also every AtomicPool worker) ends its records with end_thread_records as
// the last act of its body, on the thread, after the entry returns. The
// release callbacks then run outside the loader lock that a Windows
// thread's exit hook runs under, and the exit hook after them releases
// nothing again. This was GUI.Forms' gui_forms_atomic_pool_thread_ending
// for its own pool's threads, moved here with the pool (augmentation,
// 2026-11-16).
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

//
// Additions for the shield (owner-approved, 2026-11-22: the threading
// pieces GUI.Forms still took from std): UniqueLock, RecursiveMutex and
// RecursiveLock, MultiLock below; AtomicWord and its waits in
// atomic_word.hpp; StopSource, StopToken and StopCallback in
// stop_token.hpp. RecursiveMutex does not relax D11 for Mutex: re-locking a
// Mutex stays a defect, and a recursive lock is a separate type chosen by
// name.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/clocks.hpp"
#include "stxlib/identity.hpp"
#include "stxlib/require.hpp"
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
// The most threads the program can have running at once, every thread of
// it counted (STXlib's, the program's and any a library starts), when the
// host fixes that number in advance; 0 when it does not. Only the
// Emscripten family fixes it, in a browser: threads come from a pool of
// Web Workers made at start (Capability::fixed_thread_pool), and
// Thread::start fails with limit_exceeded (the host's EAGAIN as the
// foreign code) once all are in use, rather than wait for a new one. A
// program sizes its pools (thread_pool.hpp) within it. Node and the
// native families answer 0.
[[nodiscard]] u32 thread_pool_limit() noexcept;

// Whether the calling thread is the process's initial thread, the one that
// entered main (Capability::main_thread; replaces GUI.Forms' direct
// pthread_main_np, A0 inventory section 2). macOS and FreeBSD answer with
// pthread_main_np (FreeBSD: lib/libthr/thread/thr_main_np.c:37-50 at the
// pin); Linux compares the thread's kernel identifier with the process's
// (the initial thread's identifier is the process identifier, gettid(2);
// unconfirmed at a pin). Windows has no documented way to name the initial
// thread, so it fails there with unsupported; a program that needs the
// answer records its own main thread's ThreadIdentity on entry. Emscripten
// answers unsupported too: under PROXY_TO_PTHREAD main runs on a worker
// the runtime does not name (core/capabilities.cpp).
[[nodiscard]] OperationResult<bool> is_main_thread() noexcept;

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

// A Mutex held for a scope that may let it go and take it again before the
// scope ends, or hand it on (std::unique_lock's replacement; the shield's
// item 4, 2026-11-22). Three states, one at a time:
//   empty    no mutex (default, moved from, or released);
//   unlocked a mutex, not held by this lock;
//   locked   a mutex, held by this lock.
// The destructor unlocks in the locked state. A misuse of the lock's own
// state (lock or try_lock while locked or empty, unlock while not locked)
// is a failed requirement in every build: the state is this object's, so
// the check costs one comparison. Misuse of the Mutex itself (locking it on
// a thread that holds it through another lock) is the Mutex's defect,
// trapped in check builds (D11). One UniqueLock is used by one thread at a
// time, and only the thread that locked it unlocks it.
// ic: stxlib::UniqueLock: generated from include/stxlib/unique_lock.ic.toml by tools/ic_declarations.py; edit the
// ic: declaration, not these lines. States: empty (initial), unlocked, locked. Observed by owns(): empty false,
// ic: unlocked false, locked true. Thread: owner, unless a call says otherwise.
class STXLIB_EARLY_EXIT_LOCK UniqueLock final {
public:
    // Empty.
    // ic: Returns UniqueLock in empty.
    UniqueLock() noexcept = default;
    // Locks `mutex`: the locked state.
    // ic: Returns UniqueLock in locked.
    explicit UniqueLock(Mutex& mutex) noexcept : mutex_(&mutex), owns_(true) { mutex.lock(); }
    // Names `mutex` without taking it: the unlocked state.
    // ic: Returns UniqueLock in unlocked, owned by the caller.
    [[nodiscard]] static UniqueLock deferred(Mutex& mutex) noexcept {
        UniqueLock result(&mutex);
        return result;
    }
    ~UniqueLock() {
        if (owns_) (*mutex_).unlock();
    }
    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;
    // Moving transfers the mutex and its state; the source is left empty.
    // ic: Transfers the mutex and its state from other to the new lock.
    UniqueLock(UniqueLock&& other) noexcept : mutex_(other.mutex_), owns_(other.owns_) {
        other.mutex_ = nullptr;
        other.owns_ = false;
    }
    // Unlocks this lock's own mutex first when it holds it.
    // ic: Transfers the mutex and its state from other to this lock.
    UniqueLock& operator=(UniqueLock&& other) noexcept {
        if (this == &other) return *this;
        Mutex* const held = owns_ ? mutex_ : nullptr;
        mutex_ = other.mutex_;
        owns_ = other.owns_;
        other.mutex_ = nullptr;
        other.owns_ = false;
        if (held != nullptr) (*held).unlock();
        return *this;
    }

    // unlocked -> locked.
    // ic: Admitted in unlocked; then locked. In empty, locked: requirement, unchanged.
    void lock() noexcept {
        STXLIB_REQUIRE(mutex_ != nullptr, "UniqueLock::lock on an empty lock");
        STXLIB_REQUIRE(!owns_, "UniqueLock::lock while it holds its mutex");
        (*mutex_).lock();
        owns_ = true;
    }
    // unlocked -> locked when the mutex was free; true then.
    // ic: Admitted in unlocked. In empty, locked: requirement, unchanged. Returns bool.
    [[nodiscard]] bool try_lock() noexcept {
        STXLIB_REQUIRE(mutex_ != nullptr, "UniqueLock::try_lock on an empty lock");
        STXLIB_REQUIRE(!owns_, "UniqueLock::try_lock while it holds its mutex");
        owns_ = (*mutex_).try_lock();
        return owns_;
    }
    // locked -> unlocked.
    // ic: Admitted in locked; then unlocked. In empty, unlocked: requirement, unchanged.
    void unlock() noexcept {
        STXLIB_REQUIRE(owns_, "UniqueLock::unlock while it does not hold its mutex");
        owns_ = false;
        (*mutex_).unlock();
    }
    // Any state -> empty, without unlocking: returns the mutex (null when
    // empty), which the caller now unlocks itself if owns() was true. Read
    // owns() first.
    // ic: Admitted in any state; then empty. Returns Mutex*. Transfers the duty to unlock a held mutex from this lock
    // ic: to caller.
    [[nodiscard]] Mutex* release() noexcept {
        Mutex* const result = mutex_;
        mutex_ = nullptr;
        owns_ = false;
        return result;
    }
    // True in the locked state.
    // ic: Returns bool.
    [[nodiscard]] bool owns() const noexcept { return owns_; }
    // The named mutex, null when empty. Borrowed: the mutex is the caller's.
    // ic: Returns a borrow of the named mutex.
    [[nodiscard]] Mutex* mutex() const noexcept { return mutex_; }

private:
    friend class ConditionVariable;
    explicit UniqueLock(Mutex* const mutex) noexcept : mutex_(mutex), owns_(false) {}
    // owns_ is true only while mutex_ is not null; every member keeps it so.
    Mutex* mutex_{nullptr};
    bool owns_{false};
};

// A mutual-exclusion lock that its holder may take again (std::recursive_
// mutex's replacement; the shield's item 2, 2026-11-22, for GUI.Forms' C
// ABI, whose operations re-enter through callbacks). D11 makes re-locking a
// Mutex a defect; a RecursiveMutex is the explicit, separate choice where
// re-entry is the design, and nothing converts one into the other.
//
// The holder may lock it again up to maximum_recursive_depth times in all;
// each lock is matched by one unlock, and the last unlock frees it.
// Unlocking it on a thread that does not hold it, exceeding the depth, and
// destroying it while held are defects that fail fast in every build (the
// holder's identity is kept anyway, so the check costs nothing extra). It
// cannot be waited on with a ConditionVariable: a wait would release only
// one level. Prefer RecursiveLock.
inline constexpr u32 maximum_recursive_depth = 0xffffffffU;

// ic: stxlib::RecursiveMutex: generated from include/stxlib/recursive_mutex.ic.toml by tools/ic_declarations.py; edit
// ic: the declaration, not these lines. States: free (initial), held. Observed by held_by_current_thread(): free false,
// ic: held true. Thread: any, unless a call says otherwise.
class RecursiveMutex final {
public:
    RecursiveMutex() noexcept = default;
    ~RecursiveMutex();
    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;
    RecursiveMutex(RecursiveMutex&&) = delete;
    RecursiveMutex& operator=(RecursiveMutex&&) = delete;

    // ic: Admitted in any state; then held.
    void lock() noexcept;
    // Succeeds at once for the holder; for another thread, when it is free.
    // ic: Returns bool.
    [[nodiscard]] bool try_lock() noexcept;
    // ic: Admitted in held. In free: requirement, unchanged.
    void unlock() noexcept;
    // True when the calling thread holds it.
    // ic: Returns bool.
    [[nodiscard]] bool held_by_current_thread() const noexcept;

private:
    Mutex mutex_{};
    // The holder's ThreadIdentity value, 0 when free. Written only by the
    // holder (set after taking mutex_, cleared before releasing it) and read
    // atomically by any thread, which can see its own identity there only if
    // it wrote it.
    u64 holder_{0};
    // Locks held by the holder; read and written only by the holder.
    u32 depth_{0};
};

// Holds a RecursiveMutex for its own lifetime (one level of it).
class STXLIB_EARLY_EXIT_LOCK RecursiveLock final {
public:
    explicit RecursiveLock(RecursiveMutex& mutex) noexcept : mutex_(mutex) { mutex_.lock(); }
    ~RecursiveLock() { mutex_.unlock(); }
    RecursiveLock(const RecursiveLock&) = delete;
    RecursiveLock& operator=(const RecursiveLock&) = delete;
    RecursiveLock(RecursiveLock&&) = delete;
    RecursiveLock& operator=(RecursiveLock&&) = delete;

private:
    RecursiveMutex& mutex_;
};

namespace detail {

// The mutexes MultiLock admits (house style, section 7: explicit
// participation, never a coincidentally named lock member).
template <typename Lockable>
struct MultiLockable final {
    static constexpr bool admitted = false;
    static constexpr bool recursive = false;
};
template <>
struct MultiLockable<Mutex> final {
    static constexpr bool admitted = true;
    static constexpr bool recursive = false;
};
template <>
struct MultiLockable<RecursiveMutex> final {
    static constexpr bool admitted = true;
    static constexpr bool recursive = true;
};

} // namespace detail

// The most mutexes one MultiLock takes.
inline constexpr usize maximum_multi_lock_count = 8;

// Holds several mutexes of one kind for its own lifetime, taken without
// deadlock against every other MultiLock (std::scoped_lock over several
// mutexes; the shield's item 2). The mutexes are taken in increasing order
// of address and released in the reverse order, so two MultiLocks that
// share mutexes always take the shared ones in the same order, whatever
// order their callers named them in. This is the whole deadlock argument:
// it holds against other MultiLocks and against any code that also takes
// these mutexes in address order, not against a thread that already holds
// one of them and waits for another (no algorithm can serve that one).
//
// Naming one Mutex twice is a failed requirement (it would be locked twice,
// D11's defect); naming one RecursiveMutex twice takes it twice, as its own
// recursion allows. Spelled with its kind and count:
//   const MultiLock<RecursiveMutex, 2> both(left.operation_mutex, right.operation_mutex);
template <typename Lockable, usize lock_count>
class STXLIB_EARLY_EXIT_LOCK MultiLock final {
    static_assert(detail::MultiLockable<Lockable>::admitted, "MultiLock takes Mutex or RecursiveMutex");
    static_assert(lock_count >= 2 && lock_count <= maximum_multi_lock_count,
                  "MultiLock takes 2 to maximum_multi_lock_count mutexes; one takes ScopedLock or RecursiveLock");

public:
    template <typename... Lockables>
    explicit MultiLock(Lockables&... mutexes) noexcept : ordered_{&mutexes...} {
        static_assert(sizeof...(Lockables) == lock_count, "MultiLock: name exactly lock_count mutexes");
        order_by_address();
        for (usize index = 0; index < lock_count; ++index) (*ordered_[index]).lock();
    }
    ~MultiLock() {
        for (usize index = lock_count; index > 0; --index) (*ordered_[index - 1]).unlock();
    }
    MultiLock(const MultiLock&) = delete;
    MultiLock& operator=(const MultiLock&) = delete;
    MultiLock(MultiLock&&) = delete;
    MultiLock& operator=(MultiLock&&) = delete;

private:
    // Insertion sort by address (at most eight entries), compared as uptr:
    // the relational operators on unrelated pointers are unspecified.
    void order_by_address() noexcept {
        for (usize index = 1; index < lock_count; ++index) {
            Lockable* const moving = ordered_[index];
            const uptr address = reinterpret_cast<uptr>(moving);
            usize slot = index;
            while (slot > 0 && reinterpret_cast<uptr>(ordered_[slot - 1]) > address) {
                ordered_[slot] = ordered_[slot - 1];
                slot = slot - 1;
            }
            ordered_[slot] = moving;
        }
        if constexpr (!detail::MultiLockable<Lockable>::recursive) {
            for (usize index = 1; index < lock_count; ++index) {
                STXLIB_REQUIRE(ordered_[index - 1] != ordered_[index], "MultiLock: one Mutex named twice");
            }
        }
    }

    // The mutexes in the order they are taken. Borrowed: each is the
    // caller's and must outlive this lock.
    Lockable* ordered_[lock_count];
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
    // The same waits under a UniqueLock, which must hold its mutex (a failed
    // requirement otherwise) and holds it again when the wait returns.
    void wait(UniqueLock& lock, WaitPredicate predicate, const void* context) noexcept;
    [[nodiscard]] WaitResult wait_until(UniqueLock& lock, WaitPredicate predicate, const void* context,
                                        IntervalTime deadline) noexcept;
    [[nodiscard]] WaitResult wait_for(UniqueLock& lock, WaitPredicate predicate, const void* context,
                                      Duration interval) noexcept;
    void notify_one() noexcept;
    void notify_all() noexcept;

private:
    void wait_held(Mutex& mutex, WaitPredicate predicate, const void* context) noexcept;
    [[nodiscard]] WaitResult wait_held_until(Mutex& mutex, WaitPredicate predicate, const void* context,
                                             IntervalTime deadline) noexcept;
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

enum class AbandonResult : u8 {
    // The thread runs on, unowned; it releases its own shared state when its
    // entry returns.
    abandoned = 0,
    // The entry had already returned: the thread was joined (its body had
    // only its last steps left) and its state released here.
    already_finished = 1,
    // An empty thread, or one already joined.
    not_joinable = 2,
};

// One started thread, owned. Its entry receives the thread's StopSignal and
// the context it was started with, and must not throw (it is noexcept: an
// exception ends the process). The context is borrowed until the thread is
// joined: declare the Thread after what its context points to, so it is
// joined first.
//
// Destruction and move assignment request a stop and join, like
// std::jthread; a thread outlives its owner only when the owner abandons it
// (abandon, below). Moving transfers the
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
    [[nodiscard]] static OperationResult<Thread> start(Entry entry, void* context,
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
    // Gives the thread up instead of joining it: the host thread is
    // detached and runs on until its entry returns, and the process's exit
    // ends it if it never does. Afterwards this Thread is empty (no
    // identity; join answers not_joinable). No stop is requested. The
    // context stays borrowed by the abandoned thread until its entry
    // returns, so the caller hands it state the entry owns or that outlives
    // the thread, and the entry releases what it owns. Only the owner calls
    // it; a thread may abandon its own Thread. The shared state's ownership
    // passes by one atomic exchange with the thread's end, so exactly one of
    // the two releases it.
    [[nodiscard]] AbandonResult abandon() noexcept;

    // The started thread's shared state, defined in src/core/threads.cpp.
    struct State;

private:
    explicit Thread(State* const state) noexcept : state_(state) {}
    State* state_{nullptr};
};

} // namespace stxlib
