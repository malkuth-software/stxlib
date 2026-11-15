// Native traces on Emscripten (host.hpp, capture_return_addresses): none.
// Wasm code cannot read its own call stack: there is no frame-pointer
// chain in linear memory to walk, and _Unwind_Backtrace has no wasm
// implementation STXlib relies on. Emscripten offers
// emscripten_get_callstack (system/include/emscripten/emscripten.h:159-171
// at emscripten 6.0.13), which writes a JavaScript stack as text, not
// return addresses, so it does not fit this interface:
// Capability::stack_trace is false (core/capabilities.cpp), and
// fail-fast reports carry their shadow frames only.

#include "hosted/host.hpp"

namespace stxlib::host {

usize capture_return_addresses(uptr* const addresses, const usize capacity) noexcept {
    static_cast<void>(addresses);
    static_cast<void>(capacity);
    return 0;
}

usize capture_report_addresses(uptr* const addresses, const usize capacity) noexcept {
    static_cast<void>(addresses);
    static_cast<void>(capacity);
    return 0;
}

} // namespace stxlib::host
