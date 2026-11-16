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
// A slot whose generation reached its table's generation limit stays free
// and is never reserved again (retired), so a handle is never reissued.
//
// Handles: a table packs a slot index in as few bits as its capacity needs
// (1 to 16) and the generation in the rest of a context pointer, capped at
// 2^48 - 1 (security audit 2026-11-15, A-stxlib-include-01-F10). On 64-bit
// targets every table has the cap; on wasm32 a table of 16 slots has 2^28 -
// 1 generations per slot instead of the 65535 a fixed 16-bit index left,
// and only a table of more than 32768 slots is down to 65535
// (trampoline_generation_limit, the least any table has).
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
// in_flight above zero until it leaves; the last leaver frees the state's
// own generation, which may be later than the one a refused enter carried.
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
constexpr unsigned pointer_bits = 8 * sizeof(void*);
// The generation cap on every target (it keeps make_state's shift in u64).
constexpr u64 generation_cap = (static_cast<u64>(1) << 48) - 1;

static_assert(trampoline_capacity_limit <= (static_cast<u64>(1) << trampoline_index_bits) - 1,
              "a slot index fits the widest index");
static_assert(trampoline_generation_limit <= generation_cap, "the least limit is within the cap");
static_assert(((trampoline_generation_limit << trampoline_index_bits) |
               ((static_cast<u64>(1) << trampoline_index_bits) - 1)) <= static_cast<u64>(~static_cast<uptr>(0)),
              "the largest handle of the widest index fits a context pointer");
static_assert(trampoline_generation_limit >= 65535, "a slot accepts at least 65535 attachments");

// The index bits a table of `capacity` slots needs: the width of the
// largest index, at least 1 and at most trampoline_index_bits.
[[nodiscard]] constexpr unsigned index_bits_for(const usize capacity) noexcept {
    unsigned bits = 1;
    while (bits < trampoline_index_bits && (static_cast<u64>(1) << bits) < static_cast<u64>(capacity)) bits = bits + 1;
    return bits;
}

// The generations a slot of a table with `index_bits` accepts: the rest of
// a pointer's bits, capped.
[[nodiscard]] constexpr u64 generation_limit_for(const unsigned index_bits) noexcept {
    const unsigned bits = pointer_bits - index_bits;
    if (bits >= 48) return generation_cap;
    const u64 result = (static_cast<u64>(1) << bits) - 1;
    return result;
}

static_assert(index_bits_for(1) == 1 && index_bits_for(2) == 1 && index_bits_for(3) == 2 && index_bits_for(16) == 4 &&
              index_bits_for(17) == 5 && index_bits_for(trampoline_capacity_limit) == trampoline_index_bits);
static_assert(generation_limit_for(trampoline_index_bits) == trampoline_generation_limit,
              "the widest index leaves the least limit");

[[nodiscard]] constexpr u64 make_state(const u64 generation, const Phase phase) noexcept {
    const u64 result = (generation << generation_shift) | static_cast<u64>(phase);
    return result;
}

[[nodiscard]] constexpr Phase phase_of(const u64 state) noexcept { return static_cast<Phase>(state & phase_mask); }

[[nodiscard]] constexpr u64 generation_of(const u64 state) noexcept { return state >> generation_shift; }

[[nodiscard]] constexpr TrampolineHandle make_handle(const usize index, const u64 generation,
                                                     const unsigned index_bits) noexcept {
    const u64 value = (generation << index_bits) | static_cast<u64>(index);
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
    // This table's handle packing (index_bits_for, generation_limit_for).
    unsigned index_bits{trampoline_index_bits};
    u64 index_mask{0};
    u64 generation_limit{trampoline_generation_limit};

    [[nodiscard]] usize index_of(const TrampolineHandle handle) const noexcept {
        const usize result = static_cast<usize>(handle.value() & index_mask);
        return result;
    }
    [[nodiscard]] u64 generation_of_handle(const TrampolineHandle handle) const noexcept {
        const u64 result = handle.value() >> index_bits;
        return result;
    }
};

