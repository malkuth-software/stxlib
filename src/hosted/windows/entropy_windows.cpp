// Host entropy on Windows (host.hpp, fill_entropy), ported from
// stx_auxlib's src/hosted/entropy_windows.cpp at b6e330a: BCryptGenRandom
// with the system-preferred generator. Fails closed: no fallback source.

#include "hosted/host.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace stxlib::host {

bool fill_entropy(Byte* const bytes, const usize count) noexcept {
    // BCryptGenRandom takes a ULONG count; larger requests go in pieces.
    constexpr usize largest_request = usize{1} << 30;
    usize filled = 0;
    while (filled < count) {
        const usize remaining = count - filled;
        const usize request = remaining < largest_request ? remaining : largest_request;
        const NTSTATUS status = BCryptGenRandom(nullptr, static_cast<PUCHAR>(bytes + filled),
                                                static_cast<ULONG>(request), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            std::memset(bytes, 0, count);
            return false;
        }
        filled = filled + request;
    }
    return true;
}

} // namespace stxlib::host
