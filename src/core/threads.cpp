// Threads and synchronization (include/stxlib/threads.hpp) over the
// family's thread module (hosted/thread_backend.hpp). Ported from
// stx_auxlib's src/hosted/threads.cpp at b6e330a: the same identities,
// lifetime rules and once algorithm, with the host calls moved to the
// family modules and the identity state moved to stxlib_state. Owner
// decision D11 = b on top: STXlib's stack sizes, lock checks in check
// builds (STXLIB_LOCK_CHECKS), and predicate waits.

#include "stxlib/threads.hpp"

#include "stxlib/detail/cancellation.hpp"

#include "hosted/host.hpp"
#include "hosted/thread_backend.hpp"
#include "state/state.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"

#include <atomic>
#include <new>

namespace stxlib {

namespace {

static_assert(sizeof(Mutex) >= thread_backend::storage_bytes && alignof(Mutex) >= thread_backend::storage_alignment,
              "Mutex storage is smaller than the backend's");
static_assert(sizeof(ConditionVariable) >= thread_backend::storage_bytes &&
                  alignof(ConditionVariable) >= thread_backend::storage_alignment,
              "ConditionVariable storage is smaller than the backend's");

[[noreturn]] void identities_exhausted() noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.threads.identity",
                          ErrorDetail{.message = "every thread identity has been issued"});
    fail_fast(record);
}

// Issues the next identity from the process-wide issuer, or 0 when 2^64 - 1
// have been issued (house style, section 3: refuse, never wrap).
u64 issue_identity() noexcept {
    std::atomic<u64>& last = state::current_process().last_thread_identity;
    u64 value = last.load(std::memory_order_relaxed);
    for (;;) {
        if (value == u64_max) return 0;
        if (last.compare_exchange_weak(value, value + 1, std::memory_order_relaxed)) break;
    }
    const u64 result = value + 1;
    return result;
}

} // namespace

ThreadIdentity current_thread() noexcept {
    state::ThreadFlags& thread = state::current_flags();
    if (thread.thread_identity == 0) {
        const u64 issued = issue_identity();
        if (issued == 0) identities_exhausted();
        thread.thread_identity = issued;
    }
    const ThreadIdentity result = ThreadIdentity::from_value(thread.thread_identity);
    return result;
}

// Both sleeps end only once IntervalTime has reached the deadline: a host
// sleep may end early (a Windows sleep is capped below 49.7 days).
void sleep_for(const Duration interval) noexcept {
    if (interval <= Duration::nanoseconds(0)) return;
    sleep_until(IntervalTime::now().after(interval));
}

void sleep_until(const IntervalTime deadline) noexcept {
    for (;;) {
        const IntervalTime now = IntervalTime::now();
        if (!(now < deadline)) return;
        thread_backend::sleep_for(deadline.since(now));
    }
}

void yield_thread() noexcept { host::yield_thread(); }

u32 processor_count() noexcept {
    const u32 reported = thread_backend::processor_count();
    if (reported == 0) return 1;
    return reported;
}

bool lock_checks_enabled() noexcept {
#if defined(STXLIB_LOCK_CHECKS)
    return true;
#else
    return false;
#endif
}

// ---- Mutex and ConditionVariable ---------------------------------------------

namespace {

#if defined(STXLIB_LOCK_CHECKS)

[[noreturn]] void lock_defect(const TextView message) noexcept {
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::internal_failure, "stxlib.threads.lock", ErrorDetail{.message = message});
    fail_fast(record);
}

// owner_ is written by the holder and read by any thread that locks, so it
// is accessed atomically; a reader compares it with its own identity, which
// only it ever stores.
u64 owner_of(u64& owner) noexcept {
    const u64 result = std::atomic_ref<u64>(owner).load(std::memory_order_relaxed);
    return result;
}

void set_owner(u64& owner, const u64 value) noexcept {
    std::atomic_ref<u64>(owner).store(value, std::memory_order_relaxed);
}

// Before taking the lock: the caller must not hold it already.
u64 check_not_held(u64& owner) noexcept {
    const u64 caller = current_thread().value();
    if (owner_of(owner) == caller) lock_defect("a thread locked a Mutex it already holds");
    return caller;
}

// Before releasing the lock: the caller must hold it.
void check_held(u64& owner) noexcept {
    if (owner_of(owner) != current_thread().value()) lock_defect("a thread unlocked a Mutex it does not hold");
}

#endif

void require_predicate(const WaitPredicate predicate) noexcept {
    STXLIB_REQUIRE(predicate != nullptr, "ConditionVariable: a wait needs a predicate");
}

} // namespace

