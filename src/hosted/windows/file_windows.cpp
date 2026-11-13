// The Windows file backend (hosted/file_backend.hpp), over the Win32 file
// API. Ported from stx_auxlib's src/hosted/file_windows.cpp at b6e330a, with
// the operations stx_auxlib took from std::filesystem written over the Win32
// calls (CreateDirectoryW, DeleteFileW and RemoveDirectoryW, MoveFileExW,
// CopyFileW, FindFirstFileExW, GetTempPathW, GetCurrentDirectoryW,
// GetFinalPathNameByHandleW, FSCTL_GET_REPARSE_POINT, CreateSymbolicLinkW).
//
// Drive paths take the "\\?\" prefix so that names longer than MAX_PATH work
// without the process opting into long paths (a Path is normalized, which
// the prefix requires: no "." or ".." segments, '\' separators). Files open
// shared for reading, writing and deletion, so that renaming over an open
// file behaves as it does on POSIX hosts where Windows allows it.
//
// Names both ways are WTF-8 (file_backend.hpp, "Names"; owner decision D13
// a): a Path's text is converted to UTF-16 with core/wtf8.hpp's to_utf16,
// and names Windows returns come back with from_utf16, so a name with an
// unpaired surrogate (NTFS keeps a name as 16-bit units and does not check
// pairing) round-trips exactly. The "\\?\" prefix is removed, and UNC
// names ("\\server\share", "\\?\UNC\") are refused as unsupported, since
// Path does not represent them.

#include "core/wtf8.hpp"
#include "hosted/file_backend.hpp"
#include "hosted/windows/utf16.hpp"

#include <cstdint>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

namespace stxlib::file_backend {

namespace {

// One ReadFile or WriteFile moves at most this much (a DWORD count).
constexpr usize largest_transfer = static_cast<usize>(1) << 30;

// winerror.h: creating a symbolic link without developer mode or the
// symbolic link privilege.
constexpr DWORD privilege_not_held = 1314;

#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif

// stx_auxlib's status_from_last_error, plus the privilege error stx_auxlib
// mapped in files.cpp.
FileStatus status_of_error(const DWORD error) noexcept {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return FileStatus::not_found;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return FileStatus::already_exists;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case privilege_not_held:
        return FileStatus::permission_denied;
    case ERROR_DIRECTORY:
        return FileStatus::not_a_directory;
    case ERROR_DIR_NOT_EMPTY:
        return FileStatus::directory_not_empty;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return FileStatus::no_space;
    case ERROR_FILENAME_EXCED_RANGE:
        return FileStatus::name_too_long;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
        return FileStatus::invalid_name;
    case ERROR_NOT_SAME_DEVICE:
        return FileStatus::cross_device;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return FileStatus::busy;
    case ERROR_INVALID_HANDLE:
        return FileStatus::bad_handle;
    case ERROR_NOT_SUPPORTED:
    case ERROR_CALL_NOT_IMPLEMENTED:
        return FileStatus::unsupported;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return FileStatus::out_of_memory;
    default:
        break;
    }
    return FileStatus::io_error;
}

Outcome from_error(const DWORD error) noexcept {
    const Outcome result{status_of_error(error),
                         ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error))};
    return result;
}

Outcome last_error() noexcept {
    const Outcome result = from_error(GetLastError());
    return result;
}

Outcome no_memory() noexcept {
    const Outcome result = failure(FileStatus::out_of_memory);
    return result;
}

HANDLE native_handle(const i64 handle) noexcept {
    const HANDLE result = reinterpret_cast<HANDLE>(static_cast<intptr_t>(handle));
    return result;
}

// The NUL-terminated wide name of a Path text: '\' separators, and "\\?\"
// before a drive path.
bool wide_name(const char* const name, Growable<wchar_t>& output) noexcept {
    output.clear();
    const usize count = std::strlen(name);
    const bool drive_path = count >= 3 && name[1] == ':' && name[2] == '/';
    if (drive_path && !output.append(L"\\\\?\\", 4)) return false;
    const usize start = output.count();
    if (!wtf8::to_utf16(name, count, output)) return false;
    for (usize index = start; index < output.count(); ++index) {
        if (output[index] == L'/') output[index] = L'\\';
    }
    const bool result = utf16::terminate(output);
    return result;
}

