#pragma once

// Trampolines with revocation: how a foreign caller (an operating-system
// callback, a C library's callback, a C ABI client's table) reaches one of
// our targets through a plain function pointer and a context pointer, and
// how we take that reach away again (house style, section 4: stop revoked
// callbacks; section 5: a revoked callback must not still be running).
//
// The foreign side holds a TrampolineHandle as its context pointer. Our
// entry function (a plain function the consumer writes with the foreign
// signature) turns the context back into the handle and calls
// Trampolines<Target>::invoke, which:
//   1. enters the handle's slot: refused with stale_identity when the slot
//      has been reissued or never existed, with revoked once the handle is
//      revoked, with limit_exceeded past trampoline_nesting_limit nested
//      invocations on this thread;
//   2. calls the target through call_gate (gate.hpp), so no exception
//      unwinds into the foreign caller;
//   3. leaves the slot on every way out (a scope object).
//
// Revocation:
// - revoke(handle) from any thread that is not inside an invocation of
//   that handle stops new invocations at once and returns "completed" only
//   after every invocation already running has left the slot;
// - revoke(handle) from inside an invocation of that same handle (a target
//   that disconnects itself) cannot wait for itself: it stops new
//   invocations at once and returns "deferred"; the slot is released when
//   the last running invocation leaves it.
// Either way, once revoke returns no new invocation of the handle starts,
// and the handle is never valid again: a slot's next attachment has a new
// generation.
//
// Handles: a slot index (16 bits) and a generation (48 bits), so a handle is
// never reused while 2^48 - 1 attachments of its slot remain; a slot whose
// generations are exhausted is retired, never wrapped. The packed value is
// never 0, so a null context is never a valid handle.
//
// A table is owned by one object and created with a fixed capacity (at most
// trampoline_capacity_limit slots). Destroying it revokes every attached
// handle and waits for running invocations; it must not be destroyed from
// inside one of its own invocations.

#include "stxlib/detail/early_exit.hpp"
#include "stxlib/error.hpp"
#include "stxlib/gate.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

inline constexpr usize trampoline_capacity_limit = 65535;
// Nested trampoline invocations one thread may be inside (house style,
// section 1: nested dispatch has a stated, enforced depth).
inline constexpr usize trampoline_nesting_limit = 32;

class TrampolineHandle final {
public:
    // No handle.
    constexpr TrampolineHandle() noexcept = default;

    [[nodiscard]] constexpr u64 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool present() const noexcept { return value_ != 0; }

    // The handle as a foreign context pointer, and back. A context that did
    // not come from as_context() is input: enter() rejects it.
    [[nodiscard]] void* as_context() const noexcept {
        void* const result = reinterpret_cast<void*>(static_cast<uptr>(value_));
        return result;
    }
    [[nodiscard]] static TrampolineHandle from_context(const void* const context) noexcept {
        TrampolineHandle result;
        result.value_ = static_cast<u64>(reinterpret_cast<uptr>(context));
        return result;
    }
    [[nodiscard]] static constexpr TrampolineHandle from_value(const u64 packed_value) noexcept {
        TrampolineHandle result;
        result.value_ = packed_value;
        return result;
    }

    [[nodiscard]] constexpr bool operator==(const TrampolineHandle& other) const noexcept {
        return value_ == other.value_;
    }
    [[nodiscard]] constexpr bool operator!=(const TrampolineHandle& other) const noexcept {
        return value_ != other.value_;
    }

private:
    u64 value_{0};
};

enum class Revocation : u8 {
    completed = 1, // no invocation of the handle is running
    deferred = 2,  // revoked from inside its own invocation; released when it leaves
};

// An invocation in progress: the slot entered and its target. Made by
// TrampolineTable::enter, ended by TrampolineTable::leave.
struct TrampolineEntry final {
    void* target{nullptr};
    TrampolineHandle handle{};
};

namespace detail {
class TrampolineSlots;
} // namespace detail

// The untyped table. Targets are borrowed: each stays alive until its
// handle's revocation has completed (or, deferred, until its last running
// invocation has left). Early exits (AGENTS.md): an exception leaving a
// target is caught by the invocation's gate, and the invocation's visit
// guard closes the visit as the stack unwinds; a longjmp out of a target
// leaves its visit open, so revoking that handle waits for ever.
class TrampolineTable final {
public:
    // A table of `capacity` slots: 1 to trampoline_capacity_limit, else
    // invalid_argument; out_of_memory when its storage cannot be allocated.
    [[nodiscard]] static Result<TrampolineTable> create(usize capacity) noexcept;

