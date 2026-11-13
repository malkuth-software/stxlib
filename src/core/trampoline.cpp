// Trampolines with revocation (include/stxlib/trampoline.hpp).
//
// Each slot has three atomics:
//
//   state      generation << 2 | phase      (phase: free, reserved, attached, revoked)
//   in_flight  invocations that have announced themselves and not yet left
//   target     the attached target, written while reserved
//
// Life of a slot generation g (g >= 1):
//   free(g-1) --attach--> reserved(g) --publish--> attached(g)
//   attached(g) --revoke--> revoked(g) --last leave or revoker--> free(g)
// A slot whose generation reached generation_limit stays free and is never
// reserved again (retired), so a handle is never reissued.
//
// Why revoke cannot miss a running invocation (all four operations below are
// sequentially consistent, so they fall in one total order S):
//   enter:  in_flight += 1;  read state;  run only if state == attached(g)
//   revoke: state attached(g) -> revoked(g);  read in_flight;  wait while > 0
// If revoke's read of in_flight comes before enter's increment in S, then
// enter's read of state comes after revoke's change in S and sees revoked(g)
// (or later), so enter does not run the target. Otherwise revoke sees the
// increment and waits for the matching decrement, which comes after the
// target has returned. This is the store-buffering pattern that sequential
// consistency forbids ([atomics.order]); it is argued here, not yet proved
// by a model checker (proofs/REPORT.md).
//
// Freeing revoked(g): whoever observes in_flight == 0 with the state still
// revoked(g) moves it to free(g) with a compare-exchange, so exactly one
// party frees it: the waiting revoker, or the leave (or refused enter) that
// brought in_flight to zero. A deferred revocation (from inside its own
// invocation) is always freed by a leave, since that invocation holds
// in_flight above zero until it leaves.
//
// The calling thread's open invocations are kept on a bounded stack
// (trampoline_nesting_limit) in stxlib_state, which tells a self-revocation
// from another thread's and bounds nested dispatch.

#include "stxlib/trampoline.hpp"

#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"

#include "hosted/host.hpp"
#include "state/state.hpp"

#include <atomic>
#include <new>

