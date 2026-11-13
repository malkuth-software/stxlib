// POSIX standard error and thread yield (host.hpp), shared by macOS, glibc
// and musl. The write loop is ported from stx_auxlib's console backend
// (stx_auxlib 08c8132, src/hosted/console_posix.cpp, write()).

#include "hosted/host.hpp"

#include <cerrno>
#include <cstddef>

#include <sched.h>
#include <unistd.h>

namespace stxlib::host {

void write_standard_error(const char* const bytes, const usize count) noexcept {
    usize written = 0;
    while (written < count) {
        const ssize_t result = ::write(STDERR_FILENO, bytes + written, count - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (result == 0) return;
        written += static_cast<usize>(result);
    }
}

void yield_thread() noexcept { static_cast<void>(::sched_yield()); }

} // namespace stxlib::host
