// Native traces on Windows (host.hpp), through RtlCaptureStackBackTrace
// (declared in <winnt.h> through <windows.h>), as GUI.Forms' fail-fast
// report already uses it (GUI.Forms 491156eac, src/core/failure/
// fail_fast.cpp:59). At most 32 frames are taken (STXlib's
// shadow_trace_capacity), well inside the function's USHORT result. Its
// behaviour
// while an exception's cleanup phase is running the caller is unconfirmed
// at a pinned source, and the A1 tests exercise it.

#include "hosted/host.hpp"

#include <windows.h>

namespace stxlib::host {

usize capture_return_addresses(uptr* const addresses, const usize capacity) noexcept {
    if (addresses == nullptr || capacity == 0) return 0;
    constexpr usize most = 32;
    const usize wanted = capacity < most ? capacity : most;
    void* frames[most]{};
    const USHORT captured = RtlCaptureStackBackTrace(1, static_cast<ULONG>(wanted), frames, nullptr);
    const usize count = static_cast<usize>(captured);
    for (usize index = 0; index < count; ++index) addresses[index] = reinterpret_cast<uptr>(frames[index]);
    return count;
}

} // namespace stxlib::host
