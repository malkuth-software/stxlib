// A structured (non-C++) exception passing through C++ frames compiled for
// synchronous exceptions: catch(...) must not take it, no destructor runs
// before the unhandled-exception filter (nothing unwinds without a
// handler), and a C++ exception raised with RaiseException's C++ code but a
// foreign magic number is not a C++ exception either.

#include "case_support.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

LONG WINAPI last_filter(EXCEPTION_POINTERS* const pointers) {
    const EXCEPTION_RECORD& record = *(*pointers).ExceptionRecord;
    SAYF("unhandled filter code %08lX", static_cast<unsigned long>(record.ExceptionCode));
    // A continuable raise resumes after RaiseException; the last one ends
    // the case.
    if ((record.ExceptionFlags & EXCEPTION_NONCONTINUABLE) == 0) return EXCEPTION_CONTINUE_EXECUTION;
    ExitProcess(0);
}

void raise_structured(const DWORD code) {
    Tracker tracker("frame with structured raise");
    RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

} // namespace

int main() {
    // Set after the program's own startup, so both runtimes report here.
    SetUnhandledExceptionFilter(last_filter);
    try {
        const ULONG_PTR parameters[3] = {0x12345678u, 0, 0};
        RaiseException(0xE06D7363u, 0, 3, parameters);
        say("continued after foreign magic");
    } catch (...) {
        say("foreign magic caught, wrong");
    }
    try {
        raise_structured(0xE0001234u);
    } catch (...) {
        say("structured caught, wrong");
    }
    say("returned, wrong");
    return 0;
}
