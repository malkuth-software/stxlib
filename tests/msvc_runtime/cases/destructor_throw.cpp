// A destructor that throws while another exception unwinds its frame:
// std::terminate ([except.terminate]p1).

#include "case_support.hpp"

namespace {

struct Thrower {
    ~Thrower() noexcept(false) {
        say("thrower destructor throws");
        throw 2;
    }
};

void unwind_through_thrower() {
    Thrower thrower;
    static_cast<void>(thrower);
    throw 1;
}

} // namespace

int main() {
    std::set_terminate(case_terminate_handler);
    try {
        Thrower alone;
        static_cast<void>(alone);
    } catch (const int value) {
        SAYF("destructor throw without unwinding caught %d", value);
    }
    try {
        unwind_through_thrower();
    } catch (...) {
        say("caught, wrong");
    }
    say("returned, wrong");
    return 0;
}
