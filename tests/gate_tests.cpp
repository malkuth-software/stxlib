// Tests of the gates (include/stxlib/gate.hpp) and the recent-failure list
// (shadow.hpp): each standard exception's classification, the consumer's
// classifier, foreign text, and the pass-through of results. Tested, not
// proved (proofs/REPORT.md).

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#include <new>
#include <stdexcept>
#include <system_error>

namespace {

// ---- targets --------------------------------------------------------------

struct Succeeds final {
    stxlib::Status operator()() const { return stxlib::Status::success(); }
};

struct Adds final {
    stxlib::Result<int> operator()(int& left, int& right) const {
        const int sum = left + right;
        return stxlib::Result<int>::success(sum);
    }
};

struct ReturnsFailure final {
    stxlib::Status operator()() const {
        const stxlib::ErrorRecord record = stxlib::ErrorRecord::make(stxlib::ErrorKind::not_found, "stxlib.tests.inner");
        return stxlib::Status::failure(record);
    }
};

struct ThrowsBadAlloc final {
    stxlib::Status operator()() const { throw std::bad_alloc(); }
};
struct ThrowsInvalidArgument final {
    stxlib::Status operator()() const { throw std::invalid_argument("bad width"); }
};
struct ThrowsOutOfRange final {
    stxlib::Result<int> operator()() const { throw std::out_of_range("row 9"); }
};
struct ThrowsLengthError final {
    stxlib::Status operator()() const { throw std::length_error("too long"); }
};
struct ThrowsLogicError final {
    stxlib::Status operator()() const { throw std::logic_error("broken"); }
};
struct ThrowsRuntimeError final {
    stxlib::Status operator()() const { throw std::runtime_error("ran badly"); }
};
struct ThrowsSystemError final {
    stxlib::Status operator()() const { throw std::system_error(std::make_error_code(std::errc::permission_denied)); }
};
struct ThrowsInteger final {
    stxlib::Status operator()() const { throw 7; }
};
struct ThrowsIllFormedText final {
    stxlib::Status operator()() const { throw std::runtime_error("ok\xff tail"); }
};

// A consumer's own exception type and its classifier.
struct WrongThreadError final {};

bool classify_wrong_thread(void*, stxlib::ExceptionClass& decided) noexcept {
    try {
        throw;
    } catch (const WrongThreadError&) {
        decided.kind = stxlib::ErrorKind::wrong_thread;
        decided.code = 77;
        return true;
    } catch (...) {
        return false;
    }
}

struct ThrowsWrongThread final {
    stxlib::Status operator()() const { throw WrongThreadError(); }
};

struct ReturnsNumber final {
    int operator()(int& value) const {
        const int result = value * 2;
        return result;
    }
};

struct CountsCalls final {
    int calls{0};
    void operator()() { calls = calls + 1; }
};

int doubled(void* const context, int& value) {
    const int scale = *static_cast<int*>(context);
    const int result = value * scale;
    return result;
}

// ---- tests ----------------------------------------------------------------

template <typename Target>
stxlib::ErrorRecord failure_of(Target& target) {
    const stxlib::Status status = stxlib::call_gate("stxlib.tests.gate", target);
    STXLIB_REQUIRE(status.failed(), "the target was expected to fail");
    const stxlib::ErrorRecord record = *status.error();
    return record;
}

void results_pass_through() {
    Succeeds succeeds;
    CHECK(stxlib::call_gate("stxlib.tests.gate", succeeds).succeeded());
    Adds adds;
    int left = 2;
    int right = 3;
    stxlib::Result<int> sum = stxlib::call_gate("stxlib.tests.gate", adds, left, right);
    CHECK(sum.succeeded() && *sum.value() == 5);
    ReturnsFailure returns_failure;
    const stxlib::Status inner = stxlib::call_gate("stxlib.tests.gate", returns_failure);
    CHECK(inner.failed() && (*inner.error()).kind() == stxlib::ErrorKind::not_found);
    CHECK((*inner.error()).operation().name() == stxlib::TextView("stxlib.tests.inner"));
}

void standard_exceptions_are_classified() {
    stxlib::clear_recent_failures();
    ThrowsBadAlloc bad_alloc;
    CHECK(failure_of(bad_alloc).kind() == stxlib::ErrorKind::out_of_memory);
    ThrowsInvalidArgument invalid;
    const stxlib::ErrorRecord invalid_record = failure_of(invalid);
    CHECK(invalid_record.kind() == stxlib::ErrorKind::invalid_argument);
    CHECK(invalid_record.message() == stxlib::TextView("bad width"));
    CHECK(invalid_record.operation().name() == stxlib::TextView("stxlib.tests.gate"));
    ThrowsLengthError length;
    CHECK(failure_of(length).kind() == stxlib::ErrorKind::limit_exceeded);
    ThrowsLogicError logic;
    CHECK(failure_of(logic).kind() == stxlib::ErrorKind::internal_failure);
    ThrowsRuntimeError runtime;
    CHECK(failure_of(runtime).kind() == stxlib::ErrorKind::internal_failure);
    ThrowsSystemError system;
    const stxlib::ErrorRecord system_record = failure_of(system);
    CHECK(system_record.kind() == stxlib::ErrorKind::foreign_failure);
    CHECK(system_record.foreign().present());
    CHECK(system_record.foreign().value() == static_cast<stxlib::i64>(static_cast<int>(std::errc::permission_denied)));
    ThrowsInteger integer;
    const stxlib::ErrorRecord integer_record = failure_of(integer);
    CHECK(integer_record.kind() == stxlib::ErrorKind::foreign_exception);
    CHECK(integer_record.message().empty());

    ThrowsOutOfRange out_of_range;
    stxlib::Result<int> range = stxlib::call_gate("stxlib.tests.gate", out_of_range);
    CHECK(range.failed() && (*range.error()).kind() == stxlib::ErrorKind::out_of_range);
}

void foreign_text_is_cut_at_the_first_bad_byte() {
    ThrowsIllFormedText ill_formed;
    const stxlib::ErrorRecord record = failure_of(ill_formed);
    CHECK(record.message() == stxlib::TextView("ok"));
    CHECK(record.message_truncated());
}

void the_classifier_comes_first() {
    const stxlib::ExceptionClassifier classifier{classify_wrong_thread, nullptr};
    ThrowsWrongThread wrong_thread;
    const stxlib::Status classified = stxlib::call_gate_classified(classifier, "stxlib.tests.gate", wrong_thread);
    CHECK(classified.failed());
    CHECK((*classified.error()).kind() == stxlib::ErrorKind::wrong_thread);
    CHECK((*classified.error()).code() == 77);
    ThrowsInvalidArgument invalid;
    const stxlib::Status standard = stxlib::call_gate_classified(classifier, "stxlib.tests.gate", invalid);
    CHECK((*standard.error()).kind() == stxlib::ErrorKind::invalid_argument);
}

void development_builds_keep_type_and_history() {
    stxlib::clear_recent_failures();
    ThrowsInvalidArgument invalid;
    const stxlib::ErrorRecord record = failure_of(invalid);
    ThrowsLengthError length;
    const stxlib::ErrorRecord newer = failure_of(length);
    if (stxlib::shadowing_enabled()) {
        CHECK(!record.exception_type().empty());
        CHECK(stxlib::recent_failure_count() == 2);
        stxlib::Result<stxlib::ErrorRecord> newest = stxlib::recent_failure(0);
        CHECK(newest.succeeded() && (*newest.value()).sequence() == newer.sequence());
        stxlib::Result<stxlib::ErrorRecord> older = stxlib::recent_failure(1);
        CHECK(older.succeeded() && (*older.value()).sequence() == record.sequence());
        CHECK(stxlib::recent_failure(2).failed());
        for (int round = 0; round < 20; ++round) {
            const stxlib::ErrorRecord filler = failure_of(invalid);
            static_cast<void>(filler);
        }
        CHECK(stxlib::recent_failure_count() == stxlib::recent_failure_capacity);
    } else {
        CHECK(record.exception_type().empty());
        CHECK(stxlib::recent_failure_count() == 0);
    }
    // A shadow record in development builds only (shadow_tests.cpp shows
    // its contents).
    CHECK(record.shadow().present() == stxlib::shadowing_enabled());
}

void callbacks_pass_through() {
    ReturnsNumber returns_number;
    int value = 21;
    const int result = stxlib::invoke_callback("stxlib.tests.callback", returns_number, value);
    CHECK(result == 42);
    CountsCalls counts;
    stxlib::invoke_callback("stxlib.tests.callback", counts);
    CHECK(counts.calls == 1);
    int scale = 3;
    const stxlib::CallbackTarget<int(int&)> target = stxlib::CallbackTarget<int(int&)>::bind(doubled, &scale);
    int input = 5;
    const int scaled = stxlib::invoke_callback_target("stxlib.tests.callback", target, input);
    CHECK(scaled == 15);
}

} // namespace

int main() {
    results_pass_through();
    standard_exceptions_are_classified();
    foreign_text_is_cut_at_the_first_bad_byte();
    the_classifier_comes_first();
    development_builds_keep_type_and_history();
    callbacks_pass_through();
    const int status = stxlib_tests::finish("stxlib gates");
    return status;
}
