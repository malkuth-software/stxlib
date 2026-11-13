// Compiled with -fno-exceptions (CMakeLists.txt): the gates are direct calls
// that pass the target's result through, so nothing changes shape when a
// consumer drops exceptions (A1 design, section 6).

#include "stxlib/boundary.hpp"

#include "test_support.hpp"

#if defined(__cpp_exceptions)
#error "this test must be compiled without exceptions"
#endif

namespace {

struct Halves final {
    stxlib::Result<int> operator()(int& value) const {
        if (value % 2 != 0) {
            const stxlib::ErrorRecord odd = stxlib::ErrorRecord::make(stxlib::ErrorKind::invalid_argument,
                                                                      "stxlib.tests.halves");
            return stxlib::Result<int>::failure(odd);
        }
        const int half = value / 2;
        return stxlib::Result<int>::success(half);
    }
};

struct Squares final {
    int operator()(int& value) const {
        const int square = value * value;
        return square;
    }
};

} // namespace

int main() {
    Halves halves;
    int even = 8;
    stxlib::Result<int> half = stxlib::call_gate("stxlib.tests.gate", halves, even);
    CHECK(half.succeeded() && *half.value() == 4);
    int odd = 3;
    const stxlib::Result<int> refused = stxlib::call_gate("stxlib.tests.gate", halves, odd);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::invalid_argument);
    Squares squares;
    int four = 4;
    CHECK(stxlib::invoke_callback("stxlib.tests.callback", squares, four) == 16);
    const int status = stxlib_tests::finish("stxlib gates without exceptions");
    return status;
}
