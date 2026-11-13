// The host's zone name on Windows (hosted/local_time_backend.hpp): the
// current dynamic time zone's key name (GetDynamicTimeZoneInformation's
// TimeZoneKeyName, for example "W. Europe Standard Time"), mapped to its
// IANA name for region "001" (the zone CLDR names for the key as a whole)
// by ICU's ucal_getTimeZoneIDForWindowsID, from the ICU that Windows 10
// version 1703 and later carry as System32\icu.dll. Replaces batch 2c's
// SystemTimeToTzSpecificLocalTimeEx (owner decision D12 = a): Windows'
// rules for the zone are not read, only its name.
//
// icu.dll is loaded from System32 on first use and stays loaded for the
// process's life; its handle is kept in stxlib_state (zone_mapping_module),
// so every module shares one load. A load that loses a race to publish its
// handle releases its own reference.
//
// ICU's declaration (unicode/ucal.h, ICU 52 and later; the C API Windows
// documents for icu.dll):
//     U_CAPI int32_t U_EXPORT2 ucal_getTimeZoneIDForWindowsID(
//         const UChar* winid, int32_t len, const char* region,
//         UChar* id, int32_t idCapacity, UErrorCode* status);
// UChar is a 16-bit UTF-16 unit (wchar_t's size and representation on
// Windows), UErrorCode an int-sized enumeration whose values above zero are
// failures (U_FAILURE), and U_EXPORT2 is __cdecl. No ICU header is part of
// the toolchain, so the type below restates that declaration by hand: an
// exception to house style section 2 (take a foreign entry point's type
// from its declaration), reported in proofs/REPORT.md.

#include "hosted/local_time_backend.hpp"

#include "core/host_failure.hpp"
#include "state/state.hpp"

#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib::local_time_backend {

namespace {

constexpr Operation operation = "stxlib.local_time.host_zone";

using WindowsZoneToIana = i32(__cdecl*)(const wchar_t* windows_id, i32 length, const char* region, wchar_t* id,
                                        i32 capacity, int* status);
static_assert(sizeof(wchar_t) == 2, "ICU's UChar is a 16-bit unit");
static_assert(sizeof(WindowsZoneToIana) == sizeof(FARPROC), "function pointers share one representation");

Status last_error_failure(const ErrorKind kind) noexcept {
    const Status result =
        host_failure::failed(kind, operation, HostCode::none,
                             ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(GetLastError())));
    return result;
}

// icu.dll's handle, loading it on first use; null when it cannot be loaded.
HMODULE icu_module() noexcept {
    state::ProcessState& process = state::current_process();
    void* const published = process.zone_mapping_module.load(std::memory_order_acquire);
    if (published != nullptr) return static_cast<HMODULE>(published);
    const HMODULE loaded = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (loaded == nullptr) return nullptr;
    void* expected = nullptr;
    if (process.zone_mapping_module.compare_exchange_strong(expected, static_cast<void*>(loaded),
                                                            std::memory_order_acq_rel)) {
        return loaded;
    }
    static_cast<void>(FreeLibrary(loaded));
    return static_cast<HMODULE>(expected);
}

} // namespace

Status system_zone_text(ZoneText& text) noexcept {
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    if (GetDynamicTimeZoneInformation(&zone) == TIME_ZONE_ID_INVALID) {
        return last_error_failure(ErrorKind::io_failure);
    }
    constexpr usize key_capacity = sizeof(zone.TimeZoneKeyName) / sizeof(zone.TimeZoneKeyName[0]);
    usize key_count = 0;
    while (key_count < key_capacity && zone.TimeZoneKeyName[key_count] != L'\0') ++key_count;
    if (key_count == 0) return host_failure::failed(ErrorKind::not_found, operation, HostCode::none);
    const HMODULE module = icu_module();
    if (module == nullptr) return last_error_failure(ErrorKind::unsupported);
    const FARPROC procedure = GetProcAddress(module, "ucal_getTimeZoneIDForWindowsID");
    if (procedure == nullptr) return last_error_failure(ErrorKind::unsupported);
    WindowsZoneToIana mapping = nullptr;
    std::memcpy(&mapping, &procedure, sizeof(mapping));
    // IANA names are at most 32 ASCII characters (zone_data.inc); 128
    // units leave room for any name ICU might give.
    wchar_t mapped[128]{};
    int status = 0;
    const i32 mapped_count =
        mapping(zone.TimeZoneKeyName, static_cast<i32>(key_count), "001", mapped, 128, &status);
    // ICU's status has no ForeignFamily of its own; the failure is named
    // without it.
    if (status > 0) return host_failure::failed(ErrorKind::foreign_failure, operation, HostCode::none);
    // A key ICU does not know maps to the empty name.
    if (mapped_count <= 0 || mapped_count >= 128) {
        return host_failure::failed(ErrorKind::not_found, operation, HostCode::none);
    }
    const usize count = static_cast<usize>(mapped_count);
    for (usize index = 0; index < count; ++index) {
        const wchar_t unit = mapped[index];
        // Database names are ASCII; anything else is no name of ours.
        if (unit < 0x21 || unit > 0x7e) return host_failure::failed(ErrorKind::not_found, operation, HostCode::none);
    }
    for (usize index = 0; index < count; ++index) text.bytes[index] = static_cast<char>(mapped[index]);
    text.count = count;
    return Status::success();
}

} // namespace stxlib::local_time_backend
