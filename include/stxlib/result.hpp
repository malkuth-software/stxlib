#pragma once

// OperationStatus and OperationResult<Value>: the outcome of an operation, either success
// (with its value) or failure (with its error record), never both (house
// style, section 2: a result is its value or its failure).
//
// Public headers cannot hold std::variant (no std:: type, AGENTS.md), so
// each is a tagged union with a layout defined here: a one-byte outcome,
// then the active alternative. It is built only through the named factories
// success() and failure(), and read only through the named queries, which
// return null for an inactive alternative rather than reading it.
//
// A result is made, moved and read; it is not reassigned (assignment is
// deleted), so no alternative is ever replaced in place.

#include "stxlib/error.hpp"
#include "stxlib/require.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

class OperationStatus final {
public:
    [[nodiscard]] static OperationStatus success() noexcept {
        const OperationStatus result;
        return result;
    }
    [[nodiscard]] static OperationStatus failure(const ErrorRecord& record) noexcept {
        const OperationStatus result(record);
        return result;
    }

    [[nodiscard]] bool succeeded() const noexcept { return outcome_ == Outcome::success; }
    [[nodiscard]] bool failed() const noexcept { return outcome_ == Outcome::failure; }

    // The failure's record, or null on success. Borrowed from this status.
    [[nodiscard]] const ErrorRecord* error() const& noexcept {
        if (outcome_ != Outcome::failure) return nullptr;
        return &state_.record;
    }
    const ErrorRecord* error() const&& = delete;

private:
    enum class Outcome : u8 {
        success = 0,
        failure = 1,
    };
    struct Succeeded final {};
    union State {
        Succeeded succeeded;
        ErrorRecord record;

        State() noexcept : succeeded() {}
        explicit State(const ErrorRecord& failure_record) noexcept : record(failure_record) {}
    };

    OperationStatus() noexcept : outcome_(Outcome::success), state_() {}
    explicit OperationStatus(const ErrorRecord& record) noexcept : outcome_(Outcome::failure), state_(record) {}

    Outcome outcome_;
    State state_;
};

template <typename Value>
class OperationResult final {
    // A result moves its value without a failure path (house style, section
    // 1: element relocation must not escape through another mechanism).
    static_assert(__is_nothrow_constructible(Value, Value&&),
                  "OperationResult's Value must be nothrow move constructible");

public:
    [[nodiscard]] static OperationResult success(Value succeeded_value) noexcept {
        OperationResult result(ValueTag{}, static_cast<Value&&>(succeeded_value));
        return result;
    }
    [[nodiscard]] static OperationResult failure(const ErrorRecord& record) noexcept {
        OperationResult result(FailureTag{}, record);
        return result;
    }

    // Moves the active alternative; `other` keeps its outcome and holds a
    // moved-from value (as Value's move defines it) or the same record.
    // The member initializer selects the alternative with a conditional
    // whose operands are both prvalues of State, so the chosen one
    // initialises state_ directly ([expr.cond], [dcl.init]: no State copy
    // or move constructor is needed or called).
    OperationResult(OperationResult&& other) noexcept
        : outcome_(other.outcome_),
          state_(other.outcome_ == Outcome::success ? State(ValueTag{}, static_cast<Value&&>(other.state_.value))
                                                    : State(FailureTag{}, other.state_.record)) {}

    OperationResult(const OperationResult&) = delete;
    OperationResult& operator=(const OperationResult&) = delete;
    OperationResult& operator=(OperationResult&&) = delete;

    ~OperationResult() {
        if (outcome_ == Outcome::success) state_.value.~Value();
    }

    [[nodiscard]] bool succeeded() const noexcept { return outcome_ == Outcome::success; }
    [[nodiscard]] bool failed() const noexcept { return outcome_ == Outcome::failure; }

    // The value, or null on failure. Borrowed from this result.
    [[nodiscard]] const Value* value() const& noexcept {
        if (outcome_ != Outcome::success) return nullptr;
        return &state_.value;
    }
    [[nodiscard]] Value* value() & noexcept {
        if (outcome_ != Outcome::success) return nullptr;
        return &state_.value;
    }
    Value* value() && = delete;
    const Value* value() const&& = delete;

    // The failure's record, or null on success. Borrowed from this result.
    [[nodiscard]] const ErrorRecord* error() const& noexcept {
        if (outcome_ != Outcome::failure) return nullptr;
        return &state_.record;
    }
    const ErrorRecord* error() const&& = delete;

    // Moves the value out. Requirement: succeeded().
    [[nodiscard]] Value take_value() && noexcept {
        STXLIB_REQUIRE(outcome_ == Outcome::success, "OperationResult::take_value on a failure");
        Value result(static_cast<Value&&>(state_.value));
        return result;
    }

    // The failure as an OperationStatus, or success. For an operation that passes a
    // callee's failure on without its value.
    [[nodiscard]] OperationStatus status() const noexcept {
        if (outcome_ == Outcome::failure) return OperationStatus::failure(state_.record);
        return OperationStatus::success();
    }

private:
    enum class Outcome : u8 {
        success = 0,
        failure = 1,
    };
    struct ValueTag final {};
    struct FailureTag final {};
    union State {
        Value value;
        ErrorRecord record;

        State(ValueTag, Value&& moved) noexcept : value(static_cast<Value&&>(moved)) {}
        State(FailureTag, const ErrorRecord& failure_record) noexcept : record(failure_record) {}
        // OperationResult's destructor ends the active member's lifetime.
        ~State() {}
    };

    OperationResult(ValueTag tag, Value&& moved_value) noexcept
        : outcome_(Outcome::success), state_(tag, static_cast<Value&&>(moved_value)) {}
    OperationResult(FailureTag tag, const ErrorRecord& record) noexcept : outcome_(Outcome::failure), state_(tag,
                                                                                                             record) {}

    Outcome outcome_;
    State state_;
};

} // namespace stxlib
