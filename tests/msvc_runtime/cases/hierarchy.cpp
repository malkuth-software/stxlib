// Class exceptions: by value (copy, slicing), by reference, by base, by
// pointer and base pointer, through multiple inheritance (a displaced base)
// and virtual inheritance (a vbtable lookup), private and ambiguous bases
// that must not match, pointer qualifiers, nullptr, and that every thrown
// object is destroyed exactly once.

#include "case_support.hpp"

namespace {

int live = 0;
int copies = 0;

struct Base {
    int id;
    explicit Base(const int base_id) : id(base_id) {
        live = live + 1;
    }
    Base(const Base& other) : id(other.id) {
        live = live + 1;
        copies = copies + 1;
    }
    virtual ~Base() {
        live = live - 1;
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

struct Left {
    int left = 11;
    virtual ~Left() {}
};

// Base sits after Left, so catching Multi as Base needs a displacement.
struct Multi : Left, Base {
    explicit Multi(const int multi_id) : Base(multi_id) {}
    const char* name() const override {
        return "Multi";
    }
};

struct VirtualBase {
    int value;
    explicit VirtualBase(const int base_value) : value(base_value) {
        live = live + 1;
    }
    VirtualBase(const VirtualBase& other) : value(other.value) {
        live = live + 1;
        copies = copies + 1;
    }
    virtual ~VirtualBase() {
        live = live - 1;
    }
};

struct VirtualLeft : virtual VirtualBase {
    int pad_left = 1;
    VirtualLeft() : VirtualBase(0) {}
};

struct VirtualRight : virtual VirtualBase {
    int pad_right = 2;
    VirtualRight() : VirtualBase(0) {}
};

struct Diamond : VirtualLeft, VirtualRight {
    explicit Diamond(const int diamond_value) : VirtualBase(diamond_value) {}
};

struct Private : private Base {
    Private() : Base(5) {}
};

struct AmbiguousLeft : Base {
    AmbiguousLeft() : Base(6) {}
};
struct AmbiguousRight : Base {
    AmbiguousRight() : Base(7) {}
};
struct Ambiguous : AmbiguousLeft, AmbiguousRight {};

} // namespace

int main() {
    try {
        throw Derived(1);
    } catch (const Base& caught) {
        SAYF("by base reference %s %d", caught.name(), caught.id);
    }
    copies = 0;
    try {
        throw Derived(2);
    } catch (const Base caught) {
        SAYF("by base value %s %d copies %d", caught.name(), caught.id, copies);
    }
    try {
        throw Derived(3);
    } catch (Derived caught) {
        SAYF("by exact value %s %d", caught.name(), caught.id);
    }
    try {
        throw Multi(4);
    } catch (Base& caught) {
        SAYF("multi as base %s %d", caught.name(), caught.id);
    }
    try {
        throw Multi(5);
    } catch (Left& caught) {
        SAYF("multi as left %d", caught.left);
    }
    try {
        throw Diamond(6);
    } catch (VirtualBase& caught) {
        SAYF("diamond as virtual base %d", caught.value);
    }
    try {
        throw Diamond(7);
    } catch (VirtualRight& caught) {
        SAYF("diamond as right %d %d", caught.pad_right, caught.value);
    }
    copies = 0;
    try {
        throw Diamond(8);
    } catch (VirtualBase caught) {
        SAYF("diamond virtual base by value %d copies %d", caught.value, copies);
    }
    try {
        throw new Derived(9);
    } catch (Base* const caught) {
        SAYF("pointer as base pointer %s %d", (*caught).name(), (*caught).id);
        delete caught;
    }
    try {
        throw new Multi(10);
    } catch (Base* const caught) {
        SAYF("multi pointer as base pointer %s %d", (*caught).name(), (*caught).id);
        delete caught;
    }
    try {
        throw new Diamond(11);
    } catch (VirtualBase* const caught) {
        SAYF("diamond pointer as virtual base pointer %d", (*caught).value);
        delete caught;
    }
    Derived local(12);
    try {
        throw &local;
    } catch (void* const caught) {
        SAYF("pointer as void* same %d", caught == static_cast<void*>(&local) ? 1 : 0);
    }
    try {
        throw Private();
    } catch (Base&) {
        say("private base matched, wrong");
    } catch (...) {
        say("private base not matched");
    }
    try {
        throw Ambiguous();
    } catch (Base&) {
        say("ambiguous base matched, wrong");
    } catch (AmbiguousRight& caught) {
        SAYF("ambiguous as right %d", caught.id);
    }
    static int number = 13;
    const int* const constant = &number;
    try {
        throw constant;
    } catch (int*) {
        say("const pointer as pointer, wrong");
    } catch (const int* const caught) {
        SAYF("const pointer as const pointer %d", *caught);
    }
    try {
        throw &number;
    } catch (const int* const caught) {
        SAYF("pointer as const pointer %d", *caught);
    }
    volatile int* const changing = &number;
    try {
        throw changing;
    } catch (const int*) {
        say("volatile pointer as const pointer, wrong");
    } catch (const volatile int* const caught) {
        SAYF("volatile pointer as const volatile pointer %d", *caught);
    }
    try {
        throw nullptr;
    } catch (int*) {
        say("nullptr as int*");
    } catch (void* const caught) {
        SAYF("nullptr as void* %d", caught == nullptr ? 1 : 0);
    } catch (...) {
        say("nullptr as catch-all");
    }
    try {
        throw Derived(14);
    } catch (Derived& caught) {
        caught.id = 15;
        try {
            throw;
        } catch (const Base& again) {
            SAYF("modified by reference then rethrown %d", again.id);
        }
    }
    SAYF("live after %d", live - 1);
    return 0;
}
