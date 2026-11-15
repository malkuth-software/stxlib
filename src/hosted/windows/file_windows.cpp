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
        // Lets go before closing (AGENTS.md, rule 1).
        const HANDLE held = handle_;
        handle_ = nullptr;
        if (held != INVALID_HANDLE_VALUE && held != nullptr) (void)CloseHandle(held);
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
    // FILE_READ_ATTRIBUTES for File::information and size on every mode;
    // FILE_WRITE_ATTRIBUTES (in GENERIC_WRITE) for File::set_modified, added
    // to the append access, which holds no other write right (M2 C2).
    case FileMode::create_new:
        access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        disposition = CREATE_NEW;
        break;
    case FileMode::replace:
        access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
        disposition = CREATE_ALWAYS;
        break;
    case FileMode::append:
        access = FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
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

// ---- positioned I/O and the open file's metadata (M2 C2) ----
// ReadFile and WriteFile with an OVERLAPPED offset on a handle opened
// without FILE_FLAG_OVERLAPPED read or write at that offset and move the
// file pointer past it ("ReadFile function", Synchronization and File
// Position; Microsoft Learn, as published), so the pointer is read first
// and put back after: File's position is unchanged, as with pread.

namespace {

OVERLAPPED at_offset(const u64 offset) noexcept {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
    return overlapped;
}

bool current_pointer(const HANDLE handle, LARGE_INTEGER& saved) noexcept {
    LARGE_INTEGER zero{};
    const bool result = SetFilePointerEx(handle, zero, &saved, FILE_CURRENT) != 0;
    return result;
}

Outcome restore_pointer(const HANDLE handle, const LARGE_INTEGER saved, const Outcome outcome) noexcept {
    if (!SetFilePointerEx(handle, saved, nullptr, FILE_BEGIN) && outcome.ok()) return last_error();
    return outcome;
}

// FILETIME ticks (100 ns since 1601-01-01) of a WallTime, toward the past;
// false before 1601.
bool ticks_of(const WallTime time, FILETIME& stamp) noexcept {
    constexpr i64 unix_epoch_ticks = 116444736000000000LL;
    const i64 count = time.unix_nanoseconds();
    i64 ticks = count / 100;
    if (count % 100 < 0) ticks = ticks - 1;
    if (ticks < -unix_epoch_ticks) return false;
    const u64 since_1601 = static_cast<u64>(ticks + unix_epoch_ticks);
    stamp.dwLowDateTime = static_cast<DWORD>(since_1601 & 0xFFFFFFFFu);
    stamp.dwHighDateTime = static_cast<DWORD>(since_1601 >> 32);
    return true;
}

} // namespace

Outcome read_at(const i64 handle, const u64 offset, Byte* const bytes, const usize count, usize& transferred) noexcept {
    transferred = 0;
    const HANDLE file = native_handle(handle);
    LARGE_INTEGER saved{};
    if (!current_pointer(file, saved)) return last_error();
    const usize request = count < largest_transfer ? count : largest_transfer;
    OVERLAPPED overlapped = at_offset(offset);
    DWORD got = 0;
    Outcome outcome = success();
    if (!ReadFile(file, bytes, static_cast<DWORD>(request), &got, &overlapped)) {
        // At or past the end: zero bytes, not a failure.
        const DWORD error = GetLastError();
        if (error != ERROR_HANDLE_EOF) outcome = from_error(error);
        got = 0;
    }
    transferred = static_cast<usize>(got);
    const Outcome result = restore_pointer(file, saved, outcome);
    return result;
}

Outcome write_at(const i64 handle, const u64 offset, const Byte* const bytes, const usize count,
                 usize& transferred) noexcept {
    transferred = 0;
    const HANDLE file = native_handle(handle);
    LARGE_INTEGER saved{};
    if (!current_pointer(file, saved)) return last_error();
    const usize request = count < largest_transfer ? count : largest_transfer;
    OVERLAPPED overlapped = at_offset(offset);
    DWORD put = 0;
    Outcome outcome = success();
    if (!WriteFile(file, bytes, static_cast<DWORD>(request), &put, &overlapped)) {
        outcome = last_error();
    } else if (put == 0 && request != 0) {
        outcome = failure(FileStatus::io_error);
    }
    transferred = outcome.ok() ? static_cast<usize>(put) : 0;
    const Outcome result = restore_pointer(file, saved, outcome);
    return result;
}

