// Host entropy on POSIX hosts (host.hpp, fill_entropy), ported from
// stx_auxlib's src/hosted/entropy_posix.cpp at b6e330a. Apple hosts and the
// BSDs use arc4random_buf, a kernel-seeded generator in user space that
// cannot fail and avoids a system call per request (it is what
// std::random_device uses there). glibc (2.25 and later) and musl use
// getentropy, at most 256 bytes per call, which reads the kernel's
// generator. Fails closed: no fallback source.

#include "hosted/host.hpp"

#include <cstring>

#include <stdlib.h>
#include <unistd.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__)
#include <sys/random.h>
#endif

namespace stxlib::host {

bool fill_entropy(Byte* const bytes, const usize count) noexcept {
#if defined(__APPLE__) || defined(__FreeBSD__)
    ::arc4random_buf(bytes, count);
    return true;
#else
    constexpr usize largest_request = 256;
    usize filled = 0;
    while (filled < count) {
        const usize remaining = count - filled;
        const usize request = remaining < largest_request ? remaining : largest_request;
        if (::getentropy(bytes + filled, request) != 0) {
            std::memset(bytes, 0, count);
            return false;
        }
        filled = filled + request;
    }
    return true;
#endif
}

} // namespace stxlib::host