namespace stxlib {

namespace detail {

namespace {

enum class Phase : u64 {
    free = 0,
    reserved = 1,
    attached = 2,
    revoked = 3,
};

constexpr u64 phase_mask = 3;
constexpr unsigned generation_shift = 2;
constexpr unsigned handle_index_bits = 16;
constexpr u64 handle_index_mask = (static_cast<u64>(1) << handle_index_bits) - 1;
// Handles carry 48 bits of generation.
constexpr u64 generation_limit = (static_cast<u64>(1) << (64 - handle_index_bits)) - 1;

static_assert(trampoline_capacity_limit <= handle_index_mask, "a slot index fits the handle's index bits");

[[nodiscard]] constexpr u64 make_state(const u64 generation, const Phase phase) noexcept {
    const u64 result = (generation << generation_shift) | static_cast<u64>(phase);
    return result;
}

[[nodiscard]] constexpr Phase phase_of(const u64 state) noexcept { return static_cast<Phase>(state & phase_mask); }

[[nodiscard]] constexpr u64 generation_of(const u64 state) noexcept { return state >> generation_shift; }

[[nodiscard]] constexpr TrampolineHandle make_handle(const usize index, const u64 generation) noexcept {
    const u64 value = (generation << handle_index_bits) | static_cast<u64>(index);
    const TrampolineHandle result = TrampolineHandle::from_value(value);
    return result;
}

} // namespace

struct TrampolineSlot final {
    std::atomic<u64> state{0};
    std::atomic<u32> in_flight{0};
    std::atomic<void*> target{nullptr};
};

class TrampolineSlots final {
public:
    usize capacity{0};
    TrampolineSlot* slots{nullptr};
};

namespace {

// The calling thread's open invocations are in stxlib_state (state.hpp), so
// a table entered through one module's copy of this file and revoked
// through another's still sees a self-revocation.
[[nodiscard]] bool inside(const TrampolineSlots* const table, const u64 handle) noexcept {
    const state::ThreadState& thread = state::current_thread();
    for (usize index = 0; index < thread.open_depth; ++index) {
        const state::OpenInvocation& entry = thread.open[index];
        if (entry.table == table && entry.handle == handle) return true;
    }
    return false;
}

[[nodiscard]] bool inside_table(const TrampolineSlots* const table) noexcept {
    const state::ThreadState& thread = state::current_thread();
    for (usize index = 0; index < thread.open_depth; ++index) {
        if (thread.open[index].table == table) return true;
    }
    return false;
}

// Moves revoked(generation) to free(generation) if no invocation is in
// flight. Returns true when the slot is no longer revoked(generation),
// whoever freed it.
bool try_free(TrampolineSlot& slot, const u64 generation) noexcept {
    u64 expected = make_state(generation, Phase::revoked);
    if (slot.state.load(std::memory_order_seq_cst) != expected) return true;
    if (slot.in_flight.load(std::memory_order_seq_cst) != 0) return false;
    // The target pointer is left as it is: it is read only in the attached
    // phase, and clearing it here could race with a new attachment's store.
    const bool freed = slot.state.compare_exchange_strong(expected, make_state(generation, Phase::free),
                                                          std::memory_order_seq_cst);
    // Either this call freed it, or the state has already moved on.
    static_cast<void>(freed);
    return true;
}

// Ends one announced invocation of `generation` on `slot`.
void depart(TrampolineSlot& slot, const u64 generation) noexcept {
    const u32 before = slot.in_flight.fetch_sub(1, std::memory_order_seq_cst);
    if (before == 1) static_cast<void>(try_free(slot, generation));
}

// Waits until revoked(generation) has no invocation in flight and is freed.
void wait_until_freed(TrampolineSlot& slot, const u64 generation) noexcept {
    for (;;) {
        const bool done = try_free(slot, generation);
        if (done) return;
        host::yield_thread();
    }
}

[[nodiscard]] ErrorRecord stale(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(
        ErrorKind::stale_identity, operation,
        ErrorDetail{.message = "the trampoline handle is not attached in this table"});
    return result;
}

} // namespace

} // namespace detail

Result<TrampolineTable> TrampolineTable::create(const usize capacity) noexcept {
    if (capacity == 0 || capacity > trampoline_capacity_limit) {
        const ErrorRecord invalid = ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.trampoline.create",
            ErrorDetail{.message = "the capacity must be 1 to trampoline_capacity_limit"});
        return Result<TrampolineTable>::failure(invalid);
    }
    void* const table_block = memory::allocate(sizeof(detail::TrampolineSlots), alignof(detail::TrampolineSlots));
    void* const slot_block = memory::allocate(capacity * sizeof(detail::TrampolineSlot), alignof(detail::TrampolineSlot));
    if (table_block == nullptr || slot_block == nullptr) {
        memory::release(table_block);
        memory::release(slot_block);
        const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.trampoline.create");
        return Result<TrampolineTable>::failure(exhausted);
    }
    detail::TrampolineSlot* const slots = static_cast<detail::TrampolineSlot*>(slot_block);
    for (usize index = 0; index < capacity; ++index) {
        new (static_cast<void*>(slots + index)) detail::TrampolineSlot();
    }
    detail::TrampolineSlots* const table = new (table_block) detail::TrampolineSlots();
    (*table).capacity = capacity;
    (*table).slots = slots;
    TrampolineTable result(table);
    return Result<TrampolineTable>::success(static_cast<TrampolineTable&&>(result));
}

TrampolineTable::~TrampolineTable() {
    if (slots_ == nullptr) return;
    detail::TrampolineSlots& table = *slots_;
    STXLIB_REQUIRE(!detail::inside_table(slots_), "TrampolineTable destroyed from inside its own invocation");
    for (usize index = 0; index < table.capacity; ++index) {
        detail::TrampolineSlot& slot = table.slots[index];
        u64 state = slot.state.load(std::memory_order_seq_cst);
        // A reservation is finished by attach on another thread; destroying
        // a table that is still being attached to is the owner's defect.
        STXLIB_REQUIRE(detail::phase_of(state) != detail::Phase::reserved,
                       "TrampolineTable destroyed while a target is being attached");
        if (detail::phase_of(state) == detail::Phase::attached) {
            const u64 generation = detail::generation_of(state);
            const bool revoked = slot.state.compare_exchange_strong(
                state, detail::make_state(generation, detail::Phase::revoked), std::memory_order_seq_cst);
            STXLIB_REQUIRE(revoked, "TrampolineTable destroyed while its handles are being revoked");
        }
        if (detail::phase_of(slot.state.load(std::memory_order_seq_cst)) == detail::Phase::revoked) {
            detail::wait_until_freed(slot, detail::generation_of(slot.state.load(std::memory_order_seq_cst)));
        }
    }
    for (usize index = 0; index < table.capacity; ++index) table.slots[index].~TrampolineSlot();
    memory::release(table.slots);
    table.~TrampolineSlots();
    memory::release(slots_);
    slots_ = nullptr;
}

usize TrampolineTable::capacity() const noexcept {
    if (slots_ == nullptr) return 0;
    return (*slots_).capacity;
}

