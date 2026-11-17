// `throw;`: to the caller, inside a try nested in the same catch (the outer
// catch's object must survive), from a function the catch calls, keeping
// the dynamic type, against `throw copy;`, and with no active exception
// (terminate), each thrown object destroyed exactly once.

#include "case_support.hpp"

namespace {

int live = 0;
int destroyed = 0;

struct Base {
    int id;
    explicit Base(const int base_id) : id(base_id) {
        live = live + 1;
    }
    Base(const Base& other) : id(other.id) {
        live = live + 1;
    }
    virtual ~Base() {
        live = live - 1;
        destroyed = destroyed + 1;
    }
    virtual const char* name() const {
        return "Base";
    }
};

struct Derived : Base {
    explicit Derived(const int derived_id) : Base(derived_id) {}
    const char* name() const override {
        return "Derived";
    }
};

void rethrow_to_caller() {
    try {
        throw Derived(1);
    } catch (const Base&) {
        Tracker tracker("rethrowing catch");
        throw;
    }
}

void rethrower() {
    throw;
}

void rethrow_from_called_function() {
    try {
        throw Derived(3);
    } catch (...) {
        rethrower();
    }
}

} // namespace

int main() {
    try {
        rethrow_to_caller();
    } catch (const Derived& caught) {
        SAYF("caller got %s %d destroyed so far %d", caught.name(), caught.id, destroyed);
    }
    SAYF("after first destroyed %d live %d", destroyed, live);
    destroyed = 0;
    try {
        throw Derived(2);
    } catch (Base& outer) {
        try {
            throw;
        } catch (Derived& inner) {
            SAYF("inner rethrow same object %d", &inner == &outer ? 1 : 0);
        }
        SAYF("outer object alive %s %d destroyed %d", outer.name(), outer.id, destroyed);
    }
    SAYF("after nested rethrow destroyed %d live %d", destroyed, live);
    try {
        rethrow_from_called_function();
    } catch (const Base& caught) {
        SAYF("rethrown from called function %s %d", caught.name(), caught.id);
    }
    try {
        try {
            throw Derived(4);
        } catch (const Base& caught) {
            throw caught;
        }
    } catch (const Derived&) {
        say("throw copy kept the dynamic type, wrong");
    } catch (const Base& sliced) {
        SAYF("throw copy sliced %s %d", sliced.name(), sliced.id);
    }
    try {
        try {
            try {
                throw 5;
            } catch (const int) {
                throw;
            }
        } catch (const int first) {
            SAYF("rethrow level one %d uncaught %d", first, std::uncaught_exceptions());
            throw;
        }
    } catch (const int second) {
        SAYF("rethrow level two %d", second);
    }
    SAYF("live at end %d", live);
    std::set_terminate(case_terminate_handler);
    say("rethrow with nothing active");
    try {
        rethrower();
    } catch (...) {
        say("caught nothing, wrong");
    }
    return 0;
}
