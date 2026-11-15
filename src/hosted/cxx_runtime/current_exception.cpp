// The C++ runtime's exception interface (host.hpp): the current exception's
// type. Every phase I target
// uses the Itanium C++ ABI's exception runtime: libc++abi on macOS, Linux
// and, with LLVM MinGW, Windows (FORWARD_PROGRAMME section 4: libc++
// throughout). abi::__cxa_current_exception_type is declared in
// <cxxabi.h> and returns the type_info of the exception being handled, or
// null (libcxxabi/src/cxa_exception.cpp, LLVM llvmorg-22.1.8, lines 602-613,
// read through a web view; to reconfirm at the toolchain pin, and again if
// the pin moves to LLVM 23). FreeBSD's base runtime is libcxxrt, whose
// __cxa_current_exception_type returns the exceptionType of the top of the
// thread's caught list, or null when it is empty, with no check for a
// foreign exception (contrib/libcxxrt/exception.cc, read at FreeBSD
// fb9033d2876d and on stable/15 through the GitHub mirror, line numbers not
// taken). Its __cxa_begin_catch pushes an exception only when its handler
// count is 0, a rethrow negates the count, and __cxa_end_catch pops only
// when the count returns to 0, so the type stays available in a gate's
// handler after a nested `throw;` is caught. FreeBSD 15.2 Debug still
// recorded no type (2026-11-15); the gates now take a standard exception's
// type from its own RTTI (exception_shadow.cpp, finish_record), and this
// function is left for the other exceptions. type_info::name() is the mangled name under
// the Itanium ABI; it is demangled by the reader, never here, since
// __cxa_demangle allocates.

#include "hosted/host.hpp"

#include <cxxabi.h>
#include <cstdlib>
#include <cstring>
#include <typeinfo>

namespace stxlib::host {

// Left out of a build without the exception recorder (STXLIB_EXCEPTIONS
// OFF, CMakeLists.txt), whose objects refer to no part of the exception
// runtime (tools/check_no_exceptions.py).
#if !defined(STXLIB_NO_EXCEPTION_RECORDER)
const char* current_exception_type_name() noexcept {
    const std::type_info* const type = abi::__cxa_current_exception_type();
    if (type == nullptr) return nullptr;
    const char* const result = (*type).name();
    return result;
}
#endif

usize demangle_type_name(const char* const mangled, char* const output, const usize capacity) noexcept {
    if (mangled == nullptr || output == nullptr || capacity == 0) return 0;
    int status = 0;
    // A null buffer makes the runtime allocate the result with malloc
    // (Itanium C++ ABI 3.4, __cxa_demangle); a caller's fixed buffer cannot
    // be passed, since the runtime may realloc it.
    char* const demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
    if (demangled == nullptr) return 0;
    if (status != 0) {
        std::free(demangled);
        return 0;
    }
    const usize length = std::strlen(demangled);
    const usize kept = length < capacity ? length : capacity;
    std::memcpy(output, demangled, kept);
    std::free(demangled);
    return kept;
}

} // namespace stxlib::host
