// Libraries (include/stxlib/library.hpp): name checks and the Library
// object over the family's image module (hosted/image_backend.hpp).
// A1 platform-rest.

#include "stxlib/capabilities.hpp"
#include "stxlib/library.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/wtf8.hpp"
#include "hosted/image_backend.hpp"

namespace stxlib {

namespace {

// Bounds on what a caller may name: a file name fits every host's name
// limit (255 bytes on the POSIX filesystems and NTFS), and a symbol name
// fits the longest mangled names the C++ runtime produces for us with room
// to spare.
constexpr usize maximum_file_name_bytes = 255;
constexpr usize maximum_symbol_bytes = 4096;

Status refused(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_name);
    return result;
}

Status unsupported(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::unsupported, operation, HostCode::none);
    return result;
}

Status exhausted(const Operation operation) noexcept {
    const Status result = host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    return result;
}

// A file name: 1 to 255 bytes, no separator, no NUL, not "." or "..".
bool acceptable_file_name(const TextView name) noexcept {
    if (name.empty() || name.count() > maximum_file_name_bytes) return false;
    if (name == TextView(".") || name == TextView("..")) return false;
    for (usize index = 0; index < name.count(); ++index) {
        const char byte = name.data()[index];
        if (byte == '/' || byte == '\\' || byte == '\0') return false;
    }
    return true;
}

// A symbol name: 1 to 4,096 printable ASCII bytes (library.hpp).
bool acceptable_symbol(const TextView name) noexcept {
    if (name.empty() || name.count() > maximum_symbol_bytes) return false;
    for (usize index = 0; index < name.count(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(name.data()[index]);
        if (byte <= 0x20 || byte >= 0x7f) return false;
    }
    return true;
}

bool terminated(const TextView text, Growable<char>& output) noexcept {
    const bool result = output.append(text.data(), text.count()) && output.push('\0');
    return result;
}

// The backend's name for an absolute Path (image_backend.hpp): the POSIX
// bytes it spells, or its WTF-8 text on Windows, then a NUL. False with
// `representable` false when no POSIX name spells it.
bool native_path(const Path& path, Growable<char>& output, bool& representable) noexcept {
    representable = true;
    const PathText text = path.text();
#if defined(_WIN32)
    const bool result = output.append(text.data(), text.count()) && output.push('\0');
    return result;
#else
    if (path.drive() != 0) {
        representable = false;
        return false;
    }
    const wtf8::Converted converted = wtf8::unescape_posix(text.data(), text.count(), output);
    if (converted == wtf8::Converted::not_representable) {
        representable = false;
        return false;
    }
    if (converted != wtf8::Converted::ok) return false;
    const bool result = output.push('\0');
    return result;
#endif
}

Result<Library> failed_library(const Status& status) noexcept {
    const Result<Library> result = Result<Library>::failure(*status.error());
    return result;
}

} // namespace

Result<Library> Library::open(const Path& path) noexcept {
    const Operation operation = "stxlib.library.open";
    if (!capability_supported(Capability::dynamic_libraries)) return failed_library(unsupported(operation));
    if (!path.is_absolute()) return failed_library(refused(operation));
    Growable<char> name;
    bool representable = true;
    if (!native_path(path, name, representable)) {
        if (!representable) return failed_library(refused(operation));
        return failed_library(exhausted(operation));
    }
    uptr handle = 0;
    const Status opened = image_backend::open_library(name.data(), image_backend::LoadMode::path, handle);
    if (opened.failed()) return failed_library(opened);
    return Result<Library>::success(Library(handle));
}

Result<Library> Library::open_system(const TextView file_name) noexcept {
    const Operation operation = "stxlib.library.open_system";
    if (!capability_supported(Capability::dynamic_libraries)) return failed_library(unsupported(operation));
    if (!acceptable_file_name(file_name)) return failed_library(refused(operation));
    Growable<char> name;
    if (!terminated(file_name, name)) return failed_library(exhausted(operation));
    uptr handle = 0;
    const Status opened = image_backend::open_library(name.data(), image_backend::LoadMode::system, handle);
    if (opened.failed()) return failed_library(opened);
    return Result<Library>::success(Library(handle));
}

Result<Library> Library::find_loaded(const TextView file_name) noexcept {
    const Operation operation = "stxlib.library.find_loaded";
    if (!capability_supported(Capability::dynamic_libraries)) return failed_library(unsupported(operation));
    if (!acceptable_file_name(file_name)) return failed_library(refused(operation));
    Growable<char> name;
    if (!terminated(file_name, name)) return failed_library(exhausted(operation));
    uptr handle = 0;
    const Status opened = image_backend::open_library(name.data(), image_backend::LoadMode::loaded_only, handle);
    if (opened.failed()) return failed_library(opened);
    return Result<Library>::success(Library(handle));
}

Library::~Library() {
    if (handle_ != 0) static_cast<void>(image_backend::close_library(handle_));
}

Library& Library::operator=(Library&& other) noexcept {
    if (this == &other) return *this;
    if (handle_ != 0) static_cast<void>(image_backend::close_library(handle_));
    handle_ = other.handle_;
    other.handle_ = 0;
    return *this;
}

Result<SymbolAddress> Library::symbol(const TextView name) const noexcept {
    const Operation operation = "stxlib.library.symbol";
    if (handle_ == 0) {
        const ErrorRecord record = host_failure::record(ErrorKind::disposed, operation, HostCode::closed_handle);
        return Result<SymbolAddress>::failure(record);
    }
    if (!acceptable_symbol(name)) {
        const Status refusal = refused(operation);
        return Result<SymbolAddress>::failure(*refusal.error());
    }
    Growable<char> terminated_name;
    if (!terminated(name, terminated_name)) {
        const Status exhaustion = exhausted(operation);
        return Result<SymbolAddress>::failure(*exhaustion.error());
    }
    uptr address = 0;
    const Status found = image_backend::find_symbol(handle_, terminated_name.data(), address);
    if (found.failed()) return Result<SymbolAddress>::failure(*found.error());
    return Result<SymbolAddress>::success(SymbolAddress(address));
}

Status Library::close() noexcept {
    if (handle_ == 0) {
        return host_failure::failed(ErrorKind::disposed, "stxlib.library.close", HostCode::closed_handle);
    }
    const uptr handle = handle_;
    handle_ = 0;
    const Status result = image_backend::close_library(handle);
    return result;
}

} // namespace stxlib