// A Path text as Windows stores it in a link: '\' separators, no prefix.
bool wide_target(const char* const name, Growable<wchar_t>& output) noexcept {
    output.clear();
    if (!wtf8::to_utf16(name, std::strlen(name), output)) return false;
    for (usize index = 0; index < output.count(); ++index) {
        if (output[index] == L'/') output[index] = L'\\';
    }
    const bool result = utf16::terminate(output);
    return result;
}

bool starts_with(const wchar_t* const units, const usize count, const wchar_t* const prefix) noexcept {
    usize index = 0;
    while (prefix[index] != L'\0') {
        if (index >= count || units[index] != prefix[index]) return false;
        index = index + 1;
    }
    return true;
}

// A name Windows returned, as WTF-8 with '/' separators: "\\?\" removed,
// UNC refused (unsupported), unpaired surrogates carried.
Outcome narrow_name(const wchar_t* units, usize count, Growable<char>& text) noexcept {
    text.clear();
    if (starts_with(units, count, L"\\\\?\\UNC\\")) return failure(FileStatus::unsupported);
    if (starts_with(units, count, L"\\\\?\\") || starts_with(units, count, L"\\??\\")) {
        units = units + 4;
        count = count - 4;
    } else if (starts_with(units, count, L"\\\\")) {
        return failure(FileStatus::unsupported);
    }
    if (!wtf8::from_utf16(units, count, text)) return no_memory();
    for (usize index = 0; index < text.count(); ++index) {
        if (text[index] == '\\') text[index] = '/';
    }
    return success();
}

// Unix nanoseconds of a FILETIME (100 ns ticks since 1601-01-01).
WallTime wall_time_of(const FILETIME stamp) noexcept {
    const u64 ticks = (static_cast<u64>(stamp.dwHighDateTime) << 32) | stamp.dwLowDateTime;
    constexpr i64 unix_epoch_ticks = 116444736000000000LL;
    const i64 since_epoch = static_cast<i64>(ticks) - unix_epoch_ticks;
    const WallTime result = WallTime::from_unix_nanoseconds(detail::saturating_scale(since_epoch, 100));
    return result;
}

// Owns a handle for one call.
class OwnedHandle final {
public:
    explicit OwnedHandle(const HANDLE handle) noexcept : handle_(handle) {}
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) (void)CloseHandle(handle_);
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

private:
    HANDLE handle_;
};

// Opens a file or directory for its attributes only; `own_link` opens a
// reparse point itself rather than what it names.
HANDLE open_for_attributes(const wchar_t* const name, const bool own_link) noexcept {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (own_link) flags = flags | FILE_FLAG_OPEN_REPARSE_POINT;
    const HANDLE result = CreateFileW(name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, flags, nullptr);
    return result;
}

// A reparse point's kind from its tag: symbolic links and junctions (mount
// points, links to directories) are links; other reparse points (cloud
// placeholders, deduplicated files) are described by their attributes.
bool is_link_tag(const DWORD tag) noexcept {
    const bool result = tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT;
    return result;
}

FileKind kind_of_attributes(const DWORD attributes, const DWORD reparse_tag) noexcept {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && is_link_tag(reparse_tag)) return FileKind::symbolic_link;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return FileKind::directory;
    return FileKind::regular;
}

// The reparse data FSCTL_GET_REPARSE_POINT returns for symbolic links and
// mount points. Its declaration (REPARSE_DATA_BUFFER) is in the driver kit's
// ntifs.h, not the SDK's windows.h, so the layout is restated here as
// documented ("REPARSE_DATA_BUFFER structure", Windows driver documentation):
// a tag, the data length, a reserved word, then for both kinds four USHORT
// offsets and lengths of the substitute and print names in bytes, a ULONG of
// flags for symbolic links only, and the WCHAR path buffer.
struct ReparseHeader final {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
};

