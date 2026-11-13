// Known folders on Windows (hosted/directories_backend.hpp), through
// SHGetKnownFolderPath. A1 platform-rest. Host facts (Microsoft's
// documentation as published; unconfirmed at a pin):
//   - SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path) returns the
//     folder's path in a buffer the caller frees with CoTaskMemFree, even
//     on failure (where the pointer is null). The buffer is the shell's
//     allocation, freed by its own contract, never by STXlib's allocator.
//   - FOLDERID_Profile is the user's profile, FOLDERID_RoamingAppData the
//     roaming application data, FOLDERID_LocalAppData the local one.
// The folder GUIDs are defined in this file (initguid.h before
// knownfolders.h), so no uuid library is needed; CoTaskMemFree comes from
// ole32, which CMakeLists.txt links on Windows.

#include "hosted/directories_backend.hpp"

#include "core/host_failure.hpp"
#include "core/wtf8.hpp"

#include <cwchar>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <initguid.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

namespace stxlib::directories_backend {

namespace {

constexpr Operation operation = "stxlib.files.known_directory";

} // namespace

Status user_database_home(Growable<char>& text) noexcept {
    text.clear();
    return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
}

Status known_folder(const KnownDirectory which, Growable<char>& text) noexcept {
    text.clear();
    const GUID* folder = nullptr;
    switch (which) {
    case KnownDirectory::home:
        folder = &FOLDERID_Profile;
        break;
    case KnownDirectory::user_configuration:
    case KnownDirectory::user_data:
        folder = &FOLDERID_RoamingAppData;
        break;
    case KnownDirectory::user_cache:
        folder = &FOLDERID_LocalAppData;
        break;
    }
    if (folder == nullptr) return host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    PWSTR path = nullptr;
    const HRESULT status = SHGetKnownFolderPath(*folder, KF_FLAG_DEFAULT, nullptr, &path);
    if (FAILED(status)) {
        CoTaskMemFree(path);
        const ErrorKind kind = which == KnownDirectory::home ? ErrorKind::not_found : ErrorKind::foreign_failure;
        const HostCode code = which == KnownDirectory::home ? HostCode::no_home : HostCode::none;
        return host_failure::failed(kind, operation, code,
                                    ForeignCode::of(ForeignFamily::windows_hresult, static_cast<i64>(status)));
    }
    const usize count = path != nullptr ? static_cast<usize>(std::wcslen(path)) : 0;
    // WTF-8, so an unpaired surrogate in the folder's name is carried (D13 a).
    const bool converted = wtf8::from_utf16(path, count, text);
    CoTaskMemFree(path);
    if (!converted) {
        text.clear();
        return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    }
    for (usize index = 0; index < text.count(); ++index) {
        if (text[index] == '\\') text[index] = '/';
    }
    return Status::success();
}

} // namespace stxlib::directories_backend
