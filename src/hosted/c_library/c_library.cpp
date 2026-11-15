// The C standard library's part of the interface modules (host.hpp):
// abnormal termination, stream flushing and atexit, the same calls on every
// family. The host allocator that was here (stx_auxlib's host backend)
// moved into stxlib_state with the rest of the allocator (layout 5,
// src/state/allocator.cpp), so every module shares one heap.

#include "hosted/host.hpp"

#include <cstdio>
#include <cstdlib>

namespace stxlib::host {

void end_process_abnormally() noexcept { std::abort(); }

void flush_c_stream(const bool error) noexcept { static_cast<void>(std::fflush(error ? stderr : stdout)); }

bool register_at_exit(void (*const handler)()) noexcept {
    const bool result = std::atexit(handler) == 0;
    return result;
}

} // namespace stxlib::host
