// The DLL half of cross_dll: throws to its caller, catches what its
// caller's callback throws, rethrows across the boundary, and hands
// exception_ptr values both ways.

#define CROSS_DLL_LIBRARY
#include "cross_dll_shared.hpp"

#include "case_support.hpp"

void library_throw(const int value) {
    Tracker tracker("library frame");
    static_cast<void>(tracker);
    throw Payload(value);
}

void library_catch(const Callback callback, const int value) {
    try {
        callback(value);
    } catch (const Payload& caught) {
        SAYF("library caught program's payload %d doubled %d uncaught=%d", caught.value, caught.doubled(),
             std::uncaught_exceptions());
    }
}

void library_rethrow(const int value) {
    try {
        throw LibraryOnly{value};
    } catch (const LibraryOnly& caught) {
        SAYF("library caught its own %d, rethrowing", caught.code);
        throw;
    }
}

int library_uncaught() {
    return std::uncaught_exceptions();
}

std::exception_ptr library_capture(const int value) {
    try {
        throw Payload(value);
    } catch (...) {
        return std::current_exception();
    }
}

void library_rethrow_pointer(const std::exception_ptr pointer) {
    try {
        std::rethrow_exception(pointer);
    } catch (const Payload& caught) {
        SAYF("library rethrew program's pointer %d", caught.value);
    }
}
