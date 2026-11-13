// Desktop integration on Windows (hosted/desktop_backend.hpp):
// ShellExecuteW to open, explorer.exe /select to reveal, GetModuleFileNameW
// for the executable. Ported from stx_auxlib's
// src/hosted/desktop_windows.cpp at b6e330a.

#include "hosted/desktop_backend.hpp"

#include "core/host_failure.hpp"
#include "core/wtf8.hpp"
#include "hosted/windows/utf16.hpp"
#include "stxlib/children.hpp"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace stxlib::desktop_backend {

namespace {

Status exhausted(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    return result;
}

} // namespace

Status open_target(const TextView target, const bool is_link) noexcept {
    const Operation operation = "stxlib.desktop.open";
    Growable<wchar_t> wide;
    if (!utf16::append_wide(target.data(), target.count(), wide)) return exhausted(operation);
    // A path takes Windows' separators; a link is passed as written.
    if (!is_link) {
        for (usize index = 0; index < wide.count(); ++index) {
            if (wide[index] == L'/') wide[index] = L'\\';
        }
    }
    if (!utf16::terminate(wide)) return exhausted(operation);
    // ShellExecuteW reports success as a value above 32.
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", wide.data(), nullptr, nullptr, SW_SHOWNORMAL);
    const intptr_t code = reinterpret_cast<intptr_t>(opened);
    if (code <= 32) {
        return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none,
                                    ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(code)));
    }
    return Status::success();
}

Status reveal(const Path& path) noexcept {
    const Operation operation = "stxlib.desktop.reveal";
    const TextView text = path.text().unicode();
    Growable<char> selection;
    if (!selection.append("/select,", 8)) return exhausted(operation);
    for (usize index = 0; index < text.count(); ++index) {
        const char character = text.data()[index];
        if (!selection.push(character == '/' ? '\\' : character)) return exhausted(operation);
    }
    // "/select," and the path's own UTF-8 bytes with '\' for '/'.
    const TextView arguments[] = {detail::TextViewAccess::adopt_validated(selection.data(), selection.count())};
    const ChildRequest request{.program = "explorer.exe",
                               .arguments = View<TextView>(arguments),
                               .streams = ChildStreams::discard,
                               .detached = true};
    const Result<Child> started = Child::start(request);
    if (started.failed()) return Status::failure(*started.error());
    return Status::success();
}

Status executable_name(Growable<char>& text) noexcept {
    const Operation operation = "stxlib.desktop.current_executable";
    text.clear();
    Growable<wchar_t> buffer;
    DWORD capacity = 260;
    DWORD length = 0;
    for (;;) {
        if (!buffer.resize(capacity)) return exhausted(operation);
        length = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) {
            return host_failure::failed(
                ErrorKind::io_failure, operation, HostCode::none,
                ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError())));
        }
        if (length < capacity) break;
        if (capacity >= 32768) return host_failure::failed(ErrorKind::io_failure, operation, HostCode::too_long);
        capacity = capacity * 2;
    }
    // A "\\?\" prefix (a long-path executable) is removed; UNC is not a Path.
    const wchar_t* units = buffer.data();
    usize count = length;
    if (count >= 4 && units[0] == L'\\' && units[1] == L'\\' && units[2] == L'?' && units[3] == L'\\') {
        units = units + 4;
        count = count - 4;
    }
    // WTF-8, so an unpaired surrogate in the name is carried (D13 a).
    if (!wtf8::from_utf16(units, count, text)) return exhausted(operation);
    for (usize index = 0; index < text.count(); ++index) {
        if (text[index] == '\\') text[index] = '/';
    }
    return Status::success();
}

} // namespace stxlib::desktop_backend
