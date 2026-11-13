// Native traces on macOS, glibc and musl (host.hpp), through the Itanium
// unwinder's _Unwind_Backtrace (<unwind.h>): LLVM libunwind or libgcc's
// unwinder, whichever the toolchain links, on all three. glibc's
// backtrace() is not used because musl has none.
//
// Called from a scope's destructor while the unwinder is running that
// destructor during an exception's cleanup phase. _Unwind_Backtrace starts
// its own walk from the current context and does not touch the running
// unwind's state; that it is safe there is unconfirmed at a pinned source
// (to read: LLVM libunwind src/UnwindLevel1-gcc-ext.c at the toolchain pin),
// and the A1 tests exercise it.

#include "hosted/host.hpp"

#include <unwind.h>

namespace stxlib::host {

namespace {

struct TraceSink final {
    uptr* addresses{nullptr};
    usize capacity{0};
    usize count{0};
    usize skip{0};
};

_Unwind_Reason_Code record_frame(struct _Unwind_Context* const context, void* const argument) {
    TraceSink& sink = *static_cast<TraceSink*>(argument);
    if (sink.skip > 0) {
        sink.skip = sink.skip - 1;
        return _URC_NO_REASON;
    }
    if (sink.count == sink.capacity) return _URC_END_OF_STACK;
    const uptr address = static_cast<uptr>(_Unwind_GetIP(context));
    if (address == 0) return _URC_END_OF_STACK;
    sink.addresses[sink.count] = address;
    sink.count = sink.count + 1;
    return _URC_NO_REASON;
}

} // namespace

usize capture_return_addresses(uptr* const addresses, const usize capacity) noexcept {
    if (addresses == nullptr || capacity == 0) return 0;
    // Skips this function's own frame.
    TraceSink sink{addresses, capacity, 0, 1};
    static_cast<void>(_Unwind_Backtrace(record_frame, &sink));
    const usize result = sink.count;
    return result;
}

} // namespace stxlib::host
