// StopSource, StopToken and StopCallback (include/stxlib/stop_token.hpp).
//
// The shared state holds two counts, a flag and a callback list:
//   - references: every StopSource, StopToken and registered StopCallback,
//     plus one for each request_stop in progress (a callback may destroy
//     the source whose request runs it). The last release destroys the
//     state. An increment is relaxed (the incrementer already holds a
//     reference); a decrement is acquire-release, so the releasing thread
//     sees every other holder's writes, as Shared's counts do (shared.hpp).
//   - sources: the StopSources, for stop_possible.
//   - requested: 0, then 1 once; stored with release order under the lock,
//     loaded with acquire order anywhere.
//   - the callbacks not yet run, first registered first, and the one
//     running (if any) with the thread that runs it; all guarded by the
//     state's Mutex, which is never held across a callback.
//
// The request (run_request) takes the first callback out of the list, marks
// it running, releases the lock, calls it, retakes the lock, clears the
// mark and wakes any destructor waiting for it; then the next. After the
// call it touches only the state, never the callback, so a callback that
// destroys itself inside its own function leaves nothing dangling. A
// destructor on another thread that finds its callback running waits on the
// state's condition until the mark moves on; the requesting thread's own
// destructor (inside the callback) cannot wait for itself and returns.

#include "stxlib/stop_token.hpp"

#include "stxlib/detail/cancellation.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"
#include "stxlib/threads.hpp"

#include "core/stop_token_ic_checks.hpp"

#include <atomic>
#include <new>

namespace stxlib {

namespace detail {

struct StopState final {
    std::atomic<u64> references{1};
    std::atomic<u64> sources{1};
    std::atomic<u32> requested{0};
    Mutex lock{};
    // Notified each time `running` is cleared.
    ConditionVariable finished{};
    // Guarded by `lock`.
    StopCallback* first{nullptr};
    StopCallback* last{nullptr};
    StopCallback* running{nullptr};
    // The ThreadIdentity value of the thread that made the request; 0
    // before it.
    u64 requester{0};

    void take_reference() noexcept { references.fetch_add(1, std::memory_order_relaxed); }

    // Lets go of one reference; the last destroys the state. The caller has
    // already cleared its own pointer to it (AGENTS.md, rule 1).
    static void release_reference(StopState* const state) noexcept {
        const u64 before = (*state).references.fetch_sub(1, std::memory_order_acq_rel);
        if (before != 1) return;
        (*state).~StopState();
        memory::release(state);
    }

    static void take_source(StopState* const state) noexcept {
        (*state).sources.fetch_add(1, std::memory_order_relaxed);
        (*state).take_reference();
    }

    static void release_source(StopState* const state) noexcept {
        (*state).sources.fetch_sub(1, std::memory_order_acq_rel);
        release_reference(state);
    }

    [[nodiscard]] bool stop_requested() const noexcept {
        const bool result = requested.load(std::memory_order_acquire) != 0;
        return result;
    }

    // Under `lock`: append at the end of the list.
    void append(StopCallback& callback) noexcept {
        callback.previous_ = last;
        callback.next_ = nullptr;
        if (last == nullptr) {
            first = &callback;
        } else {
            (*last).next_ = &callback;
        }
        last = &callback;
        callback.listed_ = true;
    }

    // Under `lock`: take a listed callback out of the list.
    void unlink(StopCallback& callback) noexcept {
        if (callback.previous_ == nullptr) {
            first = callback.next_;
        } else {
            (*callback.previous_).next_ = callback.next_;
        }
        if (callback.next_ == nullptr) {
            last = callback.previous_;
        } else {
            (*callback.next_).previous_ = callback.previous_;
        }
        callback.previous_ = nullptr;
        callback.next_ = nullptr;
        callback.listed_ = false;
    }

