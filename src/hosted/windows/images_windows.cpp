// Libraries, address lookup and image regions on Windows
// (hosted/image_backend.hpp). A1 platform-rest. Host facts (Microsoft's
// documentation as published; unconfirmed at a pin):
//   - LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
//     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS loads exactly the file an absolute
//     path names and finds its dependencies beside it and in the default
//     directories; with LOAD_LIBRARY_SEARCH_SYSTEM32 a bare file name is
//     looked up in System32 only, so the current directory and PATH are
//     never searched (the planting the old search order allowed).
//   - GetModuleHandleExW with no flags takes a reference to an already
//     loaded module, so FreeLibrary gives it back as for a loaded one;
//     with GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS it finds the module
//     holding an address, and with ..._UNCHANGED_REFCOUNT it takes no
//     reference.
//   - GetProcAddress returns null and sets the last error for a name the
//     module does not export.
//   - K32EnumProcessModules and K32GetModuleInformation (PSAPI_VERSION 2:
//     kernel32, no psapi.dll) list the process's modules and give a
//     module's base and SizeOfImage; the image is mapped over
//     [base, base + SizeOfImage) (unconfirmed for discardable sections,
//     which pe_image's bounds make harmless: a section must lie inside
//     the view to be returned).
// Address lookup does not use DbgHelp, which allocates and is not
// thread-safe: the nearest export is found by walking the module's export
// directory (core/pe_image.hpp). describe allocates nothing, since
// write_stack_trace calls it on the way to a fail-fast report.

#if !defined(PSAPI_VERSION)
#define PSAPI_VERSION 2
#endif

#include "hosted/image_backend.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/pe_image.hpp"
#include "core/wtf8.hpp"

#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

namespace stxlib::image_backend {

namespace {

ForeignCode last_error_code(const DWORD error) noexcept {
    const ForeignCode result = ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error));
    return result;
}

HMODULE module_of(const uptr handle) noexcept {
    const HMODULE result = reinterpret_cast<HMODULE>(handle);
    return result;
}

// The name in UTF-16, NUL-terminated, '/' made '\' when it is a path.
bool wide_name(const char* const name, const bool is_path, Growable<wchar_t>& wide) noexcept {
    wide.clear();
    if (!wtf8::to_utf16(name, std::strlen(name), wide)) return false;
    if (is_path) {
        for (usize index = 0; index < wide.count(); ++index) {
            if (wide[index] == L'/') wide[index] = L'\\';
        }
    }
    const bool result = wide.push(L'\0');
    return result;
}

// The image's mapping, from the module's base and SizeOfImage.
bool image_view(const HMODULE module, View<Byte>& image) noexcept {
    MODULEINFO information{};
    if (K32GetModuleInformation(GetCurrentProcess(), module, &information, sizeof information) == 0) return false;
    if (information.lpBaseOfDll == nullptr || information.SizeOfImage == 0) return false;
    image = View<Byte>(static_cast<const Byte*>(information.lpBaseOfDll), static_cast<usize>(information.SizeOfImage));
    return true;
}

// Appends one code point as WTF-8 to room[at, ...) if it fits whole.
bool put_code_point(const u32 code_point, const MutableView<char> room, usize& at) noexcept {
    char bytes[4]{};
    usize length = 0;
    if (code_point < 0x80U) {
        bytes[0] = static_cast<char>(code_point);
        length = 1;
    } else if (code_point < 0x800U) {
        bytes[0] = static_cast<char>(0xc0U | (code_point >> 6));
        bytes[1] = static_cast<char>(0x80U | (code_point & 0x3fU));
        length = 2;
    } else if (code_point < 0x10000U) {
        bytes[0] = static_cast<char>(0xe0U | (code_point >> 12));
        bytes[1] = static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU));
        bytes[2] = static_cast<char>(0x80U | (code_point & 0x3fU));
        length = 3;
    } else {
        bytes[0] = static_cast<char>(0xf0U | (code_point >> 18));
        bytes[1] = static_cast<char>(0x80U | ((code_point >> 12) & 0x3fU));
        bytes[2] = static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU));
        bytes[3] = static_cast<char>(0x80U | (code_point & 0x3fU));
        length = 4;
    }
    if (at > room.count() || room.count() - at < length) return false;
    for (usize index = 0; index < length; ++index) room[at + index] = bytes[index];
    at = at + length;
    return true;
}

