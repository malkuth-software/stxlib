// Compiled with -fno-exceptions (CMakeLists.txt): the gates are direct calls
// that pass the target's result through, so nothing changes shape when a
// consumer drops exceptions (A1 design, section 6). Also the A3 helpers
// (notes planning/stages/A3-no-exceptions.md, section 1.5): call_as_result
// wraps a plain target's value as a success, and the fail-fast helpers pass
// a success on. Every public header is included, so each one compiles
// without exceptions.

#include "stxlib/boundary.hpp"
#include "stxlib/calendar.hpp"
#include "stxlib/hash.hpp"
#include "stxlib/math.hpp"
#include "stxlib/numeric_text.hpp"
#include "stxlib/precedence.hpp"
#include "stxlib/random.hpp"
#include "stxlib/row_height_index.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/voting.hpp"

#include "test_support.hpp"

#if defined(__cpp_exceptions)
#error "this test must be compiled without exceptions"
#endif

namespace {

struct Halves final {
    stxlib::OperationResult<int> operator()(int& value) const {
        if (value % 2 != 0) {
            const stxlib::ErrorRecord odd = stxlib::ErrorRecord::make(stxlib::ErrorKind::invalid_argument,
                                                                      "stxlib.tests.halves");
            return stxlib::OperationResult<int>::failure(odd);
        }
        const int half = value / 2;
        return stxlib::OperationResult<int>::success(half);
    }
};

struct Squares final {
    int operator()(int& value) const {
        const int square = value * value;
        return square;
    }
};

struct Counts final {
    int calls{0};
    void operator()() { calls = calls + 1; }
};

} // namespace

int main() {
    Halves halves;
    int even = 8;
    stxlib::OperationResult<int> half = stxlib::call_gate("stxlib.tests.gate", halves, even);
    CHECK(half.succeeded() && *half.value() == 4);
    int odd = 3;
    const stxlib::OperationResult<int> refused = stxlib::call_gate("stxlib.tests.gate", halves, odd);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument);
    int six = 6;
    const stxlib::OperationResult<int> framed = stxlib::call_gate(stxlib::GateFrame{"stxlib.tests.gate", 24, 3}, halves,
                                                                  six);
    CHECK(framed.succeeded() && *framed.value() == 3);
    Squares squares;
    int four = 4;
    CHECK(stxlib::invoke_callback("stxlib.tests.callback", squares, four) == 16);
    // The A2 forms are direct calls too.
    const stxlib::GateFrame event{
        .operation = "stxlib.tests.event", .kind = 3, .subject = "button.ok", .name = "click", .sequence = 1};
    CHECK(stxlib::invoke_callback(event, squares, four) == 16);
    const stxlib::ExceptionClassifier classifier{};
    CHECK(stxlib::invoke_callback_classified(classifier, event, squares, four) == 16);
    const stxlib::ShadowFrameText dispatch{.kind = 2, .subject = "form.main", .name = "key"};
    CHECK(stxlib::in_shadow_frame(dispatch, squares, four) == 16);
    CHECK(stxlib::shadow_depth() == 0);
    // The A3 helpers.
    stxlib::OperationResult<int> squared = stxlib::call_as_result("stxlib.tests.square", squares, four);
    CHECK(squared.succeeded() && *squared.value() == 16);
    const stxlib::GateFrame square_frame{"stxlib.tests.square", 1, 2};
    stxlib::OperationResult<int> framed_square = stxlib::call_as_result(square_frame, squares, four);
    CHECK(framed_square.succeeded() && *framed_square.value() == 16);
    stxlib::OperationResult<int> classified_square = stxlib::call_as_result_classified(classifier, event, squares,
                                                                                       four);
    CHECK(stxlib::take_or_fail_fast(static_cast<stxlib::OperationResult<int>&&>(classified_square)) == 16);
    Counts counts;
    const stxlib::OperationStatus counted = stxlib::call_as_result("stxlib.tests.count", counts);
    CHECK(counted.succeeded() && counts.calls == 1);
    stxlib::check_or_fail_fast(counted);
    CHECK(stxlib::call_as_result_classified(classifier, "stxlib.tests.count", counts).succeeded() && counts.calls == 2);
    CHECK(stxlib::shadow_depth() == 0);
    const int status = stxlib_tests::finish("stxlib gates without exceptions");
    return status;
}