Mutex::Mutex() noexcept { thread_backend::mutex_create(storage_); }

Mutex::~Mutex() {
#if defined(STXLIB_LOCK_CHECKS)
    if (owner_of(owner_) != 0) lock_defect("a Mutex was destroyed while held");
#endif
    thread_backend::mutex_destroy(storage_);
}

void Mutex::lock() noexcept {
#if defined(STXLIB_LOCK_CHECKS)
    const u64 caller = check_not_held(owner_);
    thread_backend::mutex_lock(storage_);
    set_owner(owner_, caller);
#else
    thread_backend::mutex_lock(storage_);
#endif
}

bool Mutex::try_lock() noexcept {
#if defined(STXLIB_LOCK_CHECKS)
    // A try on a mutex the caller holds is the same defect (the host's
    // answer would be EBUSY on one host and EDEADLK or success on another).
    const u64 caller = check_not_held(owner_);
    const bool result = thread_backend::mutex_try_lock(storage_);
    if (result) set_owner(owner_, caller);
#else
    const bool result = thread_backend::mutex_try_lock(storage_);
#endif
    return result;
}

void Mutex::unlock() noexcept {
#if defined(STXLIB_LOCK_CHECKS)
    check_held(owner_);
    set_owner(owner_, 0);
#endif
    thread_backend::mutex_unlock(storage_);
}

ConditionVariable::ConditionVariable() noexcept { thread_backend::condition_create(storage_); }

ConditionVariable::~ConditionVariable() { thread_backend::condition_destroy(storage_); }

namespace {

// One host wait, interval or not. The host releases the mutex for the wait,
// so in check builds the owner is cleared before and restored after. True
// when the host reports a timeout.
bool host_wait(unsigned char* const condition, unsigned char* const mutex_storage, u64& owner, const bool timed,
               const Duration interval) noexcept {
#if defined(STXLIB_LOCK_CHECKS)
    check_held(owner);
    const u64 caller = owner_of(owner);
    set_owner(owner, 0);
#else
    static_cast<void>(owner);
#endif
    bool host_timeout = false;
    if (timed) {
        host_timeout = thread_backend::condition_wait_for(condition, mutex_storage, interval);
    } else {
        thread_backend::condition_wait(condition, mutex_storage);
    }
#if defined(STXLIB_LOCK_CHECKS)
    set_owner(owner, caller);
#endif
    return host_timeout;
}

} // namespace

void ConditionVariable::wait(ScopedLock& lock, const WaitPredicate predicate, const void* const context) noexcept {
    require_predicate(predicate);
    Mutex& mutex = lock.mutex_;
    while (!predicate(context)) {
        static_cast<void>(host_wait(storage_, mutex.storage_, mutex.owner_, false, Duration()));
    }
}

// The host wait takes a relative interval, measured here against
// IntervalTime, so the deadline means the same on every host whatever clock
// the host's wait uses; timed_out is decided by IntervalTime alone, after
// the predicate was found false.
WaitResult ConditionVariable::wait_until(ScopedLock& lock, const WaitPredicate predicate, const void* const context,
                                         const IntervalTime deadline) noexcept {
    require_predicate(predicate);
    Mutex& mutex = lock.mutex_;
    for (;;) {
        if (predicate(context)) return WaitResult::satisfied;
        const IntervalTime now = IntervalTime::now();
        if (!(now < deadline)) return WaitResult::timed_out;
        static_cast<void>(host_wait(storage_, mutex.storage_, mutex.owner_, true, deadline.since(now)));
    }
}

WaitResult ConditionVariable::wait_for(ScopedLock& lock, const WaitPredicate predicate, const void* const context,
                                       const Duration interval) noexcept {
    // IntervalTime::after saturates (clocks.hpp), as stx_auxlib's
    // deadline_after did.
    const WaitResult result = wait_until(lock, predicate, context, IntervalTime::now().after(interval));
    return result;
}

void ConditionVariable::notify_one() noexcept { thread_backend::condition_notify_one(storage_); }

void ConditionVariable::notify_all() noexcept { thread_backend::condition_notify_all(storage_); }

// stxlib/detail/cancellation.hpp: the boundary's entry to the host's switch.
namespace detail {

int cancellation_off() noexcept {
    const int result = host::cancellation_off();
    return result;
}

void restore_cancellation(const int previous) noexcept { host::restore_cancellation(previous); }

} // namespace detail

