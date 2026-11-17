// Throwing inside catch blocks: caught by an outer try in the same
// function, by the caller, by a try nested in the catch body, through two
// levels of catch, and a thousand rounds that must leave nothing behind.

#include "case_support.hpp"

namespace {

int live = 0;

struct Counted {
    int value;
    explicit Counted(const int counted_value) : value(counted_value) {
        live = live + 1;
    }
    Counted(const Counted& other) : value(other.value) {
        live = live + 1;
    }
    ~Counted() {
        live = live - 1;
    }
};

void outer_try_same_function() {
    try {
        try {
            throw Counted(1);
        } catch (const Counted& first) {
            Tracker tracker("in first catch");
            SAYF("inner caught %d", first.value);
            throw 2.0;
        }
    } catch (const double second) {
        SAYF("outer caught %.1f", second);
    }
}

void throw_from_catch() {
    try {
        throw 1;
    } catch (const int) {
        Tracker tracker("local of catch");
        throw 'x';
    }
}

void caller_catches() {
    Tracker tracker("caller frame");
    try {
        throw_from_catch();
    } catch (const char letter) {
        SAYF("caller caught %c", letter);
    }
}

void nested_in_catch() {
    try {
        throw Counted(3);
    } catch (const Counted& outer) {
        try {
            throw Counted(4);
        } catch (const Counted& inner) {
            SAYF("nested %d %d", outer.value, inner.value);
        }
        SAYF("still in outer catch %d live %d", outer.value, live);
    }
}

void two_levels() {
    try {
        try {
            throw 1;
        } catch (const int first) {
            try {
                throw 2;
            } catch (const int second) {
                Tracker tracker("deepest catch");
                SAYF("deep %d %d", first, second);
                throw Counted(3);
            }
        }
    } catch (const Counted& third) {
        SAYF("escaped two catch levels %d", third.value);
    }
}

void catch_in_catch_in_loop(const int rounds) {
    int total = 0;
    for (int round = 0; round < rounds; ++round) {
        try {
            try {
                throw Counted(round);
            } catch (const Counted& first) {
                try {
                    throw first.value + 1;
                } catch (const int second) {
                    throw Counted(second + 1);
                }
            }
        } catch (const Counted& last) {
            total = total + last.value;
        }
    }
    SAYF("rounds %d total %d live %d uncaught %d", rounds, total, live, std::uncaught_exceptions());
}

int returns_from_catch() {
    for (int index = 0; index < 10; ++index) {
        try {
            throw index;
        } catch (const int value) {
            if (value == 3) return value * 10;
            if (value == 1) continue;
        }
    }
    return -1;
}

} // namespace

int main() {
    outer_try_same_function();
    caller_catches();
    nested_in_catch();
    two_levels();
    catch_in_catch_in_loop(1000);
    SAYF("returned from catch %d", returns_from_catch());
    SAYF("live at end %d", live);
    return 0;
}
