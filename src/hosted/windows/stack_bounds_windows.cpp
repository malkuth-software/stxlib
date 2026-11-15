// The calling thread's stack on Windows (hosted/stack_bounds_backend.hpp),
// compiled into stxlib_state: not asked. Windows' traces come from
// RtlCaptureStackBackTrace (trace_windows.cpp), which needs no bounds from
// STXlib, so the anchor's bounds stay unrecorded.

#include "hosted/stack_bounds_backend.hpp"

namespace stxlib::stack_bounds_backend {

bool query(uptr& low, uptr& high) noexcept {
    static_cast<void>(low);
    static_cast<void>(high);
    return false;
}

} // namespace stxlib::stack_bounds_backend
