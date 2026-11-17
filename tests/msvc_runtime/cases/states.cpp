// Control flow out of handlers and try blocks: return, break and continue
// from inside a catch, a catch in a loop, goto out of a nested try, and an
// exception thrown after a caught one in the same frame, so the frame's
// state after each catch is checked.

#include "case_support.hpp"

namespace {

int returns_from_catch(const int value) {
    Tracker outer("outer");
    try {
        Tracker inner("inner");
        throw value;
    } catch (const int caught) {
        return caught + 1;
    }
    return -1;
}

int loop_with_catches() {
    int total = 0;
    for (int round = 0; round < 6; ++round) {
        try {
            if (round % 2 == 0) throw round;
            total = total + 100;
        } catch (const int caught) {
            if (caught == 2) continue;
            if (caught == 4) break;
            total = total + caught;
        }
    }
    return total;
}

int leaves_by_goto() {
    int stage = 0;
    try {
        try {
            stage = 1;
            throw 1.5;
        } catch (const double) {
            stage = 2;
            goto done;
        }
    } catch (...) {
        stage = -1;
    }
done:
    return stage;
}

void second_throw_after_catch() {
    try {
        throw 1;
    } catch (const int) {
        say("first caught");
    }
    Tracker after("after first");
    try {
        throw 2;
    } catch (const int caught) {
        SAYF("second caught %d", caught);
    }
    throw 3;
}

} // namespace

int main() {
    SAYF("return from catch %d", returns_from_catch(4));
    SAYF("loop total %d", loop_with_catches());
    SAYF("goto stage %d", leaves_by_goto());
    try {
        second_throw_after_catch();
    } catch (const int caught) {
        SAYF("third caught %d uncaught=%d", caught, std::uncaught_exceptions());
    }
    return 0;
}
