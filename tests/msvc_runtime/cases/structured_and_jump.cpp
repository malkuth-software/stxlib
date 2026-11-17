// Structured exception handling and setjmp/longjmp through the runtime's
// __C_specific_handler, _setjmp and longjmp: filters that accept, decline
// and resume; __finally on an exception, on normal exit and on longjmp;
// longjmp across frames with C++ destructors; longjmp(buffer, 0).

#include "case_support.hpp"

#include <setjmp.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

constexpr DWORD first_code = 0xE0000001;
constexpr DWORD second_code = 0xE0000002;

int filter_for(const DWORD code, const DWORD wanted, const char* const name) {
    SAYF("filter %s sees %08lx", name, static_cast<unsigned long>(code));
    return code == wanted ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

int resume_filter(const DWORD code) {
    SAYF("resume filter sees %08lx", static_cast<unsigned long>(code));
    return EXCEPTION_CONTINUE_EXECUTION;
}

void raise_code(const DWORD code, const DWORD flags) {
    RaiseException(code, flags, 0, nullptr);
}

void accepted() {
    __try {
        raise_code(first_code, EXCEPTION_NONCONTINUABLE);
        say("unreached");
    } __except (filter_for(_exception_code(), first_code, "accepting")) {
        SAYF("handler got %08lx", static_cast<unsigned long>(_exception_code()));
    }
}

void nested_filters() {
    __try {
        __try {
            raise_code(second_code, EXCEPTION_NONCONTINUABLE);
        } __except (filter_for(_exception_code(), first_code, "inner")) {
            say("inner handler, wrong");
        }
    } __except (filter_for(_exception_code(), second_code, "outer")) {
        say("outer handler");
    }
}

void resumed() {
    __try {
        raise_code(first_code, 0);
        say("resumed after the raise");
    } __except (resume_filter(_exception_code())) {
        say("resume handler, wrong");
    }
}

void finally_on_exception() {
    __try {
        __try {
            raise_code(first_code, EXCEPTION_NONCONTINUABLE);
        } __finally {
            SAYF("finally abnormal=%d", _abnormal_termination() ? 1 : 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        say("handler after finally");
    }
}

void finally_on_normal_exit() {
    __try {
        say("body");
    } __finally {
        SAYF("finally abnormal=%d", _abnormal_termination() ? 1 : 0);
    }
}

jmp_buf jump_target;

void jump_from_deep(const int value) {
    Tracker deep("deep");
    longjmp(jump_target, value);
}

void jump_through_finally(const int value) {
    __try {
        jump_from_deep(value);
    } __finally {
        SAYF("finally on longjmp abnormal=%d", _abnormal_termination() ? 1 : 0);
    }
}

void jumps() {
    volatile int round = 0;
    const int first = setjmp(jump_target);
    SAYF("setjmp returned %d in round %d", first, static_cast<int>(round));
    round = round + 1;
    if (round == 1) jump_from_deep(5);
    if (round == 2) jump_through_finally(7);
    if (round == 3) jump_from_deep(0);
}

} // namespace

int main() {
    accepted();
    nested_filters();
    resumed();
    finally_on_exception();
    finally_on_normal_exit();
    jumps();
    return 0;
}
