#pragma once

// Optional<Value>: a value or nothing, the owned vocabulary's replacement
// for std::optional at a public surface (owner, D15 before 1.0; the public
// state shape of the roadmap's choice 10: a tagged union behind named
// factories and named queries).
//
// Shape: a one-byte presence tag, then the value's storage. Built only
// through none(), of() and the default constructor (none), read only
// through present(), value() (null when absent, never reading inactive
// storage) and value_or(). No conversion from or to Value is implicit.
//
// Unlike OperationResult, an Optional is an ordinary field type: it can be assigned
// (set(), reset(), and copy or move assignment), because the records it
// replaces std::optional in (layout margins, selected indices, colours) are
// plain data that callers update. Assignment takes the source's value into
// a local first (a copy or a move), then destroys the old value, then
// constructs the new one from the local; no step can fail. Taking the
// source first keeps an optional assigned from one held inside its own
// value correct (security audit 2026-11-15, A-stxlib-include-01-F2).
//
// Value: any type whose move constructor and destructor cannot throw. An
// Optional is copyable when Value has a non-throwing copy constructor, and
// trivially copyable (so a Vector of it relocates by memcpy and a record
// holding it stays plain data) when Value is.
//
// Comparison: operator== is true when both are absent, or both present with
// equal values by Value's operator== (IEEE equality for floating point: a
// present NaN is unequal to everything, +0 equals -0).

// Early exits (owner, 2026-11-15, decisions/2026-11-15-early-exit-
// integrity.md): Value's constructors, destructor and operator== are the
// consumer's code. Form 1 at every call: presence is set only after a
// construction completes (copy, move, of, set) and cleared before the
// destructor runs (reset, take, set, assignment), so the flag never claims
// a value that is not live; value_or and operator== change nothing.

#include "stxlib/detail/construct.hpp"
#include "stxlib/require.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

template <typename Value>
class Optional final {
    static_assert(__is_nothrow_constructible(Value, Value&&), "Optional's Value must be nothrow move constructible");
    static_assert(detail::nothrow_destructible<Value>, "Optional's Value must be nothrow destructible");

    static constexpr bool copyable = __is_nothrow_constructible(Value, const Value&);
    static constexpr bool trivial = __is_trivially_copyable(Value);

public:
    // Absent.
    constexpr Optional() noexcept : present_(false), storage_() {}

    [[nodiscard]] static constexpr Optional none() noexcept {
        Optional result;
        return result;
    }
    [[nodiscard]] static constexpr Optional of(Value contained) noexcept {
        Optional result(PresentTag{}, static_cast<Value&&>(contained));
        return result;
    }

    // Copying: trivial for a trivially copyable Value, member-wise
    // otherwise, absent for a Value that cannot be copied without throwing.
    constexpr Optional(const Optional& other) noexcept
        requires(trivial)
    = default;
    Optional(const Optional& other) noexcept
        requires(copyable && !trivial)
        : present_(false), storage_() {
        if (other.present_) {
            detail::construct<Value>(&storage_.value, other.storage_.value);
            present_ = true;
        }
    }
    constexpr Optional& operator=(const Optional& other) noexcept
        requires(trivial)
    = default;
    Optional& operator=(const Optional& other) noexcept
        requires(copyable && !trivial)
    {
        if (this == &other) return *this;
        if (!other.present_) {
            reset();
            return *this;
        }
        Value copied(other.storage_.value);
        reset();
        detail::construct<Value>(&storage_.value, static_cast<Value&&>(copied));
        present_ = true;
        return *this;
    }

