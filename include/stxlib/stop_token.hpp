#pragma once

// Cooperative stop requests shared by copies (Capability::threads; the
// shield's item 3, owner-approved 2026-11-22): StopSource, StopToken and
// StopCallback, the replacements for C++20's std::stop_source,
// std::stop_token and std::stop_callback ([thread.stoptoken]). StopSignal
// (threads.hpp) stays the one-thread form a Thread hands its entry; these
// are for a request that crosses owners: a source the requester keeps, any
// number of tokens handed to the work, and callbacks that turn the request
// into a wake-up of something the work is blocked on.
//
// One shared stop state per StopSource::create, on STXlib's allocator, so a
// state made in one of STXlib's modules may be used and released in
// another (memory.hpp). Every StopSource, StopToken and registered
// StopCallback holds a reference; the last to let go releases it.
// Reference counts cannot overflow: each holder is a distinct object, and
// 2^64 of them cannot exist.
//
// Requests: a stop is requested at most once per state, by the first
// request_stop of any source sharing it, and is permanent. stop_requested()
// on any copy acquires the writes made before the request.
//
// Callbacks (StopCallback): a function and a context registered on a
// token's state.
//   - Registered before the request: run once, by the thread whose
//     request_stop made the request, before that request_stop returns, in
//     the order they were registered.
//   - Registered after the request: run at once, by the registering thread,
//     before the constructor returns; nothing is registered.
//   - Registered on a token with no state, or on a state that can no longer
//     be stopped (stop_possible() false): never run; nothing is registered.
//   - Destroyed before its turn: unregistered, never run. Destroyed while
//     it runs on another thread: the destructor waits until it has
//     returned, so the context outlives the call. Destroyed by its own
//     function (on the requesting thread): returns at once, and the request
//     touches the callback no more.
// A callback runs with no STXlib lock held. It must return (AGENTS.md,
// early exits): no longjmp out of it, no thread exit inside it; an early
// exit out of one leaves the later callbacks unrun and a destructor waiting
// for it waiting for ever.
//
// Thread safety: as std's. Distinct copies sharing one state may be used,
// copied and destroyed on any threads at once; one StopSource or StopToken
// object is not itself synchronised. A StopCallback is constructed and
// destroyed by its owner, on any thread.
//
// Failure: StopSource::create fails with out_of_memory (D8 = a); nothing
// else fails. A host error in the state's lock is a defect (threads.hpp).

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

namespace detail {
// The shared stop state, defined in src/core/stop_token.cpp.
struct StopState;
} // namespace detail

class StopSource;
class StopCallback;

// Observes a stop state. The default token has none: it is never stopped
// and stop_possible() is false. Copies share the state.
// ic: stxlib::StopToken: generated from include/stxlib/stop_token.ic.toml by tools/ic_declarations.py; edit the
// ic: declaration, not these lines. States: none (initial), observing, stopped. Observed by stop_requested(): none
// ic: false, observing false, stopped true. Thread: owner, unless a call says otherwise.
class StopToken final {
public:
    StopToken() noexcept = default;
    ~StopToken();
    StopToken(const StopToken& other) noexcept;
    StopToken& operator=(const StopToken& other) noexcept;
    StopToken(StopToken&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
    StopToken& operator=(StopToken&& other) noexcept;

    // True once a stop has been requested on the state.
    // ic: Returns bool.
    [[nodiscard]] bool stop_requested() const noexcept;
    // True when a stop has been requested, or a StopSource that could still
    // request one exists.
    // ic: Returns bool.
    [[nodiscard]] bool stop_possible() const noexcept;
    // True when both tokens observe the same state, or both have none.
    // ic: Returns bool.
    [[nodiscard]] bool same_state(const StopToken& other) const noexcept { return state_ == other.state_; }

private:
    friend class StopSource;
    friend class StopCallback;
    // Adopts one reference the caller already took.
    explicit StopToken(detail::StopState* const state) noexcept : state_(state) {}
    detail::StopState* state_{nullptr};
};

// Requests a stop on its state. The default source has none (it cannot
// request a stop, and its tokens have no state either); create makes one.
// Copies share the state.
// ic: stxlib::StopSource: generated from include/stxlib/stop_source.ic.toml by tools/ic_declarations.py; edit the
// ic: declaration, not these lines. States: empty (initial), armed, stopped. Observed by stop_possible(): empty false,
// ic: armed true, stopped true. Thread: owner, unless a call says otherwise.
class StopSource final {
public:
    // A new state, no stop requested. Failure: out_of_memory.
    // ic: Fails out_of_memory (the allocator refuses the shared state), unchanged. Thread: any. Returns StopSource in
    // ic: armed, owned by the caller.
    [[nodiscard]] static OperationResult<StopSource> create() noexcept;

    StopSource() noexcept = default;
    ~StopSource();
    StopSource(const StopSource& other) noexcept;
    StopSource& operator=(const StopSource& other) noexcept;
    StopSource(StopSource&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }
    StopSource& operator=(StopSource&& other) noexcept;

    // A token on this source's state; a token with no state when it has
    // none.
    // ic: Returns StopToken, owned by the caller.
    [[nodiscard]] StopToken token() const noexcept;
    // Requests the stop and runs the registered callbacks (above) on this
    // thread. True when this call made the request; false when one had been
    // made already (the callbacks may still be running on its thread) or
    // the source has no state.
    // ic: Admitted in armed, stopped; then stopped. In empty: returns false, unchanged. Returns bool.
    bool request_stop() noexcept;
    // ic: Returns bool.
    [[nodiscard]] bool stop_requested() const noexcept;
    // True when the source has a state.
    // ic: Returns bool.
    [[nodiscard]] bool stop_possible() const noexcept { return state_ != nullptr; }

private:
    explicit StopSource(detail::StopState* const state) noexcept : state_(state) {}
    detail::StopState* state_{nullptr};
};

// A callback's function: receives the context it was registered with.
using StopFunction = void (*)(void* context) noexcept;

// Registers `function` with `context` on a token's state for its own
// lifetime (above). Neither copied nor moved: the state links to it by
// address. The context is borrowed until the destructor returns: declare
// the callback after what its context points to.
// ic: stxlib::StopCallback: generated from include/stxlib/stop_callback.ic.toml by tools/ic_declarations.py; edit the
// ic: declaration, not these lines. States: listed, finished, inert (initial). Thread: owner, unless a call says
// ic: otherwise.
class StopCallback final {
public:
    // A null function is a failed requirement.
    // ic: Requires not_null(function), else requirement ("StopCallback: a callback needs a function"). Returns
    // ic: StopCallback in listed, finished or inert.
    StopCallback(const StopToken& token, StopFunction function, void* context) noexcept;
    ~StopCallback();
    StopCallback(const StopCallback&) = delete;
    StopCallback& operator=(const StopCallback&) = delete;
    StopCallback(StopCallback&&) = delete;
    StopCallback& operator=(StopCallback&&) = delete;

private:
    // The state keeps its callbacks in a list through these fields.
    friend struct detail::StopState;
    // The state while registered or running (one reference held), else
    // null: no state, ran at construction, or never possible.
    detail::StopState* state_{nullptr};
    StopFunction function_{nullptr};
    void* context_{nullptr};
    // The state's list, guarded by the state's lock.
    StopCallback* previous_{nullptr};
    StopCallback* next_{nullptr};
    // True while in the state's list; guarded by the state's lock.
    bool listed_{false};
};

} // namespace stxlib
