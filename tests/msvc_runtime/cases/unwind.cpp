// Unwinding: the order of cleanups across frames, a partly built array,
// a constructor that throws after its members, temporaries, a
// function-try-block, and std::uncaught_exceptions in destructors that
// run during unwinding, including one that throws and catches inside.

#include "case_support.hpp"

namespace {

struct Element {
    int index;
    explicit Element(const int element_index) : index(element_index) {
        if (element_index == 3) throw element_index;
        SAYF("built element %d", element_index);
    }
    ~Element() {
        SAYF("destroyed element %d uncaught %d", index, std::uncaught_exceptions());
    }
};

struct Holder {
    Tracker first;
    Tracker second;
    Holder() : first("holder member first"), second("holder member second") {
        throw 'h';
    }
};

struct Inspector {
    ~Inspector() {
        SAYF("inspector before inner %d", std::uncaught_exceptions());
        try {
            throw 1;
        } catch (const int) {
            SAYF("inspector in inner catch %d", std::uncaught_exceptions());
        }
        SAYF("inspector after inner %d", std::uncaught_exceptions());
    }
};

struct TryBlock {
    Tracker member;
    TryBlock()
    try : member("function-try-block member") {
        throw 2;
    } catch (const int value) {
        SAYF("constructor's handler %d", value);
    }
};

void level_three() {
    Tracker tracker("level three");
    throw 3;
}

void level_two() {
    Tracker tracker("level two");
    level_three();
}

void level_one() {
    Tracker tracker("level one");
    level_two();
}

int make_value(const Tracker&) {
    throw 4;
}

} // namespace

int main() {
    try {
        level_one();
    } catch (const int value) {
        SAYF("caught %d", value);
    }
    try {
        Element elements[5] = {Element(0), Element(1), Element(2), Element(3), Element(4)};
        static_cast<void>(elements);
    } catch (const int value) {
        SAYF("array failed at %d", value);
    }
    try {
        Holder holder;
        static_cast<void>(holder);
    } catch (const char value) {
        SAYF("holder failed %c", value);
    }
    try {
        const int value = make_value(Tracker("temporary"));
        SAYF("unreached %d", value);
    } catch (const int value) {
        SAYF("temporary cleaned %d", value);
    }
    try {
        Inspector inspector;
        static_cast<void>(inspector);
        throw 5;
    } catch (const int value) {
        SAYF("after inspector %d uncaught %d", value, std::uncaught_exceptions());
    }
    try {
        TryBlock block;
        static_cast<void>(block);
    } catch (const int value) {
        SAYF("function-try-block rethrew %d", value);
    }
    return 0;
}
