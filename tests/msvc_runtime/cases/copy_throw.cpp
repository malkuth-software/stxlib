// A catch by value whose copy constructor throws while the handler's
// parameter is initialised: std::terminate ([except.terminate]p1, "when
// the exception handling mechanism, after completing the initialization of
// the exception object but before activation of a handler for the
// exception, calls a function that exits via an exception").

#include "case_support.hpp"

namespace {

struct Touchy {
    int value;
    explicit Touchy(const int touchy_value) : value(touchy_value) {}
    Touchy(const Touchy& other) : value(other.value) {
        if (other.value == 2) {
            say("copy throws");
            throw 5;
        }
    }
};

} // namespace

int main() {
    std::set_terminate(case_terminate_handler);
    try {
        throw Touchy(1);
    } catch (const Touchy caught) {
        SAYF("copied %d", caught.value);
    }
    try {
        throw Touchy(2);
    } catch (const Touchy caught) {
        SAYF("copied, wrong %d", caught.value);
    } catch (...) {
        say("caught, wrong");
    }
    say("returned, wrong");
    return 0;
}