Outcome information(const i64 handle, FileInformation& information) noexcept {
    information = FileInformation{};
    BY_HANDLE_FILE_INFORMATION described{};
    if (!GetFileInformationByHandle(native_handle(handle), &described)) return last_error();
    // File::open follows links, so the handle holds no link: a reparse point
    // here is a placeholder or similar, described by its attributes.
    information.kind = kind_of_attributes(described.dwFileAttributes, 0);
    if (information.kind == FileKind::regular) {
        information.size = (static_cast<u64>(described.nFileSizeHigh) << 32) | described.nFileSizeLow;
    }
    information.modified = wall_time_of(described.ftLastWriteTime);
    return success();
}

// SetFileTime with null creation and access times leaves them unchanged.
// A time before 1601 is refused (too_large: limit_exceeded, too_long).
Outcome set_modified(const i64 handle, const WallTime modified) noexcept {
    FILETIME stamp{};
    if (!ticks_of(modified, stamp)) return failure(FileStatus::too_large);
    if (!SetFileTime(native_handle(handle), nullptr, nullptr, &stamp)) return last_error();
    return success();
}

Outcome set_size(const i64 handle, const u64 bytes) noexcept {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFileInformationByHandle(native_handle(handle), FileEndOfFileInfo, &end, sizeof end)) return last_error();
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

// Nothing to do: Windows has no permission bits to carry. The temporary
// file takes the directory's inherited ACL, and MoveFileExW keeps the
// moved file's own security descriptor ("MoveFileExW function", Microsoft
// Learn: the moved file keeps its descriptor), so a replaced file's
// explicit ACL is not carried (files.hpp, write_file).
Outcome carry_permissions(const i64 handle, const char* const target) noexcept {
    static_cast<void>(handle);
    static_cast<void>(target);
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

// ---- remove_tree (files.hpp states the safety and the bounds) ----
//
// Win32 has no openat, so the path to each directory is kept stable
// instead: every directory entered is held open, from entry to removal,
// with a share mode that leaves out FILE_SHARE_DELETE, and renaming or
// deleting a file needs DELETE access, which such a share mode refuses to
// every other opener ("Creating and Opening Files", sharing modes;
// Microsoft Learn, as published). So no directory on the held path can be
// renamed, removed or replaced by a link while the walk is below it, and
// the path to a child names the child of the held directory. Each entry is
// opened with FILE_FLAG_OPEN_REPARSE_POINT, judged by its own handle's
// FileAttributeTagInfo (a symbolic link or junction is a link, never
// entered; other reparse points with the directory attribute, such as
// cloud placeholders, are directories, as inspect describes them), and
// removed through that handle with FileDispositionInfoEx: DELETE, POSIX
// semantics (the name leaves the directory when the handle closes, so a
// virus scanner's open handle does not keep it listed) and
// IGNORE_READONLY_ATTRIBUTE (FILE_DISPOSITION_INFORMATION_EX, Windows
// driver documentation, docs commit dc334b3; flags 0x1, 0x2, 0x10). Where
// the volume refuses that class (FAT, older systems), the read-only
// attribute is cleared and FileDispositionInfo is used. The listing is read
// from the held handle (GetFileInformationByHandleEx with
// FileFullDirectoryRestartInfo, then FileFullDirectoryInfo), in passes, as
// on POSIX hosts.

namespace {

// FILE_INFO_BY_HANDLE_CLASS's FileDispositionInfoEx (its 22nd enumerator,
// minwinbase.h; "FILE_INFO_BY_HANDLE_CLASS enumeration", docs commit
// 9267262), and FILE_DISPOSITION_INFO_EX's one ULONG of flags, restated so
// the build does not depend on the headers' Windows-version guards.
constexpr FILE_INFO_BY_HANDLE_CLASS disposition_ex_class = static_cast<FILE_INFO_BY_HANDLE_CLASS>(21);
constexpr ULONG disposition_delete = 0x1;
constexpr ULONG disposition_posix_semantics = 0x2;
constexpr ULONG disposition_ignore_readonly = 0x10;

struct DispositionEx final {
    ULONG flags;
};

constexpr DWORD tree_flags = FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT;
constexpr DWORD directory_access = DELETE | FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
constexpr DWORD entry_access = DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE;
// Held directories leave out FILE_SHARE_DELETE (above); entries removed at
// once share everything.
constexpr DWORD held_share = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr DWORD entry_share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Marks the open file for deletion (above); it goes when the handle closes.
Outcome dispose(const HANDLE handle) noexcept {
    DispositionEx extended{};
    extended.flags = disposition_delete | disposition_posix_semantics | disposition_ignore_readonly;
    if (SetFileInformationByHandle(handle, disposition_ex_class, &extended, sizeof extended)) return success();
    const DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION) {
        return from_error(error);
    }
    FILE_BASIC_INFO basic{};
    if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof basic) &&
        (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        // Zero times leave them unchanged (FILE_BASIC_INFO).
        FILE_BASIC_INFO cleared{};
        cleared.FileAttributes = basic.FileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
        if (cleared.FileAttributes == 0) cleared.FileAttributes = FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &cleared, sizeof cleared)) return last_error();
    }
    // Without POSIX semantics the name stays listed until every handle on
    // the file closes; a pass that then meets it fails to open it
    // (ERROR_ACCESS_DENIED for a delete-pending file), and remove_tree
    // reports permission_denied rather than looping.
    FILE_DISPOSITION_INFO classic{TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &classic, sizeof classic)) return last_error();
    return success();
}