// UTF-16 units into the room as WTF-8 (an unpaired surrogate as its
// three-byte form, D13 a), '\' as '/', cut at a whole code point.
View<char> narrow_into(const wchar_t* const units, const usize count, const MutableView<char> room,
                       bool& truncated) noexcept {
    usize at = 0;
    usize index = 0;
    truncated = false;
    while (index < count) {
        u32 code_point = static_cast<u16>(units[index]);
        usize used = 1;
        if (code_point >= 0xd800U && code_point <= 0xdbffU && index + 1 < count) {
            const u32 low = static_cast<u16>(units[index + 1]);
            if (low >= 0xdc00U && low <= 0xdfffU) {
                code_point = 0x10000U + ((code_point - 0xd800U) << 10) + (low - 0xdc00U);
                used = 2;
            }
        }
        if (code_point == static_cast<u32>('\\')) code_point = static_cast<u32>('/');
        if (!put_code_point(code_point, room, at)) {
            truncated = true;
            break;
        }
        index = index + used;
    }
    const View<char> result(room.data(), at);
    return result;
}

// Visits the query's section in one module; false when the visitor stops.
bool visit_module(const HMODULE module, const View<char> name, const ImageRegionVisitor visitor,
                  void* const context) noexcept {
    View<Byte> image{};
    if (!image_view(module, image)) return true;
    pe_image::Headers headers{};
    if (!pe_image::read_headers(image, headers)) return true;
    pe_image::Section section{};
    if (!pe_image::find_section(image, headers, name, section)) return true;
    ImageRegion region{};
    region.bytes = View<Byte>(image.data() + section.rva, static_cast<usize>(section.size));
    region.form = ImageRegionForm::named_section;
    region.alignment = 4;
    region.image_base = reinterpret_cast<uptr>(image.data());
    const bool result = visitor(region, context);
    return result;
}

} // namespace

OperationStatus open_library(const char* const name, const LoadMode mode, uptr& handle) noexcept {
    handle = 0;
    const Operation operation =
        mode == LoadMode::loaded_only ? Operation("stxlib.library.find_loaded") : Operation("stxlib.library.open");
    Growable<wchar_t> wide;
    if (!wide_name(name, mode == LoadMode::path, wide)) {
        return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    }
    HMODULE module = nullptr;
    if (mode == LoadMode::loaded_only) {
        if (GetModuleHandleExW(0, wide.data(), &module) == 0) module = nullptr;
    } else {
        const DWORD flags = mode == LoadMode::path ? (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)
                                                   : LOAD_LIBRARY_SEARCH_SYSTEM32;
        module = LoadLibraryExW(wide.data(), nullptr, flags);
    }
    if (module == nullptr) {
        const DWORD error = GetLastError();
        return host_failure::failed(ErrorKind::not_found, operation, HostCode::none, last_error_code(error));
    }
    handle = reinterpret_cast<uptr>(module);
    return OperationStatus::success();
}

OperationStatus close_library(const uptr handle) noexcept {
    if (FreeLibrary(module_of(handle)) == 0) {
        const DWORD error = GetLastError();
        return host_failure::failed(ErrorKind::foreign_failure, "stxlib.library.close", HostCode::none,
                                    last_error_code(error));
    }
    return OperationStatus::success();
}

OperationStatus find_symbol(const uptr handle, const char* const name, uptr& address) noexcept {
    address = 0;
    const FARPROC found = GetProcAddress(module_of(handle), name);
    if (found == nullptr) {
        const DWORD error = GetLastError();
        return host_failure::failed(ErrorKind::not_found, "stxlib.library.symbol", HostCode::symbol_not_found,
                                    last_error_code(error));
    }
    address = reinterpret_cast<uptr>(found);
    return OperationStatus::success();
}

