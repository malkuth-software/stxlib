// The C++ runtime's exception interface (host.hpp): the current exception's
// type. Every phase I target
// uses the Itanium C++ ABI's exception runtime: libc++abi on macOS, Linux
// and, with LLVM MinGW, Windows (FORWARD_PROGRAMME section 4: libc++
// throughout). abi::__cxa_current_exception_type is declared in
// <cxxabi.h> and returns the type_info of the exception being handled, or
// null (libcxxabi/src/cxa_exception.cpp, LLVM llvmorg-22.1.8, lines 602-613,
// read through a web view; to reconfirm at the toolchain pin, and again if
// the pin moves to LLVM 23). type_info::name() is the mangled name under
// the Itanium ABI; it is demangled by the reader, never here, since
// __cxa_demangle allocates.

#include "hosted/host.hpp"

#include <cxxabi.h>
#include <typeinfo>

namespace stxlib::host {

const char* current_exception_type_name() noexcept {
    const std::type_info* const type = abi::__cxa_current_exception_type();
    if (type == nullptr) return nullptr;
    const char* const result = (*type).name();
    return result;
}

} // namespace stxlib::host