constexpr usize symbolic_link_buffer_offset = sizeof(ReparseHeader) + sizeof(ULONG);
constexpr usize mount_point_buffer_offset = sizeof(ReparseHeader);
constexpr usize reparse_buffer_bytes = 16 * 1024; // MAXIMUM_REPARSE_DATA_BUFFER_SIZE

} // namespace

Outcome open(const char* const name, const FileMode mode, i64& handle) noexcept {
    handle = no_handle;
    DWORD access = 0;
    DWORD disposition = 0;
    switch (mode) {
    case FileMode::read:
        access = GENERIC_READ;
        disposition = OPEN_EXISTING;
        break;
    case FileMode::create_new:
        access = GENERIC_WRITE;
        disposition = CREATE_NEW;
        break;
    case FileMode::replace:
        access = GENERIC_WRITE;
        disposition = CREATE_ALWAYS;
        break;
    case FileMode::append:
        access = FILE_APPEND_DATA | SYNCHRONIZE;
        disposition = OPEN_ALWAYS;
        break;
    default:
        return failure(FileStatus::io_error);
    }
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    const DWORD attributes = GetFileAttributesW(wide.data());
    if (mode == FileMode::read && attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return failure(FileStatus::is_a_directory);
    }
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const HANDLE opened =
        CreateFileW(wide.data(), access, sharing, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (opened == INVALID_HANDLE_VALUE) return last_error();
    handle = static_cast<i64>(reinterpret_cast<intptr_t>(opened));
    return success();
}

Outcome read(const i64 handle, Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    DWORD got = 0;
    if (!ReadFile(native_handle(handle), bytes, static_cast<DWORD>(request), &got, nullptr)) {
        // A pipe's writer closing is the end of input, not a failure.
        if (GetLastError() == ERROR_BROKEN_PIPE) return success();
        return last_error();
    }
    transferred = static_cast<usize>(got);
    return success();
}

Outcome write(const i64 handle, const Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const usize request = count < largest_transfer ? count : largest_transfer;
    DWORD put = 0;
    if (!WriteFile(native_handle(handle), bytes, static_cast<DWORD>(request), &put, nullptr)) return last_error();
    if (put == 0 && request != 0) return failure(FileStatus::io_error);
    transferred = static_cast<usize>(put);
    return success();
}

Outcome size(const i64 handle, u64& bytes) noexcept {
    bytes = 0;
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(native_handle(handle), &length)) return last_error();
    if (length.QuadPart < 0) return failure(FileStatus::io_error);
    bytes = static_cast<u64>(length.QuadPart);
    return success();
}

Outcome position(const i64 handle, u64& offset) noexcept {
    offset = 0;
    LARGE_INTEGER zero{};
    LARGE_INTEGER current{};
    if (!SetFilePointerEx(native_handle(handle), zero, &current, FILE_CURRENT)) return last_error();
    offset = static_cast<u64>(current.QuadPart);
    return success();
}

Outcome seek(const i64 handle, const u64 offset) noexcept {
    if (offset > static_cast<u64>(i64_max)) return failure(FileStatus::too_large);
    LARGE_INTEGER target{};
    target.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(native_handle(handle), target, nullptr, FILE_BEGIN)) return last_error();
    return success();
}

// ordered and durable both flush: FlushFileBuffers is Win32's only flush
// (D13 a names FlushFileBuffers for Windows).
Outcome sync(const i64 handle, const Durability level) noexcept {
    if (level == Durability::none) return success();
    if (!FlushFileBuffers(native_handle(handle))) return last_error();
    return success();
}

Outcome close(const i64 handle) noexcept {
    if (!CloseHandle(native_handle(handle))) return last_error();
    return success();
}

