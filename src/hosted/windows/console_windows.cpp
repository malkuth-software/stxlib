// Windows standard error and thread yield (host.hpp). Standard error is
// written as bytes with WriteFile, so a redirected stream receives the
// UTF-8 report unchanged; a console shows it in its own code page (the
// console module of the stx_auxlib port converts to UTF-16 for consoles).

#include "hosted/host.hpp"

#include <windows.h>

namespace stxlib::host {

void write_standard_error(const char* const bytes, const usize count) noexcept {
    const HANDLE stream = GetStdHandle(STD_ERROR_HANDLE);
    if (stream == nullptr || stream == INVALID_HANDLE_VALUE) return;
    usize written = 0;
    while (written < count) {
        const usize remaining = count - written;
        const DWORD chunk = remaining > 0x40000000u ? static_cast<DWORD>(0x40000000u) : static_cast<DWORD>(remaining);
        DWORD transferred = 0;
        const BOOL succeeded = WriteFile(stream, bytes + written, chunk, &transferred, nullptr);
        if (succeeded == FALSE || transferred == 0) return;
        written += static_cast<usize>(transferred);
    }
}

void yield_thread() noexcept { static_cast<void>(SwitchToThread()); }

} // namespace stxlib::host
