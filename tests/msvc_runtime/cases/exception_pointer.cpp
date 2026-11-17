// std::exception_ptr: current_exception inside and outside a catch,
// rethrow_exception after the catch has ended, copies and comparison,
// make_exception_ptr, nested_exception, and that captured objects are
// destroyed when the last pointer goes.

#include "case_support.hpp"

#include <exception>
#include <stdexcept>

namespace {

int live = 0;

struct Payload {
    int value;
    explicit Payload(const int payload_value) : value(payload_value) {
        live = live + 1;
    }
    Payload(const Payload& other) : value(other.value) {
        live = live + 1;
    }
    ~Payload() {
        live = live - 1;
    }
};

std::exception_ptr capture(const int value) {
    try {
        throw Payload(value);
    } catch (...) {
        std::exception_ptr captured = std::current_exception();
        return captured;
    }
}

void report_nested(const std::exception& outer) {
    SAYF("outer %s", outer.what());
    try {
        std::rethrow_if_nested(outer);
    } catch (const std::exception& inner) {
        SAYF("nested %s", inner.what());
    }
}

} // namespace

int main() {
    const std::exception_ptr none = std::current_exception();
    SAYF("outside a catch %d", none ? 1 : 0);
    std::exception_ptr first = capture(1);
    SAYF("captured %d live %d", first ? 1 : 0, live);
    std::exception_ptr second = first;
    SAYF("copies compare %d", first == second ? 1 : 0);
    try {
        std::rethrow_exception(first);
    } catch (const Payload& caught) {
        SAYF("rethrown %d live %d", caught.value, live);
    }
    try {
        std::rethrow_exception(second);
    } catch (Payload& caught) {
        caught.value = 99;
    }
    try {
        std::rethrow_exception(first);
    } catch (const Payload& caught) {
        SAYF("captured object unchanged by a rethrown copy %d", caught.value);
    }
    first = nullptr;
    SAYF("one pointer left live %d", live);
    second = nullptr;
    SAYF("none left live %d", live);
    std::exception_ptr made = std::make_exception_ptr(Payload(2));
    try {
        std::rethrow_exception(made);
    } catch (const Payload& caught) {
        SAYF("made %d", caught.value);
    }
    std::exception_ptr other = std::make_exception_ptr(std::runtime_error("made error"));
    SAYF("different pointers compare %d", made == other ? 1 : 0);
    try {
        std::rethrow_exception(other);
    } catch (const std::exception& caught) {
        SAYF("made standard %s", caught.what());
    }
    made = nullptr;
    try {
        try {
            throw std::runtime_error("inner");
        } catch (...) {
            std::throw_with_nested(std::logic_error("outer"));
        }
    } catch (const std::exception& outer) {
        report_nested(outer);
    }
    try {
        throw 7;
    } catch (...) {
        const std::exception_ptr scalar = std::current_exception();
        try {
            std::rethrow_exception(scalar);
        } catch (const int value) {
            SAYF("scalar %d uncaught %d", value, std::uncaught_exceptions());
        }
    }
    SAYF("live at end %d", live);
    return 0;
}
