// An exception that reaches a noexcept function's boundary: std::terminate
// ([except.spec]p5), with a handler waiting outside that must not run.

#include "case_support.hpp"

namespace {

void thrower(const int value) {
    if (value > 0) throw value;
}

void sealed(const int value) noexcept {
    Tracker tracker("inside noexcept");
    thrower(value);
}

} // namespace

int main() {
    std::set_terminate(case_terminate_handler);
    sealed(0);
    say("sealed without throw returned");
    try {
        sealed(1);
    } catch (...) {
        say("caught, wrong");
    }
    say("returned, wrong");
    return 0;
}