// Whether the open object is a directory to enter: the directory attribute
// and not a link (symbolic link or junction).
Outcome judge(const HANDLE handle, bool& directory) noexcept {
    directory = false;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof tag)) return last_error();
    const bool link = (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && is_link_tag(tag.ReparseTag);
    directory = (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !link;
    return success();
}

struct HeldDirectory final {
    HANDLE handle{INVALID_HANDLE_VALUE};
    // The length of its path in the path buffer, without the NUL.
    usize path_count{0};
    // Entries removed from it so far (entries_limit bounds it).
    usize removed_here{0};
};

// Counts one entry removed from the directory `directory`; false past
// `entries_limit` (RemoveTreeLimits::entries_per_directory).
bool count_removed(HeldDirectory& directory, const usize entries_limit) noexcept {
    directory.removed_here = directory.removed_here + 1;
    const bool result = directory.removed_here <= entries_limit;
    return result;
}

void release_all(Growable<HeldDirectory>& held) noexcept {
    for (usize index = 0; index < held.count(); ++index) (void)CloseHandle(held[index].handle);
    held.clear();
}

// Disposes and closes; a failure still closes.
Outcome dispose_and_close(const HANDLE handle) noexcept {
    const Outcome disposed = dispose(handle);
    const bool closed = CloseHandle(handle) != 0;
    if (!disposed.ok()) return disposed;
    if (!closed) return last_error();
    return success();
}

bool is_dot_name(const wchar_t* const name, const usize count) noexcept {
    const bool dot = count == 1 && name[0] == L'.';
    const bool dot_dot = count == 2 && name[0] == L'.' && name[1] == L'.';
    return dot || dot_dot;
}

// `path` cut to `count` units, then '', `name`, NUL.
bool child_path(Growable<wchar_t>& path, const usize count, const wchar_t* const name, const usize name_count) noexcept {
    path.shrink(count);
    const bool separated = count != 0 && path[count - 1] == L'\\';
    const bool result =
        (separated || path.push(L'\\')) && path.append(name, name_count) && path.push(L'\0');
    return result;
}

usize unit_count(const wchar_t* const units) noexcept {
    usize count = 0;
    while (units[count] != L'\0') count = count + 1;
    return count;
}

// One pass's result over the top directory's listing.
enum class PassEnd : u8 {
    empty = 0,   // a whole pass found nothing
    entered = 1, // a directory was entered (pushed)
};

// Reads the top directory in passes: non-directories are removed, the first
// directory met is entered.
Outcome walk_top(Growable<HeldDirectory>& held, Growable<wchar_t>& path, const usize depth_limit,
                 const usize entries_limit, u64& removed, PassEnd& end) noexcept {
    end = PassEnd::empty;
    alignas(8) unsigned char listing[16 * 1024];
    const HeldDirectory top = held[held.count() - 1];
    bool restart = true;
    bool again = false;
    for (;;) {
        const FILE_INFO_BY_HANDLE_CLASS read_class = restart ? FileFullDirectoryRestartInfo : FileFullDirectoryInfo;
        restart = false;
        if (!GetFileInformationByHandleEx(top.handle, read_class, listing, sizeof listing)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) return from_error(error);
            if (!again) return success();
            again = false;
            restart = true;
            continue;
        }
        usize offset = 0;
        for (;;) {
            const FILE_FULL_DIR_INFO& entry = *reinterpret_cast<const FILE_FULL_DIR_INFO*>(listing + offset);
            const usize name_count = static_cast<usize>(entry.FileNameLength) / sizeof(wchar_t);
            if (!is_dot_name(entry.FileName, name_count)) {
                if (!child_path(path, top.path_count, entry.FileName, name_count)) return no_memory();
                const bool listed_directory = (entry.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                              (entry.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
                const DWORD access = listed_directory ? directory_access : entry_access;
                const DWORD share = listed_directory ? held_share : entry_share;
                const HANDLE opened = CreateFileW(path.data(), access, share, nullptr, OPEN_EXISTING, tree_flags, nullptr);
                if (opened == INVALID_HANDLE_VALUE) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return from_error(error);
                } else {
                    bool directory = false;
                    const Outcome judged = judge(opened, directory);
                    if (!judged.ok()) {
                        (void)CloseHandle(opened);
                        return judged;
                    }
                    if (!directory) {
                        const Outcome gone = dispose_and_close(opened);
                        if (!gone.ok()) return gone;
                        removed = removed + 1;
                        again = true;
                        if (!count_removed(held[held.count() - 1], entries_limit)) {
                            return failure(FileStatus::too_large);
                        }
                    } else if (!listed_directory) {
                        // Became a directory since it was listed; opened
                        // without the listing right, so read it next pass.
                        (void)CloseHandle(opened);
                        again = true;
                    } else {
                        // held holds the top and the directories below it.
                        if (held.count() > depth_limit) {
                            (void)CloseHandle(opened);
                            return failure(FileStatus::too_large);
                        }
                        HeldDirectory entered{};
                        entered.handle = opened;
                        entered.path_count = path.count() - 1;
                        if (!held.push(entered)) {
                            (void)CloseHandle(opened);
                            return no_memory();
                        }
                        end = PassEnd::entered;
                        return success();
                    }
                }
            }
            if (entry.NextEntryOffset == 0) break;
            offset = offset + static_cast<usize>(entry.NextEntryOffset);
        }
    }
}

} // namespace

