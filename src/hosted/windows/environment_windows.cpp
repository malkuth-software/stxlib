// The Windows environment backend (hosted/environment_backend.hpp), over
// the wide Win32 environment block, which child processes inherit. The C
// runtime's getenv reads a separate ANSI copy and is not used.
// core/environment.cpp serializes the calls. Ported from stx_auxlib's
// src/hosted/environment_windows.cpp at b6e330a.

#include "hosted/environment_backend.hpp"
#include "hosted/windows/utf16.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib::environment_backend {

namespace {

// The NUL-terminated UTF-8 `text` as NUL-terminated UTF-16.
bool wide_of(const char* const text, Growable<wchar_t>& output) noexcept {
    const bool result = utf16::append_wide(text, std::strlen(text), output) && utf16::terminate(output);
    return result;
}

Report last_error() noexcept {
    const Report result{Outcome::host_error,
                        ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError()))};
    return result;
}

} // namespace

Report read(const char* const name, const usize limit, Growable<char>& value) noexcept {
    Growable<wchar_t> wide_name;
    if (!wide_of(name, wide_name)) return Report{Outcome::out_of_memory, ForeignCode::none()};
    Growable<wchar_t> buffer;
    DWORD size = 256;
    usize length = 0;
    for (;;) {
        if (!buffer.reserve(size)) return Report{Outcome::out_of_memory, ForeignCode::none()};
        SetLastError(ERROR_SUCCESS);
        const DWORD found = GetEnvironmentVariableW(wide_name.data(), buffer.data(), size);
        if (found == 0) {
            // Zero is also the length of a set but empty value.
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return Report{Outcome::not_set, ForeignCode::none()};
            length = 0;
            break;
        }
        if (found < size) {
            length = found;
            break;
        }
        // Too small: `found` is the size needed, terminator included.
        if (found > 32768) return Report{Outcome::too_long, ForeignCode::none()};
        size = found;
    }
    const utf16::Narrowed narrowed = utf16::append_narrow(buffer.data(), length, value);
    if (narrowed == utf16::Narrowed::out_of_memory) return Report{Outcome::out_of_memory, ForeignCode::none()};
    if (narrowed != utf16::Narrowed::ok) return Report{Outcome::invalid_text, ForeignCode::none()};
    if (value.count() > limit) {
        value.clear();
        return Report{Outcome::too_long, ForeignCode::none()};
    }
    return Report{};
}

Report set(const char* const name, const char* const value, const usize value_count) noexcept {
    Growable<wchar_t> wide_name;
    Growable<wchar_t> wide_value;
    if (!wide_of(name, wide_name) || !utf16::append_wide(value, value_count, wide_value) ||
        !utf16::terminate(wide_value)) {
        return Report{Outcome::out_of_memory, ForeignCode::none()};
    }
    if (!SetEnvironmentVariableW(wide_name.data(), wide_value.data())) return last_error();
    return Report{};
}

Report remove(const char* const name) noexcept {
    Growable<wchar_t> wide_name;
    if (!wide_of(name, wide_name)) return Report{Outcome::out_of_memory, ForeignCode::none()};
    if (!SetEnvironmentVariableW(wide_name.data(), nullptr) && GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        return last_error();
    }
    return Report{};
}

} // namespace stxlib::environment_backend
