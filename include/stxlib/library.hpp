#pragma once

// Loading shared libraries and finding their symbols
// (Capability::dynamic_libraries). Replaces GUI.Forms' direct
// LoadLibraryW, GetModuleHandleW and GetProcAddress (abi
// drawing_platform_windows.cpp at 491156eac) and the dlopen family on
// POSIX hosts (A0 inventory, section 2: "own-image sections and module
// lookup"). A1 platform-rest; the design note
// (planning/stages/A1-platform-rest.md) records the choices below.
//
// Host calls: dlopen, dlsym and dlclose on macOS, Linux (glibc, musl) and
// FreeBSD (libexec/rtld-elf/rtld.c at the pin: dlopen 3964, dlsym 4322,
// dlclose 3887); LoadLibraryExW, GetModuleHandleExW, GetProcAddress and
// FreeLibrary on Windows.
//
// Ownership: a Library holds one reference to the loaded image, taken by
// open, open_system or find_loaded, and gives it back when it is closed or
// destroyed. The host unloads the image when its last reference goes, so a
// SymbolAddress stays valid only while some Library (or the host itself)
// keeps the image loaded. Nothing here frees memory the loader owns.
//
// Search. open takes an absolute Path, and the host loads exactly that
// file (and its dependencies by the host's rules). open_system takes a
// file name alone (no separator) and searches as the host does for its
// own libraries: on POSIX hosts the dynamic linker's default search
// (which honours LD_LIBRARY_PATH and its equivalents); on Windows the
// System32 directory only (LOAD_LIBRARY_SEARCH_SYSTEM32), so a library
// planted in the working or the program's directory is never taken for a
// system one. find_loaded takes the same file name and succeeds only when
// the host already has the image loaded (RTLD_NOLOAD; GetModuleHandleExW),
// loading nothing.
//
// Symbols are named by their linker name (a C function's name, or a C++
// name mangled; on Apple hosts without the leading underscore the object
// file adds). The name is ASCII without NUL: GetProcAddress takes an ANSI
// name, and every export name the hosts produce for us is ASCII.
//
// Failures: invalid_argument with HostCode::invalid_name for a refused name
// or a relative Path; not_found (with the host's message cut into the
// record, where it is UTF-8) when the library cannot be found or loaded,
// and not_found with HostCode::symbol_not_found for a missing symbol;
// disposed with HostCode::closed_handle on a closed Library; out_of_memory;
// foreign_failure with the host's code otherwise; unsupported where
// Capability::dynamic_libraries is false.
//
// Thread safety: the hosts' loaders serialize themselves. dlerror's text is
// per thread on glibc, musl and macOS (unconfirmed at a pin) and on
// FreeBSD once libthr is loaded (libexec/rtld-elf/rtld_lock.c:465-468 at
// the pin); it is read right after the failed call on the same thread.

#include "stxlib/path.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

// A symbol's address in a loaded image. Valid while the image stays loaded
// (see above).
class SymbolAddress final {
public:
    constexpr SymbolAddress() noexcept = default;
    explicit constexpr SymbolAddress(const uptr symbol_address) noexcept : address_(symbol_address) {}

    [[nodiscard]] constexpr uptr address() const noexcept { return address_; }

    // The symbol as a function of type Function. Take the type from the
    // function's declaration (house style, section 2: never retyped by
    // hand), for example `symbol.function<decltype(GetDpiForWindow)>()`.
    // Calling through the wrong type is undefined behaviour.
    template <typename Function>
    [[nodiscard]] Function* function() const noexcept {
        static_assert(__is_function(Function), "SymbolAddress::function takes a function type");
        Function* const result = reinterpret_cast<Function*>(address_);
        return result;
    }

    // The symbol as data, read-only. Writing through it needs the
    // library's own contract.
    [[nodiscard]] const void* data() const noexcept {
        const void* const result = reinterpret_cast<const void*>(address_);
        return result;
    }

private:
    uptr address_{0};
};

class Library final {
public:
    // Loads the library at `path`, which must be absolute.
    [[nodiscard]] static OperationResult<Library> open(const Path& path) noexcept;
    // Loads a host library by file name ("libX11.so.6", "gdi32.dll").
    [[nodiscard]] static OperationResult<Library> open_system(TextView file_name) noexcept;
    // A reference to a library the host has already loaded, by file name;
    // not_found when it has not.
    [[nodiscard]] static OperationResult<Library> find_loaded(TextView file_name) noexcept;

    Library() noexcept = default;
    // Gives the reference back, discarding a failure (call close() where it
    // matters).
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;
    Library(Library&& other) noexcept : handle_(other.handle_) { other.handle_ = 0; }
    // Closes this library first, discarding a failure.
    Library& operator=(Library&& other) noexcept;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != 0; }

    // The address of the symbol `name`.
    [[nodiscard]] OperationResult<SymbolAddress> symbol(TextView name) const noexcept;

    // Gives the reference back; the Library is closed even when the host
    // reports a failure.
    [[nodiscard]] OperationStatus close() noexcept;

private:
    explicit Library(const uptr host_handle) noexcept : handle_(host_handle) {}

    // The host's handle (dlopen's result, or the HMODULE); 0 when closed.
    uptr handle_{0};
};

} // namespace stxlib