Outcome remove_tree(const char* const name, const usize depth_limit, const usize entries_limit, u64& removed) noexcept {
    Growable<wchar_t> path;
    if (!wide_name(name, path)) return no_memory();
    {
        // The top is looked up by name (files.hpp): anything but a directory
        // is removed as remove removes it.
        const OwnedHandle described(open_for_attributes(path.data(), true));
        if (!described.valid()) return last_error();
        bool directory = false;
        const Outcome judged = judge(described.get(), directory);
        if (!judged.ok()) return judged;
        if (!directory) {
            const Outcome single = remove(name);
            if (single.ok()) removed = removed + 1;
            return single;
        }
    }
    const HANDLE top = CreateFileW(path.data(), directory_access, held_share, nullptr, OPEN_EXISTING, tree_flags, nullptr);
    if (top == INVALID_HANDLE_VALUE) return last_error();
    Growable<HeldDirectory> held;
    HeldDirectory first{};
    first.handle = top;
    first.path_count = unit_count(path.data());
    if (!held.push(first)) {
        (void)CloseHandle(top);
        return no_memory();
    }
    for (;;) {
        PassEnd end = PassEnd::empty;
        const Outcome walked = walk_top(held, path, depth_limit, entries_limit, removed, end);
        if (!walked.ok()) {
            release_all(held);
            return walked;
        }
        if (end == PassEnd::entered) continue;
        const HeldDirectory emptied = held[held.count() - 1];
        held.shrink(held.count() - 1);
        const Outcome gone = dispose_and_close(emptied.handle);
        if (!gone.ok()) {
            release_all(held);
            return gone;
        }
        removed = removed + 1;
        if (held.empty()) return success();
        if (!count_removed(held[held.count() - 1], entries_limit)) {
            release_all(held);
            return failure(FileStatus::too_large);
        }
    }
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
// `permissions` names POSIX bits Windows does not have.
Outcome copy_file(const char* const from, const char* const to, const CopyMode mode,
                  const CopyPermissions permissions) noexcept {
    static_cast<void>(permissions);
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
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        // A volume's root has no "." or ".." entries (the audit's host
        // fact, A-stxlib-hosted-01-F9; not confirmed on a host here), so an
        // empty root matches nothing, and a search that matches nothing
        // fails with ERROR_FILE_NOT_FOUND ("FindFirstFileExW function",
        // Microsoft Learn, Return value). The name is then checked as a
        // directory, so a missing one stays not_found.
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) {
            pattern.shrink(separated ? pattern.count() - 2 : pattern.count() - 3);
            if (!utf16::terminate(pattern)) return no_memory();
            const DWORD attributes = GetFileAttributesW(pattern.data());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return success();
        }
        return from_error(error);
    }
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
        // STXlib's own bound, before allocating for the name (audit A, H8):
        // each UTF-16 unit is at least one byte of WTF-8, so a longer name
        // cannot become a Path of maximum_path_bytes, even after a "\\?\"
        // prefix of up to 8 units is removed.
        if (length > maximum_path_bytes + 8) return failure(FileStatus::too_large);
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
