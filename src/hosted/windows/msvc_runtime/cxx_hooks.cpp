// The C++ library's exceptions the runtime throws: std::bad_cast (a failed
// reference dynamic_cast, [expr.dynamic.cast]p9), std::bad_typeid (typeid
// of a null pointer, [expr.typeid]p3), and a std::bad_alloc for an
// exception_ptr that could not be allocated ([propagation]p8).
//
// The one translation unit of the runtime compiled with exceptions and
// RTTI: it throws with the C++ library's own classes (libc++ built with
// _LIBCPP_NO_VCRUNTIME, which defines them), so a handler for those types
// matches by their decorated names. Everything else in the runtime is
// compiled without exceptions and stays independent of the C++ library.

#include "runtime_state.hpp"

#include <atomic>
#include <new>
#include <typeinfo>

namespace stxlib::msvc_runtime {

void raise_bad_cast() {
    throw std::bad_cast();
}

void raise_bad_typeid() {
    throw std::bad_typeid();
}

namespace {

// Static storage for the one std::bad_alloc, constructed on first use: the
// runtime runs no dynamic initialisers of its own. Never destroyed: process
// termination releases nothing.
alignas(std::bad_alloc) Byte bad_alloc_storage[sizeof(std::bad_alloc)];
constinit std::atomic<i32> bad_alloc_state{0};   // 0 none, 1 constructing, 2 ready

} // namespace

void* bad_alloc_object(const ThrowInfo** const info) noexcept {
    i32 expected = 0;
    if (bad_alloc_state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        new (bad_alloc_storage) std::bad_alloc();
        bad_alloc_state.store(2, std::memory_order_release);
    }
    while (bad_alloc_state.load(std::memory_order_acquire) != 2) {
        YieldProcessor();
    }
    std::bad_alloc* const object = std::launder(reinterpret_cast<std::bad_alloc*>(bad_alloc_storage));
    // clang's builtin for the ThrowInfo of an expression's type, as libc++'s
    // make_exception_ptr uses it on this ABI.
    *info = static_cast<const ThrowInfo*>(__GetExceptionInfo(*object));
    return object;
}

} // namespace stxlib::msvc_runtime
