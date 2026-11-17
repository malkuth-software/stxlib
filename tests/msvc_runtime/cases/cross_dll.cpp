// Exceptions across a DLL boundary: thrown in the library and caught in the
// program, thrown in the program and caught in the library, rethrown from
// a library catch, uncaught counts seen from both sides, and
// exception_ptr values made on one side and rethrown on the other. The
// types match by decorated name, each image carrying its own descriptors.

#include "cross_dll_shared.hpp"

#include "case_support.hpp"

namespace {

struct CountOnUnwind {
    ~CountOnUnwind() {
        SAYF("program frame: library sees uncaught=%d", library_uncaught());
    }
};

void throw_from_program(const int value) {
    throw Payload(value);
}

void throw_through_library(const int value) {
    CountOnUnwind counter;
    static_cast<void>(counter);
    library_throw(value);
}

} // namespace

int main() {
    try {
        throw_through_library(7);
    } catch (const Payload& caught) {
        SAYF("program caught library's payload %d doubled %d uncaught=%d", caught.value, caught.doubled(),
             std::uncaught_exceptions());
    }
    library_catch(throw_from_program, 11);
    try {
        library_rethrow(13);
    } catch (const LibraryOnly& caught) {
        SAYF("program caught the rethrow %d", caught.code);
    }
    const std::exception_ptr from_library = library_capture(17);
    try {
        std::rethrow_exception(from_library);
    } catch (const Payload& caught) {
        SAYF("program rethrew library's pointer %d", caught.value);
    }
    std::exception_ptr from_program;
    try {
        throw Payload(19);
    } catch (...) {
        from_program = std::current_exception();
    }
    library_rethrow_pointer(from_program);
    SAYF("uncaught at the end %d %d", std::uncaught_exceptions(), library_uncaught());
    return 0;
}