Result<TrampolineHandle> TrampolineTable::attach(void* const target) noexcept {
    STXLIB_REQUIRE(target != nullptr, "TrampolineTable::attach: null target");
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::attach on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    for (usize index = 0; index < table.capacity; ++index) {
        detail::TrampolineSlot& slot = table.slots[index];
        u64 state = slot.state.load(std::memory_order_seq_cst);
        if (detail::phase_of(state) != detail::Phase::free) continue;
        const u64 generation = detail::generation_of(state);
        if (generation >= detail::generation_limit) continue; // retired
        const u64 next = generation + 1;
        const bool reserved = slot.state.compare_exchange_strong(
            state, detail::make_state(next, detail::Phase::reserved), std::memory_order_seq_cst);
        if (!reserved) continue;
        slot.target.store(target, std::memory_order_relaxed);
        slot.state.store(detail::make_state(next, detail::Phase::attached), std::memory_order_seq_cst);
        const TrampolineHandle handle = detail::make_handle(index, next);
        return Result<TrampolineHandle>::success(handle);
    }
    const ErrorRecord full = ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.trampoline.attach",
                                               ErrorDetail{.message = "every trampoline slot is in use"});
    return Result<TrampolineHandle>::failure(full);
}

Result<TrampolineEntry> TrampolineTable::enter(const TrampolineHandle handle) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::enter on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    const usize index = static_cast<usize>(handle.value() & detail::handle_index_mask);
    const u64 generation = handle.value() >> detail::handle_index_bits;
    if (generation == 0 || index >= table.capacity) {
        return Result<TrampolineEntry>::failure(detail::stale("stxlib.trampoline.enter"));
    }
    state::ThreadState& thread = state::current_thread();
    if (thread.open_depth == trampoline_nesting_limit) {
        const ErrorRecord too_deep = ErrorRecord::make(
            ErrorKind::limit_exceeded, "stxlib.trampoline.enter",
            ErrorDetail{.message = "trampoline invocations are nested deeper than trampoline_nesting_limit"});
        return Result<TrampolineEntry>::failure(too_deep);
    }
    detail::TrampolineSlot& slot = table.slots[index];
    slot.in_flight.fetch_add(1, std::memory_order_seq_cst);
    const u64 state = slot.state.load(std::memory_order_seq_cst);
    if (state != detail::make_state(generation, detail::Phase::attached)) {
        detail::depart(slot, generation);
        if (state == detail::make_state(generation, detail::Phase::revoked)) {
            const ErrorRecord revoked = ErrorRecord::make(ErrorKind::revoked, "stxlib.trampoline.enter",
                                                          ErrorDetail{.message = "the trampoline handle is revoked"});
            return Result<TrampolineEntry>::failure(revoked);
        }
        return Result<TrampolineEntry>::failure(detail::stale("stxlib.trampoline.enter"));
    }
    void* const target = slot.target.load(std::memory_order_relaxed);
    thread.open[thread.open_depth] = state::OpenInvocation{slots_, handle.value()};
    thread.open_depth = thread.open_depth + 1;
    const TrampolineEntry entry{target, handle};
    return Result<TrampolineEntry>::success(entry);
}

void TrampolineTable::leave(const TrampolineEntry& entry) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::leave on a moved-from table");
    state::ThreadState& thread = state::current_thread();
    STXLIB_REQUIRE(thread.open_depth > 0, "TrampolineTable::leave without an open invocation");
    const state::OpenInvocation& top = thread.open[thread.open_depth - 1];
    STXLIB_REQUIRE(top.table == slots_ && top.handle == entry.handle.value(),
                   "TrampolineTable::leave out of order");
    thread.open_depth = thread.open_depth - 1;
    detail::TrampolineSlots& table = *slots_;
    const usize index = static_cast<usize>(entry.handle.value() & detail::handle_index_mask);
    const u64 generation = entry.handle.value() >> detail::handle_index_bits;
    detail::depart(table.slots[index], generation);
}

Result<Revocation> TrampolineTable::revoke(const TrampolineHandle handle) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::revoke on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    const usize index = static_cast<usize>(handle.value() & detail::handle_index_mask);
    const u64 generation = handle.value() >> detail::handle_index_bits;
    if (generation == 0 || index >= table.capacity) {
        return Result<Revocation>::failure(detail::stale("stxlib.trampoline.revoke"));
    }
    detail::TrampolineSlot& slot = table.slots[index];
    u64 expected = detail::make_state(generation, detail::Phase::attached);
    const bool revoked = slot.state.compare_exchange_strong(
        expected, detail::make_state(generation, detail::Phase::revoked), std::memory_order_seq_cst);
    if (!revoked) return Result<Revocation>::failure(detail::stale("stxlib.trampoline.revoke"));
    if (detail::inside(slots_, handle.value())) return Result<Revocation>::success(Revocation::deferred);
    detail::wait_until_freed(slot, generation);
    return Result<Revocation>::success(Revocation::completed);
}

} // namespace stxlib
