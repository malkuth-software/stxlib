// typeid and dynamic_cast: dynamic types through base pointers, typeid of a
// null pointer (std::bad_typeid), downcasts, cross casts, failures, a
// failed reference cast (std::bad_cast), dynamic_cast<void*>, virtual
// bases, ambiguous and private bases, and casts made while a virtual base
// is under construction (where a vtordisp adjusts the complete object).

#include "case_support.hpp"

#include <typeinfo>

namespace {

struct Root {
    int root = 1;
    virtual ~Root() {}
};

struct Middle : Root {
    int middle = 2;
};

struct Leaf : Middle {
    int leaf = 3;
};

struct Other {
    int other = 4;
    virtual ~Other() {}
};

// Leaf-like class with a second polymorphic base: cross casts.
struct Both : Middle, Other {
    int both = 5;
};

struct Shared {
    int shared = 6;
    virtual ~Shared() {}
};

struct SharedLeft : virtual Shared {
    int left = 7;
};

struct SharedRight : virtual Shared {
    int right = 8;
};

struct SharedBoth : SharedLeft, SharedRight {
    int both = 9;
};

struct TwiceLeft : Root {};
struct TwiceRight : Root {};
struct Twice : TwiceLeft, TwiceRight {};

struct Hidden : private Root {
    Root* as_root() {
        return this;
    }
};

// A vtordisp arises when a class with a virtual base overrides one of the
// base's virtual functions and has its own constructor; inspect() then runs
// with the virtual base displaced from where the complete class puts it.
struct Probe {
    virtual void touch() {}
    virtual ~Probe() {}
};

void inspect(Probe* const probe, const char* const when) {
    void* const complete = dynamic_cast<void*>(probe);
    SAYF("%s: void* is probe %d, typeid probe %d", when, complete == static_cast<void*>(probe) ? 1 : 0,
         typeid(*probe) == typeid(Probe) ? 1 : 0);
}

struct Builder : virtual Probe {
    int data = 10;
    Builder() {
        inspect(this, "in Builder()");
    }
    void touch() override {}
};

struct Outer : Builder {
    int extra[4] = {1, 2, 3, 4};
    Outer() {
        inspect(this, "in Outer()");
    }
};

bool same_name(const std::type_info& left, const std::type_info& right) {
    return left == right;
}

} // namespace

int main() {
    Leaf leaf;
    Root* const root = &leaf;
    SAYF("typeid leaf %d", same_name(typeid(*root), typeid(Leaf)) ? 1 : 0);
    SAYF("typeid not middle %d", same_name(typeid(*root), typeid(Middle)) ? 0 : 1);
    Root* const nothing = nullptr;
    try {
        const std::type_info& type = typeid(*nothing);
        SAYF("typeid of null, wrong %d", same_name(type, typeid(Root)) ? 1 : 0);
    } catch (const std::bad_typeid&) {
        say("typeid of null threw bad_typeid");
    }
    Middle* const middle = dynamic_cast<Middle*>(root);
    SAYF("downcast to middle %d %d", middle != nullptr ? 1 : 0, middle != nullptr ? (*middle).middle : 0);
    Leaf* const back = dynamic_cast<Leaf*>(root);
    SAYF("downcast to leaf %d", back == &leaf ? 1 : 0);
    Middle plain;
    Root* const plain_root = &plain;
    SAYF("failed downcast null %d", dynamic_cast<Leaf*>(plain_root) == nullptr ? 1 : 0);
    try {
        Leaf& wrong = dynamic_cast<Leaf&>(*plain_root);
        SAYF("reference cast, wrong %d", wrong.leaf);
    } catch (const std::bad_cast&) {
        say("reference cast threw bad_cast");
    }
    Both both;
    Root* const both_root = &both;
    Other* const crossed = dynamic_cast<Other*>(both_root);
    SAYF("cross cast %d %d", crossed == static_cast<Other*>(&both) ? 1 : 0, crossed != nullptr ? (*crossed).other : 0);
    Other* const other_side = &both;
    Middle* const crossed_back = dynamic_cast<Middle*>(other_side);
    SAYF("cross cast back %d", crossed_back == static_cast<Middle*>(&both) ? 1 : 0);
    SAYF("void* of other side %d", dynamic_cast<void*>(other_side) == static_cast<void*>(&both) ? 1 : 0);
    Other lone;
    SAYF("cross cast fails %d", dynamic_cast<Root*>(&lone) == nullptr ? 1 : 0);
    SharedBoth shared_both;
    Shared* const shared = &shared_both;
    SharedRight* const right = dynamic_cast<SharedRight*>(shared);
    SAYF("virtual base downcast %d %d", right == static_cast<SharedRight*>(&shared_both) ? 1 : 0,
         right != nullptr ? (*right).right : 0);
    SharedBoth* const whole = dynamic_cast<SharedBoth*>(shared);
    SAYF("virtual base to complete %d", whole == &shared_both ? 1 : 0);
    SAYF("virtual base void* %d", dynamic_cast<void*>(shared) == static_cast<void*>(&shared_both) ? 1 : 0);
    SAYF("typeid via virtual base %d", same_name(typeid(*shared), typeid(SharedBoth)) ? 1 : 0);
    Twice twice;
    TwiceLeft* const twice_left = &twice;
    Root* const one_root = twice_left;
    SAYF("downcast from one of two roots %d", dynamic_cast<Twice*>(one_root) == &twice ? 1 : 0);
    SAYF("cross from root to right %d", dynamic_cast<TwiceRight*>(one_root) == static_cast<TwiceRight*>(&twice) ? 1 : 0);
    Other* const no_root = nullptr;
    SAYF("null stays null %d", dynamic_cast<Root*>(no_root) == nullptr ? 1 : 0);
    Hidden hidden;
    Root* const hidden_root = hidden.as_root();
    SAYF("private base downcast fails %d", dynamic_cast<Hidden*>(hidden_root) == nullptr ? 1 : 0);
    Outer outer;
    inspect(&outer, "after construction");
    SAYF("outer data %d", outer.data);
    return 0;
}