    TrampolineTable(TrampolineTable&& other) noexcept : slots_(other.slots_) { other.slots_ = nullptr; }
    TrampolineTable(const TrampolineTable&) = delete;
    TrampolineTable& operator=(const TrampolineTable&) = delete;
    TrampolineTable& operator=(TrampolineTable&&) = delete;
    // Revokes every attached handle, waits for running invocations, frees
    // the slots. Requirement: not called from inside an invocation of this
    // table.
    ~TrampolineTable();

    [[nodiscard]] usize capacity() const noexcept;

    // Attaches `target` to a free slot; limit_exceeded when none is free.
    // Requirement: target is non-null.
    [[nodiscard]] Result<TrampolineHandle> attach(void* target) noexcept;

    // Steps 1 and 3 above. Every successful enter is matched by one leave
    // on the same thread.
    [[nodiscard]] Result<TrampolineEntry> enter(TrampolineHandle handle) noexcept;
    void leave(const TrampolineEntry& entry) noexcept;

    // See "Revocation" above. stale_identity when the handle is not
    // attached in this table (never was, already revoked, or reissued).
    [[nodiscard]] Result<Revocation> revoke(TrampolineHandle handle) noexcept;

private:
    explicit TrampolineTable(detail::TrampolineSlots* const slots) noexcept : slots_(slots) {}

    detail::TrampolineSlots* slots_;
};

// Leaves an entered slot on every way out of an invocation.
class STXLIB_EARLY_EXIT_GUARD STXLIB_EARLY_EXIT_OPEN TrampolineVisit final {
public:
    TrampolineVisit(TrampolineTable& table, const TrampolineEntry& entry) noexcept : table_(table), entry_(entry) {}
    TrampolineVisit(const TrampolineVisit&) = delete;
    TrampolineVisit& operator=(const TrampolineVisit&) = delete;
    ~TrampolineVisit() { table_.leave(entry_); }

private:
    TrampolineTable& table_;
    TrampolineEntry entry_;
};

// The typed table: every attached target is a Target, invoked as
// target(arguments...) returning Status or Result<Value>.
template <typename Target>
class Trampolines final {
public:
    [[nodiscard]] static Result<Trampolines> create(const Operation operation, const usize capacity) noexcept {
        Result<TrampolineTable> table = TrampolineTable::create(capacity);
        if (table.failed()) return Result<Trampolines>::failure(*table.error());
        Trampolines result(operation, static_cast<Result<TrampolineTable>&&>(table).take_value());
        return Result<Trampolines>::success(static_cast<Trampolines&&>(result));
    }

    Trampolines(Trampolines&&) noexcept = default;
    Trampolines(const Trampolines&) = delete;
    Trampolines& operator=(const Trampolines&) = delete;
    Trampolines& operator=(Trampolines&&) = delete;
    ~Trampolines() = default;

    // `target` is borrowed until its revocation completes.
    [[nodiscard]] Result<TrampolineHandle> attach(Target& target) noexcept {
        Result<TrampolineHandle> result = table_.attach(&target);
        return result;
    }

    [[nodiscard]] Result<Revocation> revoke(const TrampolineHandle handle) noexcept {
        Result<Revocation> result = table_.revoke(handle);
        return result;
    }

    // The invocation sequence above. A refused entry returns its failure as
    // the target's result type.
    template <typename... Arguments>
    [[nodiscard]] typename detail::TargetReturn<Target, Arguments...>::Type invoke(const TrampolineHandle handle,
                                                                                  Arguments&... arguments) noexcept {
        using Return = typename detail::TargetReturn<Target, Arguments...>::Type;
        Result<TrampolineEntry> entered = table_.enter(handle);
        if (entered.failed()) return Return::failure(*entered.error());
        const TrampolineEntry entry = *entered.value();
        const TrampolineVisit visit(table_, entry);
        Target& target = *static_cast<Target*>(entry.target);
        Return result = call_gate(operation_, target, arguments...);
        return result;
    }

private:
    Trampolines(const Operation operation, TrampolineTable&& table) noexcept
        : operation_(operation), table_(static_cast<TrampolineTable&&>(table)) {}

    Operation operation_;
    TrampolineTable table_;
};

} // namespace stxlib
