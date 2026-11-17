// Scalars thrown and caught: by value, by reference, catch(...), handler
// order, a throw per loop iteration, and the uncaught count at rest.

#include "case_support.hpp"

namespace {

void throw_int(const int value) {
    throw value;
}

int add_on_throw(const int count) {
    int caught = 0;
    for (int index = 0; index < count; ++index) {
        try {
            if (index % 2 == 1) throw index;
            SAYF("no throw %d", index);
        } catch (const int value) {
            caught = caught + value;
        }
    }
    return caught;
}

} // namespace

int main() {
    try {
        throw_int(7);
    } catch (const int value) {
        SAYF("int %d", value);
    }
    try {
        throw 3.5;
    } catch (const int value) {
        SAYF("wrong int %d", value);
    } catch (const double value) {
        SAYF("double %.1f", value);
    }
    try {
        throw "text";
    } catch (const char* const text) {
        SAYF("string %s", text);
    }
    try {
        throw 'c';
    } catch (...) {
        say("catch-all");
    }
    try {
        throw 5L;
    } catch (long& value) {
        value = value + 1;
        SAYF("reference %ld", value);
    }
    try {
        throw 1u;
    } catch (const unsigned value) {
        SAYF("first handler %u", value);
    } catch (...) {
        say("second handler");
    }
    try {
        throw static_cast<short>(9);
    } catch (const int value) {
        SAYF("short as int, wrong %d", value);
    } catch (const short value) {
        SAYF("short %d", value);
    }
    const int caught = add_on_throw(7);
    SAYF("loop %d", caught);
    SAYF("uncaught at rest %d", std::uncaught_exceptions());
    return 0;
}