Outcome inspect(const char* const name, const LinkMode links, FileInformation& information) noexcept {
    information = FileInformation{};
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    const OwnedHandle opened(open_for_attributes(wide.data(), links == LinkMode::no_follow));
    if (!opened.valid()) return last_error();
    BY_HANDLE_FILE_INFORMATION described{};
    if (!GetFileInformationByHandle(opened.get(), &described)) return last_error();
    DWORD tag = 0;
    if (links == LinkMode::no_follow && (described.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        FILE_ATTRIBUTE_TAG_INFO tag_information{};
        if (!GetFileInformationByHandleEx(opened.get(), FileAttributeTagInfo, &tag_information,
                                          sizeof tag_information)) {
            return last_error();
        }
        tag = tag_information.ReparseTag;
    }
    information.kind = kind_of_attributes(described.dwFileAttributes, tag);
    if (information.kind == FileKind::regular) {
        information.size = (static_cast<u64>(described.nFileSizeHigh) << 32) | described.nFileSizeLow;
    }
    information.modified = wall_time_of(described.ftLastWriteTime);
    return success();
}

// MoveFileExW replacing a file is not documented as atomic, so
// Capability::atomic_replace is false here. MOVEFILE_WRITE_THROUGH: the
// call returns once the move is flushed to disk (MoveFileExW's
// documentation).
Outcome replace(const char* const from, const char* const to, const Durability level) noexcept {
    Growable<wchar_t> source;
    Growable<wchar_t> target;
    if (!wide_name(from, source) || !wide_name(to, target)) return no_memory();
    DWORD flags = MOVEFILE_REPLACE_EXISTING;
    if (level == Durability::durable) flags = flags | MOVEFILE_WRITE_THROUGH;
    if (!MoveFileExW(source.data(), target.data(), flags)) {
        return last_error();
    }
    return success();
}

Outcome create_directory(const char* const name) noexcept {
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    if (!CreateDirectoryW(wide.data(), nullptr)) return last_error();
    return success();
}

// A directory, or a link to one (its own attributes say directory), is
// removed with RemoveDirectoryW, which removes a link and not what it names;
// anything else with DeleteFileW.
Outcome remove(const char* const name) noexcept {
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    const DWORD attributes = GetFileAttributesW(wide.data());
    if (attributes == INVALID_FILE_ATTRIBUTES) return last_error();
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (!RemoveDirectoryW(wide.data())) return last_error();
        return success();
    }
    if (!DeleteFileW(wide.data())) return last_error();
    return success();
}

Outcome rename(const char* const from, const char* const to) noexcept {
    Growable<wchar_t> source;
    Growable<wchar_t> target;
    if (!wide_name(from, source) || !wide_name(to, target)) return no_memory();
    if (!MoveFileExW(source.data(), target.data(), MOVEFILE_REPLACE_EXISTING)) return last_error();
    return success();
}

// Whether two open handles name one file. Microsoft's
// BY_HANDLE_FILE_INFORMATION documentation: the volume serial number and the
// file index identify a file, and on ReFS the 64-bit index may not be unique,
// so FILE_ID_INFO's 128-bit identifier (GetFileInformationByHandleEx,
// FileIdInfo; Windows 8 and later) is asked first. (From those pages as
// published, not re-read in this batch.)
bool same_file(const HANDLE first, const HANDLE second) noexcept {
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602
    FILE_ID_INFO first_id{};
    FILE_ID_INFO second_id{};
    if (GetFileInformationByHandleEx(first, FileIdInfo, &first_id, sizeof first_id) &&
        GetFileInformationByHandleEx(second, FileIdInfo, &second_id, sizeof second_id)) {
        const bool result = first_id.VolumeSerialNumber == second_id.VolumeSerialNumber &&
                            std::memcmp(&first_id.FileId, &second_id.FileId, sizeof first_id.FileId) == 0;
        return result;
    }
#endif
    BY_HANDLE_FILE_INFORMATION first_information{};
    BY_HANDLE_FILE_INFORMATION second_information{};
    if (!GetFileInformationByHandle(first, &first_information) ||
        !GetFileInformationByHandle(second, &second_information)) {
        return false;
    }
    const bool result = first_information.dwVolumeSerialNumber == second_information.dwVolumeSerialNumber &&
                        first_information.nFileIndexHigh == second_information.nFileIndexHigh &&
                        first_information.nFileIndexLow == second_information.nFileIndexLow;
    return result;
}