// The first caller to take the mutex runs `entry`; later callers wait on the
// mutex and then see done_. (call_once's inline acquire load lets finished
// callers skip the mutex.)
void call_once_slow(OnceFlag& flag, void (*const entry)(void* context) noexcept, void* const context) noexcept {
    const ScopedLock lock(flag.mutex_);
    if (__atomic_load_n(&flag.done_, __ATOMIC_RELAXED) != 0) return;
    entry(context);
    __atomic_store_n(&flag.done_, 1U, __ATOMIC_RELEASE);
}

// ---- Thread -------------------------------------------------------------------

// Owned by the Thread; the host thread borrows it until joined. `joined` and
// `host` are the owner's alone.
struct Thread::State final {
    Entry entry{nullptr};
    void* context{nullptr};
    StopSignal stop{};
    ThreadIdentity identity{};
    u64 host{0};
    bool joined{false};
};

// The host thread's body (thread_backend.hpp): adopt the identity the owner
// issued, then run the entry.
void thread_backend::run_started_thread(void* const address) noexcept {
    Thread::State& started = *static_cast<Thread::State*>(address);
    state::current_flags().thread_identity = started.identity.value();
    started.entry(started.stop, started.context);
}

namespace {

void release_state(Thread::State* const state) noexcept {
    (*state).~State();
    memory::release(state);
}

// Requests a stop and joins; a thread disposing of its own Thread cannot join
// itself, which is a defect.
void finish_thread(Thread& thread) noexcept {
    thread.request_stop();
    const JoinResult result = thread.join();
    if (result == JoinResult::self_join) {
        const ErrorRecord record =
            ErrorRecord::make(ErrorKind::internal_failure, "stxlib.threads.join",
                              ErrorDetail{.message = "a thread destroyed its own Thread, which cannot join itself"});
        fail_fast(record);
    }
}

} // namespace

Thread::~Thread() {
    if (state_ == nullptr) return;
    finish_thread(*this);
    // Let go before releasing (AGENTS.md, rule 1).
    State* const held = state_;
    state_ = nullptr;
    release_state(held);
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this == &other) return *this;
    if (state_ != nullptr) finish_thread(*this);
    State* const held = state_;
    state_ = other.state_;
    other.state_ = nullptr;
    if (held != nullptr) release_state(held);
    return *this;
}

Result<Thread> Thread::start(const Entry entry, void* const context, const usize stack_bytes) noexcept {
    const Operation operation = "stxlib.threads.start";
    if (entry == nullptr) {
        const ErrorRecord refused = ErrorRecord::make(
            ErrorKind::invalid_argument, operation,
            ErrorDetail{.code = static_cast<u32>(ThreadFailure::no_entry), .message = "a thread needs an entry"});
        return Result<Thread>::failure(refused);
    }
    if (stack_bytes < minimum_thread_stack_bytes || stack_bytes > maximum_thread_stack_bytes) {
        const ErrorRecord refused =
            ErrorRecord::make(ErrorKind::invalid_argument, operation,
                              ErrorDetail{.code = static_cast<u32>(ThreadFailure::stack_size_out_of_range),
                                          .message = "a thread's stack size is outside STXlib's bounds"});
        return Result<Thread>::failure(refused);
    }
    const u64 identity = issue_identity();
    if (identity == 0) {
        const ErrorRecord exhausted = ErrorRecord::make(
            ErrorKind::limit_exceeded, operation, ErrorDetail{.message = "every thread identity has been issued"});
        return Result<Thread>::failure(exhausted);
    }
    void* const block = memory::allocate(sizeof(State), alignof(State));
    if (block == nullptr) return Result<Thread>::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
    State* const created = ::new (block) State();
    (*created).entry = entry;
    (*created).context = context;
    (*created).identity = ThreadIdentity::from_value(identity);
    ForeignCode host_failure{};
    if (!thread_backend::thread_start(created, stack_bytes, (*created).host, host_failure)) {
        release_state(created);
        const ErrorRecord refused =
            ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                              ErrorDetail{.foreign = host_failure, .message = "the host refused another thread"});
        return Result<Thread>::failure(refused);
    }
    Result<Thread> result = Result<Thread>::success(Thread(created));
    return result;
}

ThreadIdentity Thread::identity() const noexcept {
    if (state_ == nullptr || (*state_).joined) return ThreadIdentity();
    const ThreadIdentity result = (*state_).identity;
    return result;
}

void Thread::request_stop() noexcept {
    if (state_ == nullptr) return;
    (*state_).stop.request();
}

JoinResult Thread::join() noexcept {
    if (state_ == nullptr || (*state_).joined) return JoinResult::not_joinable;
    State& started = *state_;
    if (current_thread() == started.identity) return JoinResult::self_join;
    thread_backend::thread_join(started.host);
    started.joined = true;
    return JoinResult::joined;
}

} // namespace stxlib