    // Moving: other keeps its presence, holding a moved-from value (as
    // Value's move defines it).
    constexpr Optional(Optional&& other) noexcept
        requires(trivial)
    = default;
    Optional(Optional&& other) noexcept
        requires(!trivial)
        : present_(false), storage_() {
        if (other.present_) {
            detail::construct<Value>(&storage_.value, static_cast<Value&&>(other.storage_.value));
            present_ = true;
        }
    }
    constexpr Optional& operator=(Optional&& other) noexcept
        requires(trivial)
    = default;
    Optional& operator=(Optional&& other) noexcept
        requires(!trivial)
    {
        if (this == &other) return *this;
        if (!other.present_) {
            reset();
            return *this;
        }
        Value taken(static_cast<Value&&>(other.storage_.value));
        reset();
        detail::construct<Value>(&storage_.value, static_cast<Value&&>(taken));
        present_ = true;
        return *this;
    }

    constexpr ~Optional()
        requires(trivial)
    = default;
    constexpr ~Optional()
        requires(!trivial)
    {
        reset();
    }

    // ---- queries ----------------------------------------------------------

    [[nodiscard]] constexpr bool present() const noexcept { return present_; }
    [[nodiscard]] constexpr bool absent() const noexcept { return !present_; }

    // The value, or null when absent. Borrowed from this optional: valid
    // until it is reset, assigned, moved from or destroyed.
    [[nodiscard]] constexpr const Value* value() const& noexcept {
        if (!present_) return nullptr;
        return &storage_.value;
    }
    [[nodiscard]] constexpr Value* value() & noexcept {
        if (!present_) return nullptr;
        return &storage_.value;
    }
    const Value* value() const&& = delete;
    Value* value() && = delete;

    // A copy of the value, or `fallback` when absent.
    [[nodiscard]] constexpr Value value_or(const Value& fallback) const noexcept {
        static_assert(copyable, "Optional::value_or needs a nothrow copy constructor");
        if (!present_) {
            const Value result(fallback);
            return result;
        }
        const Value result(storage_.value);
        return result;
    }

    // Moves the value out; the optional is absent afterwards.
    // Requirement: present().
    [[nodiscard]] constexpr Value take() noexcept {
        STXLIB_REQUIRE(present_, "Optional::take on an absent value");
        Value result(static_cast<Value&&>(storage_.value));
        reset();
        return result;
    }

    // ---- changes (none can fail) ------------------------------------------

    // Present with `replacement` (moved in); a previous value is destroyed first.
    void set(Value replacement) noexcept {
        reset();
        detail::construct<Value>(&storage_.value, static_cast<Value&&>(replacement));
        present_ = true;
    }

    // Absent; a previous value is destroyed.
    constexpr void reset() noexcept {
        if (!present_) return;
        present_ = false;
        if constexpr (!trivial) detail::destroy(&storage_.value);
    }

    [[nodiscard]] constexpr bool operator==(const Optional& other) const noexcept {
        if (present_ != other.present_) return false;
        if (!present_) return true;
        const bool result = storage_.value == other.storage_.value;
        return result;
    }
    [[nodiscard]] constexpr bool operator!=(const Optional& other) const noexcept {
        const bool equal = *this == other;
        return !equal;
    }

private:
    struct PresentTag final {};
    struct Nothing final {};
    // A union whose special members are trivial exactly when Value's are,
    // so a trivially copyable Value gives a trivially copyable Optional.
    union Storage {
        Nothing nothing;
        Value value;

        constexpr Storage() noexcept : nothing() {}
        constexpr explicit Storage(PresentTag, Value&& moved) noexcept : value(static_cast<Value&&>(moved)) {}
        constexpr Storage(const Storage&) noexcept
            requires(trivial)
        = default;
        constexpr Storage& operator=(const Storage&) noexcept
            requires(trivial)
        = default;
        constexpr ~Storage()
            requires(trivial)
        = default;
        // Optional's own members construct and destroy the active member.
        constexpr ~Storage()
            requires(!trivial)
        {}
    };

    constexpr Optional(PresentTag tag, Value&& moved) noexcept : present_(true), storage_(tag, static_cast<Value&&>(moved)) {}

    bool present_;
    Storage storage_;
};

} // namespace stxlib