    // The request: false when one was made already. Runs every listed
    // callback, first registered first, without the lock. Static, like the
    // reference counting above: a callback may destroy any object but the
    // state, which the caller keeps alive with a reference of its own.
    [[nodiscard]] static bool run_request(StopState* const state) noexcept {
        UniqueLock held((*state).lock);
        if ((*state).requested.load(std::memory_order_relaxed) != 0) return false;
        (*state).requester = current_thread().value();
        (*state).requested.store(1, std::memory_order_release);
        for (;;) {
            StopCallback* const callback = (*state).first;
            if (callback == nullptr) break;
            (*state).unlink(*callback);
            (*state).running = callback;
            const StopFunction function = (*callback).function_;
            void* const context = (*callback).context_;
            held.unlock();
            STXLIB_EARLY_EXIT_WHOLE("the callback is out of the list and marked running, and the state's lock is "
                                    "released; nothing reads the callback after the call");
            function(context);
            held.lock();
            (*state).running = nullptr;
            (*state).finished.notify_all();
        }
        return true;
    }
};

// One reference to a state for as long as it lives: request_stop's own,
// since a callback may destroy the source whose request runs it.
class RequestReference final {
public:
    explicit RequestReference(StopState* const state) noexcept : state_(state) { (*state).take_reference(); }
    ~RequestReference() { StopState::release_reference(state_); }
    RequestReference(const RequestReference&) = delete;
    RequestReference& operator=(const RequestReference&) = delete;
    RequestReference(RequestReference&&) = delete;
    RequestReference& operator=(RequestReference&&) = delete;

private:
    StopState* state_;
};

} // namespace detail

namespace {

// The context of a destructor's wait: its state and its callback.
struct RunningCallback final {
    const detail::StopState* state;
    const StopCallback* callback;
};

// True once the request has finished running the callback (called under
// the state's lock).
bool callback_finished(const void* const address) noexcept {
    const RunningCallback& waiting = *static_cast<const RunningCallback*>(address);
    const bool result = (*waiting.state).running != waiting.callback;
    return result;
}

} // namespace

// ---- StopToken ------------------------------------------------------------------

StopToken::~StopToken() {
    if (state_ == nullptr) return;
    detail::StopState* const held = state_;
    state_ = nullptr;
    detail::StopState::release_reference(held);
}

StopToken::StopToken(const StopToken& other) noexcept : state_(other.state_) {
    if (state_ != nullptr) (*state_).take_reference();
}

StopToken& StopToken::operator=(const StopToken& other) noexcept {
    if (this == &other) return *this;
    if (other.state_ != nullptr) (*other.state_).take_reference();
    detail::StopState* const held = state_;
    state_ = other.state_;
    if (held != nullptr) detail::StopState::release_reference(held);
    return *this;
}

StopToken& StopToken::operator=(StopToken&& other) noexcept {
    if (this == &other) return *this;
    detail::StopState* const held = state_;
    state_ = other.state_;
    other.state_ = nullptr;
    if (held != nullptr) detail::StopState::release_reference(held);
    return *this;
}

bool StopToken::stop_requested() const noexcept {
    if (state_ == nullptr) return false;
    const bool result = (*state_).stop_requested();
    return result;
}

bool StopToken::stop_possible() const noexcept {
    if (state_ == nullptr) return false;
    if ((*state_).stop_requested()) return true;
    const bool result = (*state_).sources.load(std::memory_order_acquire) != 0;
    return result;
}

// ---- StopSource -----------------------------------------------------------------

OperationResult<StopSource> StopSource::create() noexcept {
    void* const block = memory::allocate(sizeof(detail::StopState), alignof(detail::StopState));
    if (block == nullptr) {
        const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.stop.create");
        return OperationResult<StopSource>::failure(exhausted);
    }
    detail::StopState* const state = ::new (block) detail::StopState();
    OperationResult<StopSource> result = OperationResult<StopSource>::success(StopSource(state));
    return result;
}

StopSource::~StopSource() {
    if (state_ == nullptr) return;
    detail::StopState* const held = state_;
    state_ = nullptr;
    detail::StopState::release_source(held);
}

StopSource::StopSource(const StopSource& other) noexcept : state_(other.state_) {
    if (state_ != nullptr) detail::StopState::take_source(state_);
}

StopSource& StopSource::operator=(const StopSource& other) noexcept {
    if (this == &other) return *this;
    if (other.state_ != nullptr) detail::StopState::take_source(other.state_);
    detail::StopState* const held = state_;
    state_ = other.state_;
    if (held != nullptr) detail::StopState::release_source(held);
    return *this;
}

StopSource& StopSource::operator=(StopSource&& other) noexcept {
    if (this == &other) return *this;
    detail::StopState* const held = state_;
    state_ = other.state_;
    other.state_ = nullptr;
    if (held != nullptr) detail::StopState::release_source(held);
    return *this;
}

StopToken StopSource::token() const noexcept {
    if (state_ == nullptr) return StopToken();
    (*state_).take_reference();
    StopToken result(state_);
    return result;
}

bool StopSource::request_stop() noexcept {
    if (state_ == nullptr) return false;
    detail::StopState* const state = state_;
    // Held until the return: a callback may destroy this source.
    const detail::RequestReference request(state);
    const bool result = detail::StopState::run_request(state);
    return result;
}

bool StopSource::stop_requested() const noexcept {
    if (state_ == nullptr) return false;
    const bool result = (*state_).stop_requested();
    return result;
}

// ---- StopCallback ---------------------------------------------------------------

namespace {

enum class Registration : u8 {
    // No state, or no source left and no request: never run.
    none = 0,
    // In the state's list until the request or the destructor.
    listed = 1,
    // The stop was requested already: run here, now.
    run_now = 2,
};

} // namespace

StopCallback::StopCallback(const StopToken& token, const StopFunction function, void* const context) noexcept
    : function_(function), context_(context) {
    STXLIB_REQUIRE(detail::ic::stop_callback::admits_callback_function(function),
                   detail::ic::stop_callback::callback_function_message);
    detail::StopState* const state = token.state_;
    if (state == nullptr) return;
    Registration registration = Registration::none;
    {
        const ScopedLock held((*state).lock);
        if ((*state).requested.load(std::memory_order_relaxed) != 0) {
            registration = Registration::run_now;
        } else if ((*state).sources.load(std::memory_order_acquire) != 0) {
            (*state).take_reference();
            state_ = state;
            (*state).append(*this);
            registration = Registration::listed;
        }
    }
    if (registration != Registration::run_now) return;
    STXLIB_EARLY_EXIT_WHOLE("nothing is registered and no STXlib lock is held");
    function(context);
}

StopCallback::~StopCallback() {
    if (state_ == nullptr) return;
    detail::StopState* const state = state_;
    {
        // The condition wait is a cancellation point reached with the
        // state's lock held (AGENTS.md, cancellation).
        const detail::CancellationHold no_cancel;
        ScopedLock held((*state).lock);
        if (listed_) {
            (*state).unlink(*this);
        } else if ((*state).running == this && (*state).requester != current_thread().value()) {
            const RunningCallback waiting{.state = state, .callback = this};
            (*state).finished.wait(held, callback_finished, &waiting);
        }
    }
    state_ = nullptr;
    detail::StopState::release_reference(state);
}

} // namespace stxlib