OperationStatus describe(const uptr address, const MutableView<char> image_name_room, const MutableView<char> symbol_name_room,
                AddressDescription& description) noexcept {
    description = AddressDescription{};
    const Operation operation = "stxlib.images.describe_address";
    HMODULE module = nullptr;
    const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    if (GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module) == 0 || module == nullptr) {
        return host_failure::failed(ErrorKind::not_found, operation, HostCode::no_image);
    }
    description.image_base = reinterpret_cast<uptr>(module);
    // The name, in a fixed buffer: a longer one is reported cut.
    wchar_t name[1024]{};
    const DWORD length = GetModuleFileNameW(module, name, 1024);
    if (length > 0) {
        const wchar_t* units = name;
        usize count = length < 1024 ? static_cast<usize>(length) : 1023;
        if (count >= 4 && units[0] == L'\\' && units[1] == L'\\' && units[2] == L'?' && units[3] == L'\\') {
            units = units + 4;
            count = count - 4;
        }
        description.image_name = narrow_into(units, count, image_name_room, description.image_name_truncated);
        if (length >= 1024) description.image_name_truncated = true;
    }
    View<Byte> image{};
    pe_image::Headers headers{};
    if (!image_view(module, image) || !pe_image::read_headers(image, headers)) return OperationStatus::success();
    const uptr base = reinterpret_cast<uptr>(image.data());
    if (address < base || address - base >= image.count()) return OperationStatus::success();
    pe_image::Export found{};
    if (!pe_image::nearest_export(image, headers, static_cast<u32>(address - base), found)) return OperationStatus::success();
    const char* const symbol = reinterpret_cast<const char*>(image.data() + found.name_rva);
    description.symbol_name = copy_into(symbol, found.name_count, symbol_name_room, description.symbol_name_truncated);
    description.symbol_address = base + found.rva;
    return OperationStatus::success();
}

OperationStatus visit_regions(const ImageRegionQuery& query, const bool containing, const uptr address,
                     const ImageRegionVisitor visitor, void* const context) noexcept {
    const View<char> name(query.pe_section.data(), query.pe_section.count());
    if (containing) {
        HMODULE module = nullptr;
        // A reference is held while the visitor runs.
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(address), &module) == 0 ||
            module == nullptr) {
            return host_failure::failed(ErrorKind::not_found, "stxlib.images.visit_regions_containing",
                                        HostCode::no_image);
        }
        static_cast<void>(visit_module(module, name, visitor, context));
        static_cast<void>(FreeLibrary(module));
        return OperationStatus::success();
    }
    const Operation operation = "stxlib.images.visit_regions";
    Growable<HMODULE> modules;
    DWORD needed = 0;
    // The list can grow between the calls; it is read again until it fits.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const usize room = modules.count();
        const DWORD room_bytes = static_cast<DWORD>(room * sizeof(HMODULE));
        if (K32EnumProcessModules(GetCurrentProcess(), modules.data(), room_bytes, &needed) == 0) {
            const DWORD error = GetLastError();
            return host_failure::failed(ErrorKind::foreign_failure, operation, HostCode::none, last_error_code(error));
        }
        if (needed <= room_bytes) break;
        if (!modules.resize(static_cast<usize>(needed) / sizeof(HMODULE) + 16)) {
            return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
        }
    }
    const usize listed = static_cast<usize>(needed) / sizeof(HMODULE);
    const usize count = listed < modules.count() ? listed : modules.count();
    for (usize index = 0; index < count; ++index) {
        // The listed handle holds no reference; one is taken through an
        // address inside the module, and a module unloaded meanwhile is
        // skipped.
        HMODULE held = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(modules[index]), &held) ==
                0 ||
            held != modules[index]) {
            if (held != nullptr) static_cast<void>(FreeLibrary(held));
            continue;
        }
        const bool more = visit_module(held, name, visitor, context);
        static_cast<void>(FreeLibrary(held));
        if (!more) break;
    }
    return OperationStatus::success();
}

} // namespace stxlib::image_backend
