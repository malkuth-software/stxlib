// The Windows console backend (hosted/console_backend.hpp), and the host's
// standard-error writer, thread yield and brief sleep (host.hpp). Ported
// from stx_auxlib's src/hosted/console_windows.cpp at b6e330a.
//
// A console handle is written and read as UTF-16 through WriteConsoleW and
// ReadConsoleW, so the console's code page does not matter; a redirected
// handle (file or pipe) carries UTF-8 bytes through WriteFile and ReadFile.
// Change from stx_auxlib: the high surrogate carried between console reads
// is in stxlib_state (the caller holds the console input lock).
//
// write_standard_error (the fault path) writes bytes with WriteFile, so it
// allocates nothing; a console shows them in its own code page.

#include "hosted/console_backend.hpp"
#include "hosted/host.hpp"
#include "hosted/windows/utf16.hpp"
#include "state/state.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace host {

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

void hold_cancellation() noexcept {} // Windows has no thread cancellation

int cancellation_off() noexcept { return 0; } // Windows has no thread cancellation

void restore_cancellation(const int previous) noexcept { static_cast<void>(previous); }

void yield_thread() noexcept { static_cast<void>(SwitchToThread()); }

void sleep_briefly() noexcept { Sleep(1); }

} // namespace host

namespace console_backend {

namespace {

// One WriteConsoleW, WriteFile or read call moves at most this much.
constexpr DWORD largest_transfer = 1u << 16;

HANDLE handle_for(const ConsoleStream stream) noexcept {
    const DWORD which = stream == ConsoleStream::error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    const HANDLE result = GetStdHandle(which);
    return result;
}

bool usable(const HANDLE handle) noexcept {
    const bool result = handle != nullptr && handle != INVALID_HANDLE_VALUE;
    return result;
}

bool is_console(const HANDLE handle) noexcept {
    DWORD mode = 0;
    const bool result = GetConsoleMode(handle, &mode) != 0;
    return result;
}

Report report_last_error() noexcept {
    const DWORD error = GetLastError();
    const ForeignCode foreign = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error));
    if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE) {
        return Report{Outcome::closed, foreign};
    }
    return Report{Outcome::io_error, foreign};
}

Report write_console_text(const HANDLE handle, const char* const bytes, const usize count) noexcept {
    Growable<wchar_t> wide;
    if (!utf16::append_wide(bytes, count, wide)) return Report{Outcome::out_of_memory, ForeignCode::none()};
    usize written = 0;
    while (written < wide.count()) {
        const usize remaining = wide.count() - written;
        const DWORD request = remaining < largest_transfer ? static_cast<DWORD>(remaining) : largest_transfer;
        DWORD transferred = 0;
        if (!WriteConsoleW(handle, wide.data() + written, request, &transferred, nullptr)) return report_last_error();
        if (transferred == 0) return Report{Outcome::io_error, ForeignCode::none()};
        written += transferred;
    }
    return Report{};
}

Report write_bytes(const HANDLE handle, const char* const bytes, const usize count) noexcept {
    usize written = 0;
    while (written < count) {
        const usize remaining = count - written;
        const DWORD request = remaining < largest_transfer ? static_cast<DWORD>(remaining) : largest_transfer;
        DWORD transferred = 0;
        if (!WriteFile(handle, bytes + written, request, &transferred, nullptr)) return report_last_error();
        if (transferred == 0) return Report{Outcome::io_error, ForeignCode::none()};
        written += transferred;
    }
    return Report{};
}

} // namespace

Report write(const ConsoleStream stream, const char* const bytes, const usize count) noexcept {
    const HANDLE handle = handle_for(stream);
    if (!usable(handle)) return Report{Outcome::closed, ForeignCode::none()};
    if (is_console(handle)) return write_console_text(handle, bytes, count);
    const Report result = write_bytes(handle, bytes, count);
    return result;
}

Report write_line(const ConsoleStream stream, const char* const bytes, const usize count) noexcept {
    const Report first = write(stream, bytes, count);
    if (first.outcome != Outcome::ok) return first;
    const Report result = write(stream, "\n", 1);
    return result;
}

bool is_terminal(const ConsoleStream stream) noexcept {
    const HANDLE handle = handle_for(stream);
    const bool result = usable(handle) && is_console(handle);
    return result;
}

// Console input arrives as UTF-16 and is converted here; each UTF-16 unit
// becomes at most three UTF-8 bytes, so a read of capacity / 3 - 1 units
// (plus a carried surrogate) always fits. Ctrl+Z at the start of a read
// ends the input, as cmd does.
Report read(char* const buffer, const usize capacity, usize& transferred) noexcept {
    transferred = 0;
    const HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    if (!usable(handle)) return Report{Outcome::closed, ForeignCode::none()};
    if (!is_console(handle)) {
        const DWORD request = capacity < largest_transfer ? static_cast<DWORD>(capacity) : largest_transfer;
        DWORD count = 0;
        if (!ReadFile(handle, buffer, request, &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) return Report{};
            return report_last_error();
        }
        transferred = count;
        return Report{};
    }
    // core/console.cpp passes a 4096-byte chunk; a buffer below 6 bytes
    // could not hold one converted unit plus a carried surrogate.
    if (capacity < 6) return Report{Outcome::io_error, ForeignCode::none()};
    state::ProcessState& process = state::current_process();
    const usize units = capacity / 3 - 1;
    Growable<wchar_t> wide;
    if (!wide.reserve(units + 1)) return Report{Outcome::out_of_memory, ForeignCode::none()};
    usize total = 0;
    // Repeats only when a read delivered nothing but a high surrogate,
    // which must not look like the end of input.
    while (total == 0) {
        wide.clear();
        usize start = 0;
        if (process.console_pending_high_surrogate != 0) {
            if (!wide.push(static_cast<wchar_t>(process.console_pending_high_surrogate))) {
                return Report{Outcome::out_of_memory, ForeignCode::none()};
            }
            process.console_pending_high_surrogate = 0;
            start = 1;
        }
        DWORD count = 0;
        if (!ReadConsoleW(handle, wide.data() + start, static_cast<DWORD>(units), &count, nullptr)) {
            return report_last_error();
        }
        if (count == 0) return Report{};
        if (wide[start] == L'\x1a') return Report{};
        total = start + count;
        const u16 last = static_cast<u16>(wide[total - 1]);
        if (last >= 0xd800U && last <= 0xdbffU) {
            process.console_pending_high_surrogate = last;
            total = total - 1;
        }
    }
    Growable<char> narrow;
    const utf16::Narrowed narrowed = utf16::append_narrow(wide.data(), total, narrow);
    if (narrowed == utf16::Narrowed::out_of_memory) return Report{Outcome::out_of_memory, ForeignCode::none()};
    if (narrowed != utf16::Narrowed::ok) return Report{Outcome::invalid_text, ForeignCode::none()};
    std::memcpy(buffer, narrow.data(), narrow.count());
    transferred = narrow.count();
    return Report{};
}

} // namespace console_backend

} // namespace stxlib
