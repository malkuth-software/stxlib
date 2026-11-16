// Desktop integration on Windows (hosted/desktop_backend.hpp):
// ShellExecuteW to open, the system explorer.exe /select to reveal, GetModuleFileNameW
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

OperationStatus exhausted(const Operation operation) noexcept {
    const OperationStatus result = host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    return result;
}

} // namespace

OperationStatus open_target(const TextView target, const bool is_link) noexcept {
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
    return OperationStatus::success();
}

OperationStatus reveal(const Path& path) noexcept {
    const Operation operation = "stxlib.desktop.reveal";
    const PathText text = path.text();
    Growable<char> selection;
    if (!selection.append("/select,", 8)) return exhausted(operation);
    for (usize index = 0; index < text.count(); ++index) {
        const char character = text.data()[index];
        if (!selection.push(character == '/' ? '\\' : character)) return exhausted(operation);
    }
    // "/select," and the path's own WTF-8 bytes with '\' for '/' (still
    // well-formed WTF-8: one ASCII byte for another).
    // The shared Windows directory's explorer.exe by its absolute name, so no
    // search order applies (A-stxlib-hosted-01-F2). GetSystemWindowsDirectoryW
    // gives the shared directory even under Terminal Services, where
    // GetWindowsDirectoryW may name a private one ("GetSystemWindowsDirectoryW
    // function", Microsoft Learn); it returns the length without the NUL, or
    // the size needed when the buffer is too small, or 0 on failure.
    Growable<wchar_t> directory;
    UINT capacity = MAX_PATH;
    UINT length = 0;
    for (;;) {
        if (!directory.resize(capacity)) return exhausted(operation);
        length = GetSystemWindowsDirectoryW(directory.data(), capacity);
        if (length == 0) {
            return host_failure::failed(
                ErrorKind::io_failure, operation, HostCode::none,
                ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError())));
        }
        if (length < capacity) break;
        if (capacity >= 32768) return host_failure::failed(ErrorKind::io_failure, operation, HostCode::too_long);
        capacity = length + 1;
    }
    Growable<char> program;
    if (!wtf8::from_utf16(directory.data(), length, program)) return exhausted(operation);
    if (program.count() == 0 || program[program.count() - 1] != '\\') {
        if (!program.push('\\')) return exhausted(operation);
    }
    if (!program.append("explorer.exe", 12)) return exhausted(operation);
    const PathText arguments[] = {detail::PathTextAccess::adopt_validated(selection.data(), selection.count())};
    const ChildRequest request{.program = detail::PathTextAccess::adopt_validated(program.data(), program.count()),
                               .arguments = View<PathText>(arguments),
                               .streams = ChildStreams::discard,
                               .detached = true};
    const OperationResult<Child> started = Child::start(request);
    if (started.failed()) return OperationStatus::failure(*started.error());
    return OperationStatus::success();
}

OperationStatus executable_name(Growable<char>& text) noexcept {
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
    // A "\\?\" prefix (a long-path executable) is removed. A UNC name
    // ("\\server\share\..." or "\\?\UNC\...") is refused as unsupported,
    // as the file backend refuses it: a Path has no UNC form, and stripping
    // the prefix would re-root "\\?\UNC\server\share" as the local
    // "UNC/server/share" (A-stxlib-core-01-F1).
    const wchar_t* units = buffer.data();
    usize count = length;
    if (count >= 8 && units[0] == L'\\' && units[1] == L'\\' && units[2] == L'?' && units[3] == L'\\' &&
        (units[4] == L'U' || units[4] == L'u') && (units[5] == L'N' || units[5] == L'n') &&
        (units[6] == L'C' || units[6] == L'c') && units[7] == L'\\') {
        return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    }
    if (count >= 4 && units[0] == L'\\' && units[1] == L'\\' && units[2] == L'?' && units[3] == L'\\') {
        units = units + 4;
        count = count - 4;
    } else if (count >= 2 && units[0] == L'\\' && units[1] == L'\\') {
        return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    }
    // WTF-8, so an unpaired surrogate in the name is carried (D13 a).
    if (!wtf8::from_utf16(units, count, text)) return exhausted(operation);
    for (usize index = 0; index < text.count(); ++index) {
        if (text[index] == '\\') text[index] = '/';
    }
    return OperationStatus::success();
}

} // namespace stxlib::desktop_backend
