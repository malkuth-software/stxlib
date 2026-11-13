// Threads and synchronization (include/stxlib/threads.hpp) over the
// family's thread module (hosted/thread_backend.hpp). Ported from
// stx_auxlib's src/hosted/threads.cpp at b6e330a: the same identities,
// lifetime rules and once algorithm, with the host calls moved to the
// family modules and the identity state moved to stxlib_state.

#include "stxlib/threads.hpp"

#include "hosted/host.hpp"
#include "hosted/thread_backend.hpp"
#include "state/state.hpp"
#include "stxlib/fail_fast.hpp"
#include "stxlib/memory.hpp"

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
    state::ThreadState& thread = state::current_thread();
    if (thread.thread_identity == 0) {
        const u64 issued = issue_identity();
        if (issued == 0) identities_exhausted();
        thread.thread_identity = issued;
    }
    const ThreadIdentity result = ThreadIdentity::from_value(thread.thread_identity);
    return result;
}

// Both sleeps end only once MonotonicTime has reached the deadline: a host
// sleep may end early (a Windows sleep is capped below 49.7 days).
void sleep_for(const Duration interval) noexcept {
    if (interval <= Duration::nanoseconds(0)) return;
    sleep_until(MonotonicTime::now().after(interval));
}

void sleep_until(const MonotonicTime deadline) noexcept {
    for (;;) {
        const MonotonicTime now = MonotonicTime::now();
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

// ---- Mutex and ConditionVariable ---------------------------------------------

Mutex::Mutex() noexcept { thread_backend::mutex_create(storage_); }

Mutex::~Mutex() { thread_backend::mutex_destroy(storage_); }

void Mutex::lock() noexcept { thread_backend::mutex_lock(storage_); }

bool Mutex::try_lock() noexcept {
    const bool result = thread_backend::mutex_try_lock(storage_);
    return result;
}

void Mutex::unlock() noexcept { thread_backend::mutex_unlock(storage_); }

ConditionVariable::ConditionVariable() noexcept { thread_backend::condition_create(storage_); }

ConditionVariable::~ConditionVariable() { thread_backend::condition_destroy(storage_); }

void ConditionVariable::wait(ScopedLock& lock) noexcept {
    thread_backend::condition_wait(storage_, lock.mutex_.storage_);
}

// The host wait takes a relative interval, measured here against
// MonotonicTime, so the deadline means the same on every host whatever clock
// the host's wait uses. A host timeout before the deadline (another clock
// ran faster) is reported as woken, which the caller's loop rechecks.
WaitResult ConditionVariable::wait_until(ScopedLock& lock, const MonotonicTime deadline) noexcept {
    const MonotonicTime now = MonotonicTime::now();
    if (!(now < deadline)) return WaitResult::timed_out;
    const bool host_timeout = thread_backend::condition_wait_for(storage_, lock.mutex_.storage_, deadline.since(now));
    if (host_timeout && !(MonotonicTime::now() < deadline)) return WaitResult::timed_out;
    return WaitResult::woken;
}

WaitResult ConditionVariable::wait_for(ScopedLock& lock, const Duration interval) noexcept {
    // MonotonicTime::after saturates (clocks.hpp), as stx_auxlib's
    // deadline_after did.
    const WaitResult result = wait_until(lock, MonotonicTime::now().after(interval));
    return result;
}

void ConditionVariable::notify_one() noexcept { thread_backend::condition_notify_one(storage_); }

void ConditionVariable::notify_all() noexcept { thread_backend::condition_notify_all(storage_); }

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
    state::current_thread().thread_identity = started.identity.value();
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
    release_state(state_);
    state_ = nullptr;
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this == &other) return *this;
    if (state_ != nullptr) {
        finish_thread(*this);
        release_state(state_);
    }
    state_ = other.state_;
    other.state_ = nullptr;
    return *this;
}

Result<Thread> Thread::start(const Entry entry, void* const context) noexcept {
    const Operation operation = "stxlib.threads.start";
    if (entry == nullptr) {
        const ErrorRecord refused = ErrorRecord::make(ErrorKind::invalid_argument, operation,
                                                      ErrorDetail{.message = "a thread needs an entry"});
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
    if (!thread_backend::thread_start(created, (*created).host, host_failure)) {
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
