#pragma once

// Status and Result<Value>: the outcome of an operation, either success
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

class Status final {
public:
    [[nodiscard]] static Status success() noexcept {
        const Status result;
        return result;
    }
    [[nodiscard]] static Status failure(const ErrorRecord& record) noexcept {
        const Status result(record);
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

    Status() noexcept : outcome_(Outcome::success), state_() {}
    explicit Status(const ErrorRecord& record) noexcept : outcome_(Outcome::failure), state_(record) {}

    Outcome outcome_;
    State state_;
};

template <typename Value>
class Result final {
    // A result moves its value without a failure path (house style, section
    // 1: element relocation must not escape through another mechanism).
    static_assert(__is_nothrow_constructible(Value, Value&&), "Result's Value must be nothrow move constructible");

public:
    [[nodiscard]] static Result success(Value succeeded_value) noexcept {
        Result result(ValueTag{}, static_cast<Value&&>(succeeded_value));
        return result;
    }
    [[nodiscard]] static Result failure(const ErrorRecord& record) noexcept {
        Result result(FailureTag{}, record);
        return result;
    }

    // Moves the active alternative; `other` keeps its outcome and holds a
    // moved-from value (as Value's move defines it) or the same record.
    // The member initializer selects the alternative with a conditional
    // whose operands are both prvalues of State, so the chosen one
    // initialises state_ directly ([expr.cond], [dcl.init]: no State copy
    // or move constructor is needed or called).
    Result(Result&& other) noexcept
        : outcome_(other.outcome_),
          state_(other.outcome_ == Outcome::success ? State(ValueTag{}, static_cast<Value&&>(other.state_.value))
                                                    : State(FailureTag{}, other.state_.record)) {}

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result& operator=(Result&&) = delete;

    ~Result() {
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

    // The failure's record, or null on success. Borrowed from this result.
    [[nodiscard]] const ErrorRecord* error() const& noexcept {
        if (outcome_ != Outcome::failure) return nullptr;
        return &state_.record;
    }
    const ErrorRecord* error() const&& = delete;

    // Moves the value out. Requirement: succeeded().
    [[nodiscard]] Value take_value() && noexcept {
        STXLIB_REQUIRE(outcome_ == Outcome::success, "Result::take_value on a failure");
        Value result(static_cast<Value&&>(state_.value));
        return result;
    }

    // The failure as a Status, or success. For an operation that passes a
    // callee's failure on without its value.
    [[nodiscard]] Status status() const noexcept {
        if (outcome_ == Outcome::failure) return Status::failure(state_.record);
        return Status::success();
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
        // Result's destructor ends the active member's lifetime.
        ~State() {}
    };

    Result(ValueTag tag, Value&& moved_value) noexcept
        : outcome_(Outcome::success), state_(tag, static_cast<Value&&>(moved_value)) {}
    Result(FailureTag tag, const ErrorRecord& record) noexcept : outcome_(Outcome::failure), state_(tag, record) {}

    Outcome outcome_;
    State state_;
};

} // namespace stxlib