namespace {

// The calling thread's open invocations are in stxlib_state (state.hpp), so
// a table entered through one module's copy of this file and revoked
// through another's still sees a self-revocation.
// A thread with no block (state.hpp, degraded mode) has no open invocation:
// enter refuses there.
[[nodiscard]] bool inside(const TrampolineSlots* const table, const u64 handle) noexcept {
    const state::ThreadState* const kept = state::current_thread_state();
    if (kept == nullptr) return false;
    const state::ThreadState& thread = *kept;
    for (usize index = 0; index < thread.open_depth; ++index) {
        const state::OpenInvocation& entry = thread.open[index];
        if (entry.table == table && entry.handle == handle) return true;
    }
    return false;
}

[[nodiscard]] bool inside_table(const TrampolineSlots* const table) noexcept {
    const state::ThreadState* const kept = state::current_thread_state();
    if (kept == nullptr) return false;
    const state::ThreadState& thread = *kept;
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

// Ends one announced invocation on `slot`. The last to leave frees the
// slot if it is revoked, whatever generation the leaver carried: a refused
// enter with a stale handle announces itself too, and if it is the last to
// leave after a self-revocation of a later generation, freeing only its own
// generation would strand revoked(later) with nothing in flight
// (A-stxlib-core-01-F2). The state's own generation is the one freed.
void depart(TrampolineSlot& slot) noexcept {
    const u32 before = slot.in_flight.fetch_sub(1, std::memory_order_seq_cst);
    if (before != 1) return;
    const u64 state = slot.state.load(std::memory_order_seq_cst);
    if (phase_of(state) == Phase::revoked) static_cast<void>(try_free(slot, generation_of(state)));
}

// Waits until revoked(generation) has no invocation in flight and is freed,
// with spin::lock's waiting policy (spin_lock.hpp; audit A, C8): yield 64
// times, then sleep about a millisecond between attempts, so a long callback
// does not have the revoking thread burning a core.
void wait_until_freed(TrampolineSlot& slot, const u64 generation) noexcept {
    u32 attempts = 0;
    for (;;) {
        const bool done = try_free(slot, generation);
        if (done) return;
        if (attempts < 64) {
            attempts = attempts + 1;
            host::yield_thread();
        } else {
            host::sleep_briefly();
        }
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

OperationResult<TrampolineTable> TrampolineTable::create(const usize capacity) noexcept {
    if (capacity == 0 || capacity > trampoline_capacity_limit) {
        const ErrorRecord invalid = ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.trampoline.create",
            ErrorDetail{.message = "the capacity must be 1 to trampoline_capacity_limit"});
        return OperationResult<TrampolineTable>::failure(invalid);
    }
    void* const table_block = memory::allocate(sizeof(detail::TrampolineSlots), alignof(detail::TrampolineSlots));
    void* const slot_block = memory::allocate(capacity * sizeof(detail::TrampolineSlot), alignof(detail::TrampolineSlot));
    if (table_block == nullptr || slot_block == nullptr) {
        memory::release(table_block);
        memory::release(slot_block);
        const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.trampoline.create");
        return OperationResult<TrampolineTable>::failure(exhausted);
    }
    detail::TrampolineSlot* const slots = static_cast<detail::TrampolineSlot*>(slot_block);
    for (usize index = 0; index < capacity; ++index) {
        new (static_cast<void*>(slots + index)) detail::TrampolineSlot();
    }
    detail::TrampolineSlots* const table = new (table_block) detail::TrampolineSlots();
    (*table).capacity = capacity;
    (*table).slots = slots;
    (*table).index_bits = detail::index_bits_for(capacity);
    (*table).index_mask = (static_cast<u64>(1) << (*table).index_bits) - 1;
    (*table).generation_limit = detail::generation_limit_for((*table).index_bits);
    TrampolineTable result(table);
    return OperationResult<TrampolineTable>::success(static_cast<TrampolineTable&&>(result));
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
    // Let go before releasing (AGENTS.md, rule 1).
    detail::TrampolineSlots* const held = slots_;
    slots_ = nullptr;
    detail::TrampolineSlot* const slots = table.slots;
    const usize capacity = table.capacity;
    table.slots = nullptr;
    table.capacity = 0;
    for (usize index = 0; index < capacity; ++index) slots[index].~TrampolineSlot();
    memory::release(slots);
    table.~TrampolineSlots();
    memory::release(held);
}

usize TrampolineTable::capacity() const noexcept {
    if (slots_ == nullptr) return 0;
    return (*slots_).capacity;
}

u64 TrampolineTable::generation_limit() const noexcept {
    if (slots_ == nullptr) return 0;
    return (*slots_).generation_limit;
}

OperationResult<TrampolineHandle> TrampolineTable::attach(void* const target) noexcept {
    STXLIB_REQUIRE(target != nullptr, "TrampolineTable::attach: null target");
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::attach on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    for (usize index = 0; index < table.capacity; ++index) {
        detail::TrampolineSlot& slot = table.slots[index];
        u64 state = slot.state.load(std::memory_order_seq_cst);
        if (detail::phase_of(state) != detail::Phase::free) continue;
        const u64 generation = detail::generation_of(state);
        if (generation >= table.generation_limit) continue; // retired
        const u64 next = generation + 1;
        const bool reserved = slot.state.compare_exchange_strong(
            state, detail::make_state(next, detail::Phase::reserved), std::memory_order_seq_cst);
        if (!reserved) continue;
        slot.target.store(target, std::memory_order_relaxed);
        slot.state.store(detail::make_state(next, detail::Phase::attached), std::memory_order_seq_cst);
        const TrampolineHandle handle = detail::make_handle(index, next, table.index_bits);
        return OperationResult<TrampolineHandle>::success(handle);
    }
    const ErrorRecord full = ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.trampoline.attach",
                                               ErrorDetail{.message = "every trampoline slot is in use"});
    return OperationResult<TrampolineHandle>::failure(full);
}

OperationResult<TrampolineEntry> TrampolineTable::enter(const TrampolineHandle handle) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::enter on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    const usize index = table.index_of(handle);
    const u64 generation = table.generation_of_handle(handle);
    if (generation == 0 || generation > table.generation_limit || index >= table.capacity) {
        return OperationResult<TrampolineEntry>::failure(detail::stale("stxlib.trampoline.enter"));
    }
    state::ThreadState* const kept = state::current_thread_state();
    if (kept == nullptr) {
        // No block: the open-invocation stack is not in the failure reserve.
        if (state::current_flags().phase == state::ThreadPhase::ended) {
            const ErrorRecord ended = ErrorRecord::make(
                ErrorKind::disposed, "stxlib.trampoline.enter",
                ErrorDetail{.message = "this thread's state was released at its exit"});
            return OperationResult<TrampolineEntry>::failure(ended);
        }
        const ErrorRecord unallocated = ErrorRecord::make(
            ErrorKind::out_of_memory, "stxlib.trampoline.enter",
            ErrorDetail{.message = "this thread's state could not be allocated"});
        return OperationResult<TrampolineEntry>::failure(unallocated);
    }
    state::ThreadState& thread = *kept;
    if (thread.open_depth == trampoline_nesting_limit) {
        const ErrorRecord too_deep = ErrorRecord::make(
            ErrorKind::limit_exceeded, "stxlib.trampoline.enter",
            ErrorDetail{.message = "trampoline invocations are nested deeper than trampoline_nesting_limit"});
        return OperationResult<TrampolineEntry>::failure(too_deep);
    }
    detail::TrampolineSlot& slot = table.slots[index];
    slot.in_flight.fetch_add(1, std::memory_order_seq_cst);
    const u64 state = slot.state.load(std::memory_order_seq_cst);
    if (state != detail::make_state(generation, detail::Phase::attached)) {
        detail::depart(slot);
        if (state == detail::make_state(generation, detail::Phase::revoked)) {
            const ErrorRecord revoked = ErrorRecord::make(ErrorKind::revoked, "stxlib.trampoline.enter",
                                                          ErrorDetail{.message = "the trampoline handle is revoked"});
            return OperationResult<TrampolineEntry>::failure(revoked);
        }
        return OperationResult<TrampolineEntry>::failure(detail::stale("stxlib.trampoline.enter"));
    }
    void* const target = slot.target.load(std::memory_order_relaxed);
    thread.open[thread.open_depth] = state::OpenInvocation{slots_, handle.value()};
    thread.open_depth = thread.open_depth + 1;
    const TrampolineEntry entry{target, handle};
    return OperationResult<TrampolineEntry>::success(entry);
}

void TrampolineTable::leave(const TrampolineEntry& entry) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::leave on a moved-from table");
    state::ThreadState* const kept = state::current_thread_state();
    STXLIB_REQUIRE(kept != nullptr, "TrampolineTable::leave without an open invocation");
    state::ThreadState& thread = *kept;
    STXLIB_REQUIRE(thread.open_depth > 0, "TrampolineTable::leave without an open invocation");
    const state::OpenInvocation& top = thread.open[thread.open_depth - 1];
    STXLIB_REQUIRE(top.table == slots_ && top.handle == entry.handle.value(),
                   "TrampolineTable::leave out of order");
    thread.open_depth = thread.open_depth - 1;
    detail::TrampolineSlots& table = *slots_;
    const usize index = table.index_of(entry.handle);
    detail::depart(table.slots[index]);
}

OperationResult<Revocation> TrampolineTable::revoke(const TrampolineHandle handle) noexcept {
    STXLIB_REQUIRE(slots_ != nullptr, "TrampolineTable::revoke on a moved-from table");
    detail::TrampolineSlots& table = *slots_;
    const usize index = table.index_of(handle);
    const u64 generation = table.generation_of_handle(handle);
    if (generation == 0 || generation > table.generation_limit || index >= table.capacity) {
        return OperationResult<Revocation>::failure(detail::stale("stxlib.trampoline.revoke"));
    }
    detail::TrampolineSlot& slot = table.slots[index];
    u64 expected = detail::make_state(generation, detail::Phase::attached);
    const bool revoked = slot.state.compare_exchange_strong(
        expected, detail::make_state(generation, detail::Phase::revoked), std::memory_order_seq_cst);
    if (!revoked) return OperationResult<Revocation>::failure(detail::stale("stxlib.trampoline.revoke"));
    if (detail::inside(slots_, handle.value())) return OperationResult<Revocation>::success(Revocation::deferred);
    detail::wait_until_freed(slot, generation);
    return OperationResult<Revocation>::success(Revocation::completed);
}

} // namespace stxlib