// A copy onto the source itself is refused (already_exists), as on POSIX
// hosts: CopyFileW does not refuse it (CI run 43429568298, Windows Debug,
// tests/files_tests.cpp's copy onto itself).
Outcome copy_file(const char* const from, const char* const to, const CopyMode mode) noexcept {
    Growable<wchar_t> source;
    Growable<wchar_t> target;
    if (!wide_name(from, source) || !wide_name(to, target)) return no_memory();
    const DWORD attributes = GetFileAttributesW(source.data());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return failure(FileStatus::is_a_directory);
    }
    {
        const OwnedHandle source_handle(open_for_attributes(source.data(), false));
        if (!source_handle.valid()) return last_error();
        const OwnedHandle target_handle(open_for_attributes(target.data(), false));
        if (target_handle.valid() && same_file(source_handle.get(), target_handle.get())) {
            return failure(FileStatus::already_exists);
        }
    }
    const BOOL fail_if_exists = mode == CopyMode::fail_if_exists ? TRUE : FALSE;
    if (!CopyFileW(source.data(), target.data(), fail_if_exists)) return last_error();
    return success();
}

Outcome list(const char* const name, const usize limit, Growable<char>& names, Growable<RawEntry>& entries) noexcept {
    Growable<wchar_t> pattern;
    if (!wide_name(name, pattern)) return no_memory();
    pattern.shrink(pattern.count() - 1);
    // A root already ends with its separator ("\\?\C:\", "\").
    const bool separated = !pattern.empty() && pattern[pattern.count() - 1] == L'\\';
    const bool appended = separated ? pattern.push(L'*') : pattern.append(L"\\*", 2);
    if (!appended || !utf16::terminate(pattern)) return no_memory();
    WIN32_FIND_DATAW found{};
    const HANDLE search =
        FindFirstFileExW(pattern.data(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (search == INVALID_HANDLE_VALUE) return last_error();
    usize seen = 0;
    Outcome outcome = success();
    for (;;) {
        const wchar_t* const entry_name = found.cFileName;
        const bool dot = entry_name[0] == L'.' && entry_name[1] == L'\0';
        const bool dot_dot = entry_name[0] == L'.' && entry_name[1] == L'.' && entry_name[2] == L'\0';
        if (!dot && !dot_dot) {
            if (seen == limit) {
                outcome = failure(FileStatus::too_large);
                break;
            }
            seen = seen + 1;
            usize units = 0;
            while (entry_name[units] != L'\0') units = units + 1;
            const usize offset = names.count();
            if (!wtf8::from_utf16(entry_name, units, names)) {
                outcome = no_memory();
                break;
            }
            // dwReserved0 holds the reparse tag when the entry is a reparse
            // point (WIN32_FIND_DATAW's documentation).
            const RawEntry raw{offset, names.count() - offset,
                               kind_of_attributes(found.dwFileAttributes, found.dwReserved0)};
            if (!names.push('\0') || !entries.push(raw)) {
                outcome = no_memory();
                break;
            }
        }
        if (!FindNextFileW(search, &found)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) outcome = from_error(error);
            break;
        }
    }
    (void)FindClose(search);
    return outcome;
}

Outcome temporary_directory(Growable<char>& text) noexcept {
    Growable<wchar_t> buffer;
    DWORD capacity = MAX_PATH + 1;
    DWORD length = 0;
    for (;;) {
        if (!buffer.resize(capacity)) return no_memory();
        length = GetTempPathW(capacity, buffer.data());
        if (length == 0) return last_error();
        if (length < capacity) break;
        capacity = length + 1; // the size needed, terminator included
    }
    const Outcome result = narrow_name(buffer.data(), length, text);
    return result;
}

Outcome working_directory(Growable<char>& text) noexcept {
    Growable<wchar_t> buffer;
    DWORD capacity = MAX_PATH + 1;
    DWORD length = 0;
    for (;;) {
        if (!buffer.resize(capacity)) return no_memory();
        length = GetCurrentDirectoryW(capacity, buffer.data());
        if (length == 0) return last_error();
        if (length < capacity) break;
        capacity = length; // the size needed, terminator included
    }
    const Outcome result = narrow_name(buffer.data(), length, text);
    return result;
}

Outcome resolve(const char* const name, Growable<char>& text) noexcept {
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    const OwnedHandle opened(open_for_attributes(wide.data(), false));
    if (!opened.valid()) return last_error();
    Growable<wchar_t> buffer;
    DWORD capacity = MAX_PATH + 1;
    DWORD length = 0;
    for (;;) {
        if (!buffer.resize(capacity)) return no_memory();
        length = GetFinalPathNameByHandleW(opened.get(), buffer.data(), capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0) return last_error();
        if (length < capacity) break;
        capacity = length + 1;
    }
    const Outcome result = narrow_name(buffer.data(), length, text);
    return result;
}

// The print name when present, else the substitute name; a relative
// symbolic link's names are relative.
Outcome read_link(const char* const name, Growable<char>& text) noexcept {
    Growable<wchar_t> wide;
    if (!wide_name(name, wide)) return no_memory();
    const OwnedHandle opened(open_for_attributes(wide.data(), true));
    if (!opened.valid()) return last_error();
    Growable<Byte> buffer;
    if (!buffer.resize(reparse_buffer_bytes)) return no_memory();
    DWORD returned = 0;
    if (!DeviceIoControl(opened.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(),
                         static_cast<DWORD>(buffer.count()), &returned, nullptr)) {
        return last_error();
    }
    if (returned < sizeof(ReparseHeader)) return failure(FileStatus::io_error);
    ReparseHeader header{};
    std::memcpy(&header, buffer.data(), sizeof header);
    usize path_buffer = 0;
    if (header.tag == IO_REPARSE_TAG_SYMLINK) {
        path_buffer = symbolic_link_buffer_offset;
    } else if (header.tag == IO_REPARSE_TAG_MOUNT_POINT) {
        path_buffer = mount_point_buffer_offset;
    } else {
        return failure(FileStatus::unsupported);
    }
    usize offset = header.print_offset;
    usize length = header.print_length;
    if (length == 0) {
        offset = header.substitute_offset;
        length = header.substitute_length;
    }
    if (path_buffer + offset + length > returned || length % sizeof(wchar_t) != 0) return failure(FileStatus::io_error);
    Growable<wchar_t> units;
    if (!units.resize(length / sizeof(wchar_t))) return no_memory();
    if (length != 0) std::memcpy(units.data(), buffer.data() + path_buffer + offset, length);
    const Outcome result = narrow_name(units.data(), units.count(), text);
    return result;
}

// SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE lets developer mode create
// links without the privilege; Windows versions before it refuse the flag
// with ERROR_INVALID_PARAMETER, so the call is repeated without it.
Outcome create_link(const char* const target, const char* const link, const bool to_directory) noexcept {
    Growable<wchar_t> wide_link;
    Growable<wchar_t> wide_link_target;
    if (!wide_name(link, wide_link) || !wide_target(target, wide_link_target)) return no_memory();
    DWORD flags = to_directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
    if (CreateSymbolicLinkW(wide_link.data(), wide_link_target.data(), flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        return success();
    }
    if (GetLastError() != ERROR_INVALID_PARAMETER) return last_error();
    if (!CreateSymbolicLinkW(wide_link.data(), wide_link_target.data(), flags)) return last_error();
    return success();
}

} // namespace stxlib::file_backend
