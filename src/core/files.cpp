// Files and directories (include/stxlib/files.hpp): File, the whole-file
// operations and the directory operations over the family's backend
// (hosted/file_backend.hpp). Ported from stx_auxlib's src/hosted/files.cpp
// at b6e330a. What stx_auxlib took from std::filesystem is built here from
// the backend's primitives (create_directories, remove_tree, absolute) or is
// a primitive of its own; see files.hpp, "Changes from stx_auxlib".

#include "stxlib/files.hpp"
#include "stxlib/text.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/wtf8.hpp"
#include "hosted/file_backend.hpp"
#include "state/state.hpp"

#include <atomic>
#include <cstring>

namespace stxlib {

namespace detail {

class DirectoryListingBuilder final {
public:
    [[nodiscard]] static DirectoryListing make(Buffer<char> names, Buffer<DirectoryEntryRecord> entries,
                                               const usize unnamed) noexcept {
        DirectoryListing result;
        result.names_ = static_cast<Buffer<char>&&>(names);
        result.entries_ = static_cast<Buffer<DirectoryEntryRecord>&&>(entries);
        result.unnamed_ = unnamed;
        return result;
    }

};

class FileAccess final {
public:
    // The host handle of an open File (write_file's temporary).
    [[nodiscard]] static i64 handle(const File& file) noexcept {
        const i64 result = file.handle_;
        return result;
    }
};

} // namespace detail

namespace {

using file_backend::FileStatus;
using file_backend::Outcome;

ErrorRecord record_of(const Outcome outcome, const Operation operation) noexcept {
    ErrorKind kind = ErrorKind::io_failure;
    HostCode code = HostCode::none;
    switch (outcome.status) {
    case FileStatus::ok:
    case FileStatus::io_error:
        break;
    case FileStatus::not_found:
        kind = ErrorKind::not_found;
        break;
    case FileStatus::already_exists:
        kind = ErrorKind::already_exists;
        break;
    case FileStatus::permission_denied:
        kind = ErrorKind::permission_denied;
        break;
    case FileStatus::not_a_directory:
        kind = ErrorKind::invalid_argument;
        code = HostCode::not_a_directory;
        break;
    case FileStatus::is_a_directory:
        kind = ErrorKind::invalid_argument;
        code = HostCode::is_a_directory;
        break;
    case FileStatus::directory_not_empty:
        kind = ErrorKind::invalid_argument;
        code = HostCode::directory_not_empty;
        break;
    case FileStatus::no_space:
        kind = ErrorKind::limit_exceeded;
        code = HostCode::no_space;
        break;
    case FileStatus::too_large:
        kind = ErrorKind::limit_exceeded;
        code = HostCode::too_long;
        break;
    case FileStatus::name_too_long:
        kind = ErrorKind::limit_exceeded;
        code = HostCode::name_too_long;
        break;
    case FileStatus::invalid_name:
        kind = ErrorKind::invalid_argument;
        code = HostCode::invalid_name;
        break;
    case FileStatus::unsupported:
        kind = ErrorKind::unsupported;
        break;
    case FileStatus::cross_device:
        kind = ErrorKind::unsupported;
        code = HostCode::cross_device;
        break;
    case FileStatus::busy:
        kind = ErrorKind::limit_exceeded;
        code = HostCode::busy;
        break;
    case FileStatus::bad_handle:
        kind = ErrorKind::disposed;
        code = HostCode::closed_handle;
        break;
    case FileStatus::out_of_memory:
        kind = ErrorKind::out_of_memory;
        break;
    }
    const ErrorRecord result = host_failure::record(kind, operation, code, outcome.foreign);
    return result;
}

OperationStatus failed(const Outcome outcome, const Operation operation) noexcept {
    const OperationStatus result = OperationStatus::failure(record_of(outcome, operation));
    return result;
}

OperationStatus status_of(const Outcome outcome, const Operation operation) noexcept {
    if (outcome.ok()) return OperationStatus::success();
    const OperationStatus result = failed(outcome, operation);
    return result;
}

// The backend's name for `path` and a NUL (file_backend.hpp, "Names"):
// POSIX hosts take the host bytes the path spells (they have no drive
// roots, and a Path holding a surrogate that is not an escape spells none),
// Windows takes the WTF-8 text. Where the file system keeps only UTF-8
// names (file_backend::utf8_names_only), a path whose host bytes are not
// UTF-8 is refused as invalid_name, as APFS refuses it with EILSEQ. The
// bytes are checked, not the text: Path also escapes bytes that are UTF-8
// (':' and '\'), so a text that is not Unicode can still spell a UTF-8
// name (the second web run refused "back\slash" and "c:" when the text
// was checked).
Outcome native_name(const Path& path, Growable<char>& output) noexcept {
    output.clear();
    const PathText text = path.text();
    if constexpr (file_backend::byte_names) {
        if (path.drive() != 0) return file_backend::failure(FileStatus::unsupported);
        const wtf8::Converted converted = wtf8::unescape_posix(text.data(), text.count(), output);
        if (converted == wtf8::Converted::not_representable) return file_backend::failure(FileStatus::invalid_name);
        if (converted != wtf8::Converted::ok) return file_backend::failure(FileStatus::out_of_memory);
        if (file_backend::utf8_names_only && !text.is_unicode() &&
            validate_utf8(output.data(), output.count()).failed()) {
            return file_backend::failure(FileStatus::invalid_name);
        }
    } else if (!output.append(text.data(), text.count())) {
        return file_backend::failure(FileStatus::out_of_memory);
    }
    if (!output.push('\0')) return file_backend::failure(FileStatus::out_of_memory);
    return file_backend::success();
}

// A host name as a Path, losslessly (D13 a); invalid_name only when the
// name breaks Path's syntax in a way no escape covers (a ".." above a root
// in a link target, a ':' in a Windows name outside a drive root).
OperationResult<Path> from_host(const Growable<char>& text, const Operation operation) noexcept {
    OperationResult<Path> parsed = file_backend::byte_names ? Path::from_posix_name(text.data(), text.count())
                                                   : Path::parse_wtf8(text.data(), text.count());
    if (parsed.succeeded()) return parsed;
    if ((*parsed.error()).kind() == ErrorKind::out_of_memory) {
        return OperationResult<Path>::failure(record_of(file_backend::failure(FileStatus::out_of_memory), operation));
    }
    return OperationResult<Path>::failure(record_of(file_backend::failure(FileStatus::invalid_name), operation));
}

ErrorRecord bad_handle_record(const Operation operation) noexcept {
    const ErrorRecord result = record_of(file_backend::failure(FileStatus::bad_handle), operation);
    return result;
}

OperationStatus bad_handle(const Operation operation) noexcept {
    const OperationStatus result = OperationStatus::failure(bad_handle_record(operation));
    return result;
}

ErrorRecord record_of_status(const FileStatus status, const Operation operation) noexcept {
    const ErrorRecord result = record_of(file_backend::failure(status), operation);
    return result;
}

void append_hexadecimal(char* const output, const u64 value) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (usize index = 0; index < 16; ++index) {
        const unsigned shift = static_cast<unsigned>(60 - 4 * index);
        output[index] = digits[(value >> shift) & 0xfU];
    }
}

// A name for write_file's temporary file (".stx-write-<stamp>-<serial>",
// stx_auxlib's), unique within this process and unlikely to collide with
// another's: the interval clock and a serial in stxlib_state. The serial
// wraps after 2^64 writes, which only risks a collision create_new detects;
// it is part of a name, not an identity, so house style §3's rule that
// identity counters never wrap does not apply (audit A, C4).
struct TemporaryName final {
    char text[11 + 16 + 1 + 16]{};
};

TemporaryName temporary_name() noexcept {
    state::ProcessState& process = state::current_process();
    const u64 serial = process.temporary_serial.fetch_add(1, std::memory_order_relaxed);
    const u64 stamp = static_cast<u64>(IntervalTime::now().count_nanoseconds());
    TemporaryName result{};
    std::memcpy(result.text, ".stx-write-", 11);
    append_hexadecimal(result.text + 11, stamp);
    result.text[27] = '-';
    append_hexadecimal(result.text + 28, serial);
    return result;
}

// Byte order of names, compared as unsigned bytes (std::string's order).
bool name_less(const char* const names, const file_backend::RawEntry& left,
               const file_backend::RawEntry& right) noexcept {
    const usize shorter = left.count < right.count ? left.count : right.count;
    const int compared = shorter == 0 ? 0 : std::memcmp(names + left.offset, names + right.offset, shorter);
    if (compared != 0) return compared < 0;
    return left.count < right.count;
}

// Heap sort by name: O(n log n), in place, no allocation.
void sift_down(const char* const names, file_backend::RawEntry* const entries, usize root, const usize count) noexcept {
    for (;;) {
        const usize left = 2 * root + 1;
        if (left >= count) return;
        usize larger = left;
        if (left + 1 < count && name_less(names, entries[left], entries[left + 1])) larger = left + 1;
        if (!name_less(names, entries[root], entries[larger])) return;
        const file_backend::RawEntry held = entries[root];
        entries[root] = entries[larger];
        entries[larger] = held;
        root = larger;
    }
}

void sort_by_name(const char* const names, file_backend::RawEntry* const entries, const usize count) noexcept {
    if (count < 2) return;
    for (usize start = count / 2; start > 0; --start) sift_down(names, entries, start - 1, count);
    for (usize end = count - 1; end > 0; --end) {
        const file_backend::RawEntry held = entries[0];
        entries[0] = entries[end];
        entries[end] = held;
        sift_down(names, entries, 0, end);
    }
}

// True when WTF-8 bytes[0, count) is one Path segment (stx_auxlib's
// is_segment): not empty, ".", or "..", and without '/', '\', ':' or NUL.
// Every POSIX name passes once escaped; a Windows name fails only when it
// holds what Win32 names cannot (files.hpp, unnamed_entries).
bool is_segment(const char* const bytes, const usize count) noexcept {
    if (count == 0) return false;
    if (count == 1 && bytes[0] == '.') return false;
    if (count == 2 && bytes[0] == '.' && bytes[1] == '.') return false;
    for (usize index = 0; index < count; ++index) {
        const char byte = bytes[index];
        if (byte == '/' || byte == '\\' || byte == ':' || byte == '\0') return false;
    }
    return true;
}

} // namespace

// ---- File --------------------------------------------------------------------

// Each owner lets go of its handle before closing it (AGENTS.md, rule 1):
// close is a cancellation point on POSIX.
File::~File() {
    const i64 held = handle_;
    handle_ = closed;
    if (held != closed) (void)file_backend::close(held);
}

File::File(File&& other) noexcept : handle_(other.handle_), mode_(other.mode_) { other.handle_ = closed; }

File& File::operator=(File&& other) noexcept {
    if (this == &other) return *this;
    const i64 held = handle_;
    handle_ = other.handle_;
    mode_ = other.mode_;
    other.handle_ = closed;
    if (held != closed) (void)file_backend::close(held);
    return *this;
}

OperationResult<File> File::open(const Path& path, const FileMode mode) noexcept {
    const Operation operation = "stxlib.files.open";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<File>::failure(record_of(converted, operation));
    i64 handle = file_backend::no_handle;
    const Outcome opened = file_backend::open(name.data(), mode, handle);
    if (!opened.ok()) return OperationResult<File>::failure(record_of(opened, operation));
    OperationResult<File> result = OperationResult<File>::success(File(handle, mode));
    return result;
}

OperationResult<usize> File::read(const MutableView<Byte> output) noexcept {
    const Operation operation = "stxlib.files.read";
    if (!is_open()) return OperationResult<usize>::failure(bad_handle_record(operation));
    usize transferred = 0;
    const Outcome outcome = file_backend::read(handle_, output.data(), output.count(), transferred);
    if (!outcome.ok()) return OperationResult<usize>::failure(record_of(outcome, operation));
    return OperationResult<usize>::success(transferred);
}

OperationStatus File::write_all(const View<Byte> input) noexcept {
    const Operation operation = "stxlib.files.write";
    if (!is_open()) return bad_handle(operation);
    usize written = 0;
    while (written < input.count()) {
        usize transferred = 0;
        const Outcome outcome =
            file_backend::write(handle_, input.data() + written, input.count() - written, transferred);
        if (!outcome.ok()) return failed(outcome, operation);
        written = written + transferred;
    }
    return OperationStatus::success();
}

OperationResult<u64> File::size() noexcept {
    const Operation operation = "stxlib.files.size";
    if (!is_open()) return OperationResult<u64>::failure(bad_handle_record(operation));
    u64 bytes = 0;
    const Outcome outcome = file_backend::size(handle_, bytes);
    if (!outcome.ok()) return OperationResult<u64>::failure(record_of(outcome, operation));
    return OperationResult<u64>::success(bytes);
}

OperationResult<u64> File::position() noexcept {
    const Operation operation = "stxlib.files.position";
    if (!is_open()) return OperationResult<u64>::failure(bad_handle_record(operation));
    u64 offset = 0;
    const Outcome outcome = file_backend::position(handle_, offset);
    if (!outcome.ok()) return OperationResult<u64>::failure(record_of(outcome, operation));
    return OperationResult<u64>::success(offset);
}

OperationStatus File::seek(const u64 offset) noexcept {
    const Operation operation = "stxlib.files.seek";
    if (!is_open()) return bad_handle(operation);
    const OperationStatus result = status_of(file_backend::seek(handle_, offset), operation);
    return result;
}

OperationStatus File::sync(const Durability level) noexcept {
    const Operation operation = "stxlib.files.sync";
    if (!is_open()) return bad_handle(operation);
    const OperationStatus result = status_of(file_backend::sync(handle_, level), operation);
    return result;
}

OperationStatus File::close() noexcept {
    const Operation operation = "stxlib.files.close";
    if (!is_open()) return bad_handle(operation);
    const i64 handle = handle_;
    handle_ = closed;
    const OperationStatus result = status_of(file_backend::close(handle), operation);
    return result;
}

// ---- positioned I/O and the open file's metadata (M2 C2) ------------------------

namespace {

constexpr u64 largest_offset = static_cast<u64>(i64_max);

OperationStatus refused_state(const Operation operation) noexcept {
    const OperationStatus result = host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_state);
    return result;
}

} // namespace

OperationResult<usize> File::read_at(const u64 offset, const MutableView<Byte> output) noexcept {
    const Operation operation = "stxlib.files.read_at";
    if (!is_open()) return OperationResult<usize>::failure(bad_handle_record(operation));
    if (offset > largest_offset) return OperationResult<usize>::failure(record_of_status(FileStatus::too_large, operation));
    usize transferred = 0;
    const Outcome outcome = file_backend::read_at(handle_, offset, output.data(), output.count(), transferred);
    if (!outcome.ok()) return OperationResult<usize>::failure(record_of(outcome, operation));
    return OperationResult<usize>::success(transferred);
}

OperationStatus File::write_all_at(const u64 offset, const View<Byte> input) noexcept {
    const Operation operation = "stxlib.files.write_at";
    if (!is_open()) return bad_handle(operation);
    if (mode_ == FileMode::read || mode_ == FileMode::append) return refused_state(operation);
    if (offset > largest_offset || input.count() > largest_offset - offset) {
        return failed(file_backend::failure(FileStatus::too_large), operation);
    }
    usize written = 0;
    while (written < input.count()) {
        usize transferred = 0;
        const u64 at = offset + static_cast<u64>(written);
        const Outcome outcome =
            file_backend::write_at(handle_, at, input.data() + written, input.count() - written, transferred);
        if (!outcome.ok()) return failed(outcome, operation);
        written = written + transferred;
    }
    return OperationStatus::success();
}

OperationResult<FileInformation> File::information() noexcept {
    const Operation operation = "stxlib.files.information";
    if (!is_open()) return OperationResult<FileInformation>::failure(bad_handle_record(operation));
    FileInformation described{};
    const Outcome outcome = file_backend::information(handle_, described);
    if (!outcome.ok()) return OperationResult<FileInformation>::failure(record_of(outcome, operation));
    return OperationResult<FileInformation>::success(described);
}

OperationStatus File::set_modified(const WallTime modified) noexcept {
    const Operation operation = "stxlib.files.set_modified";
    if (!is_open()) return bad_handle(operation);
    if (mode_ == FileMode::read) return refused_state(operation);
    const OperationStatus result = status_of(file_backend::set_modified(handle_, modified), operation);
    return result;
}

OperationStatus File::set_size(const u64 bytes) noexcept {
    const Operation operation = "stxlib.files.set_size";
    if (!is_open()) return bad_handle(operation);
    if (mode_ == FileMode::read || mode_ == FileMode::append) return refused_state(operation);
    if (bytes > largest_offset) return failed(file_backend::failure(FileStatus::too_large), operation);
    const OperationStatus result = status_of(file_backend::set_size(handle_, bytes), operation);
    return result;
}

// ---- whole files -------------------------------------------------------------

// Reads into a growing buffer, toward `limit` as the file proves longer than
// its reported size. When the buffer is full it probes one byte into a
// local: none is the end (at the limit too, so a file of exactly `limit`
// bytes is read, as in stx_auxlib's read_file); one at the limit is
// too_large; one below it is growth, so the buffer grows and keeps the byte.
// The buffer is reserved at exactly the reported size and handed to the
// result without a copy (audit A, X5), so a file that keeps its size costs
// one block of its size and one extra read; one that grows while it is
// read costs the doubling's block, under twice the bytes read.
OperationResult<Buffer<Byte>> read_file(const Path& path, const usize limit) noexcept {
    const Operation operation = "stxlib.files.read_file";
    OperationResult<File> opened = File::open(path, FileMode::read);
    if (opened.failed()) return OperationResult<Buffer<Byte>>::failure(*opened.error());
    File& file = *opened.value();
    const OperationResult<u64> reported = file.size();
    if (reported.failed()) return OperationResult<Buffer<Byte>>::failure(*reported.error());
    const u64 bytes = *reported.value();
    if (bytes > limit) return OperationResult<Buffer<Byte>>::failure(record_of_status(FileStatus::too_large, operation));
    Growable<Byte> content;
    if (!content.reserve_exact(static_cast<usize>(bytes)) || !content.resize(static_cast<usize>(bytes))) {
        return OperationResult<Buffer<Byte>>::failure(record_of_status(FileStatus::out_of_memory, operation));
    }
    usize filled = 0;
    for (;;) {
        if (filled == content.count()) {
            // Full: one byte into a local tells the end of the file from
            // growth, so a file that kept its reported size ends here with
            // its exact block, never grown and copied (A-stxlib-core-01-F3).
            Byte probe[1]{};
            const OperationResult<usize> extra = file.read(MutableView<Byte>(probe));
            if (extra.failed()) return OperationResult<Buffer<Byte>>::failure(*extra.error());
            if (*extra.value() == 0) break;
            if (content.count() == limit) {
                return OperationResult<Buffer<Byte>>::failure(record_of_status(FileStatus::too_large, operation));
            }
            const usize current = content.count();
            usize doubled = 4096;
            if (current >= 2048) doubled = current > usize_max / 2 ? usize_max : current * 2;
            if (!content.resize(doubled < limit ? doubled : limit)) {
                return OperationResult<Buffer<Byte>>::failure(record_of_status(FileStatus::out_of_memory, operation));
            }
            content[filled] = probe[0];
            filled = filled + 1;
            continue;
        }
        const OperationResult<usize> chunk = file.read(MutableView<Byte>(content.data() + filled, content.count() - filled));
        if (chunk.failed()) return OperationResult<Buffer<Byte>>::failure(*chunk.error());
        if (*chunk.value() == 0) break;
        filled = filled + *chunk.value();
    }
    const OperationStatus closed = file.close();
    if (closed.failed()) return OperationResult<Buffer<Byte>>::failure(*closed.error());
    content.shrink(filled);
    return OperationResult<Buffer<Byte>>::success(content.take_as_buffer());
}

// Creates write_file's temporary file in `directory`, retrying on the rare
// name collision (16 attempts, stx_auxlib's); `temporary` receives its name.
OperationResult<File> open_temporary(const Path& directory, Growable<char>& temporary, const Operation operation) noexcept {
    constexpr int attempts = 16;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const TemporaryName name = temporary_name();
        OperationResult<Path> name_path = Path::parse_bytes(name.text, sizeof name.text);
        if (name_path.failed()) return OperationResult<File>::failure(*name_path.error());
        OperationResult<Path> candidate = directory.join(*name_path.value());
        if (candidate.failed()) return OperationResult<File>::failure(record_of_status(FileStatus::name_too_long, operation));
        const Outcome converted = native_name(*candidate.value(), temporary);
        if (!converted.ok()) return OperationResult<File>::failure(record_of(converted, operation));
        OperationResult<File> attempted = File::open(*candidate.value(), FileMode::create_new);
        const bool collided = attempted.failed() && (*attempted.error()).kind() == ErrorKind::already_exists;
        if (!collided) return attempted;
    }
    return OperationResult<File>::failure(record_of_status(FileStatus::already_exists, operation));
}

// Writes, flushes (as `level` says) and closes the temporary file, then
// renames it over the target. The file is closed on every path.
OperationStatus write_and_replace(File& file, const View<Byte> content, const char* const temporary, const char* const target,
                         const Durability level, const Operation operation) noexcept {
    // The replaced file's permission bits, before any content is written
    // (A-stxlib-core-01-F4): a 0600 file stays 0600.
    const Outcome carried = file_backend::carry_permissions(detail::FileAccess::handle(file), target);
    if (!carried.ok()) {
        (void)file.close();
        return failed(carried, operation);
    }
    const OperationStatus written = file.write_all(content);
    if (written.failed()) {
        (void)file.close();
        return written;
    }
    const OperationStatus synced = file.sync(level);
    if (synced.failed()) {
        (void)file.close();
        return synced;
    }
    const OperationStatus closed = file.close();
    if (closed.failed()) return closed;
    const OperationStatus result = status_of(file_backend::replace(temporary, target, level), operation);
    return result;
}

OperationStatus write_file(const Path& path, const View<Byte> content, const Durability level) noexcept {
    const Operation operation = "stxlib.files.write_file";
    if (path.file_name().empty()) return failed(file_backend::failure(FileStatus::invalid_name), operation);
    Growable<char> target;
    const Outcome converted = native_name(path, target);
    if (!converted.ok()) return failed(converted, operation);
    OperationResult<Path> directory = path.parent();
    if (directory.failed()) return OperationStatus::failure(*directory.error());
    Growable<char> temporary;
    OperationResult<File> opened = open_temporary(*directory.value(), temporary, operation);
    if (opened.failed()) return OperationStatus::failure(*opened.error());
    const OperationStatus result =
        write_and_replace(*opened.value(), content, temporary.data(), target.data(), level, operation);
    if (result.failed()) (void)file_backend::remove(temporary.data());
    return result;
}

// ---- metadata and directories ------------------------------------------------

OperationResult<FileInformation> inspect(const Path& path, const LinkMode links) noexcept {
    const Operation operation = "stxlib.files.inspect";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<FileInformation>::failure(record_of(converted, operation));
    FileInformation information{};
    const Outcome outcome = file_backend::inspect(name.data(), links, information);
    if (!outcome.ok()) return OperationResult<FileInformation>::failure(record_of(outcome, operation));
    return OperationResult<FileInformation>::success(information);
}

OperationStatus create_directory(const Path& path) noexcept {
    const Operation operation = "stxlib.files.create_directory";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return failed(converted, operation);
    const OperationStatus result = status_of(file_backend::create_directory(name.data()), operation);
    return result;
}

// Creates each prefix of the path that ends at a separator, then the whole
// path, accepting the ones that exist; then checks that the whole names a
// directory (std::filesystem::create_directories' outcome).
OperationStatus create_directories(const Path& path) noexcept {
    const Operation operation = "stxlib.files.create_directories";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return failed(converted, operation);
    const usize count = name.count() - 1;
    const usize root = path.is_absolute() ? (path.drive() != 0 ? 3 : 1) : 0;
    Growable<char> prefix;
    for (usize end = root + 1; end <= count; ++end) {
        if (end != count && name[end] != '/') continue;
        prefix.clear();
        if (!prefix.append(name.data(), end) || !prefix.push('\0')) {
            return failed(file_backend::failure(FileStatus::out_of_memory), operation);
        }
        const Outcome created = file_backend::create_directory(prefix.data());
        if (!created.ok() && created.status != FileStatus::already_exists) return failed(created, operation);
    }
    FileInformation information{};
    const Outcome described = file_backend::inspect(name.data(), LinkMode::follow, information);
    if (!described.ok()) return failed(described, operation);
    if (information.kind != FileKind::directory) {
        return failed(file_backend::failure(FileStatus::not_a_directory), operation);
    }
    return OperationStatus::success();
}

OperationStatus remove(const Path& path) noexcept {
    const Operation operation = "stxlib.files.remove";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return failed(converted, operation);
    const OperationStatus result = status_of(file_backend::remove(name.data()), operation);
    return result;
}

// The traversal is the backend's (file_backend::remove_tree): relative to
// each directory's own descriptor or held handle, so only `path` is looked up
// by name (files.hpp states the safety and the bounds).
OperationResult<u64> remove_tree(const Path& path, const RemoveTreeLimits& limits) noexcept {
    const Operation operation = "stxlib.files.remove_tree";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<u64>::failure(record_of(converted, operation));
    u64 removed = 0;
    const Outcome outcome = file_backend::remove_tree(name.data(), limits.depth, limits.entries_per_directory, removed);
    if (outcome.status == FileStatus::not_found && removed == 0) return OperationResult<u64>::success(0);
    if (!outcome.ok()) return OperationResult<u64>::failure(record_of(outcome, operation));
    return OperationResult<u64>::success(removed);
}

OperationStatus rename(const Path& from, const Path& to) noexcept {
    const Operation operation = "stxlib.files.rename";
    Growable<char> source;
    Growable<char> target;
    Outcome converted = native_name(from, source);
    if (converted.ok()) converted = native_name(to, target);
    if (!converted.ok()) return failed(converted, operation);
    const OperationStatus result = status_of(file_backend::rename(source.data(), target.data()), operation);
    return result;
}

OperationStatus copy_file(const Path& from, const Path& to, const CopyMode mode, const CopyPermissions permissions) noexcept {
    const Operation operation = "stxlib.files.copy_file";
    Growable<char> source;
    Growable<char> target;
    Outcome converted = native_name(from, source);
    if (converted.ok()) converted = native_name(to, target);
    if (!converted.ok()) return failed(converted, operation);
    const OperationStatus result = status_of(file_backend::copy_file(source.data(), target.data(), mode, permissions), operation);
    return result;
}

PathText DirectoryListing::name(const usize index) const& noexcept {
    const detail::DirectoryEntryRecord& entry = entries_[index];
    // Only well-formed WTF-8 segments are listed (list_directory makes them).
    const PathText result =
        detail::PathTextAccess::adopt_validated(names_.view().data() + entry.name_offset, entry.name_count);
    return result;
}

FileKind DirectoryListing::kind(const usize index) const noexcept {
    const FileKind result = entries_[index].kind;
    return result;
}

OperationResult<DirectoryListing> list_directory(const Path& path, const usize limit) noexcept {
    const Operation operation = "stxlib.files.list_directory";
    const Outcome no_memory = file_backend::failure(FileStatus::out_of_memory);
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<DirectoryListing>::failure(record_of(converted, operation));
    Growable<char> host_names;
    Growable<file_backend::RawEntry> host_entries;
    const Outcome listed = file_backend::list(name.data(), limit, host_names, host_entries);
    if (!listed.ok()) return OperationResult<DirectoryListing>::failure(record_of(listed, operation));
    // Each name as Path text: POSIX bytes escaped, Windows' WTF-8 as it is.
    // Keep the valid segments.
    Growable<char> names;
    Growable<file_backend::RawEntry> raw;
    usize unnamed = 0;
    usize kept_bytes = 0;
    for (usize index = 0; index < host_entries.count(); ++index) {
        const file_backend::RawEntry entry = host_entries[index];
        const usize offset = names.count();
        const char* const host = host_names.data() + entry.offset;
        const bool appended = file_backend::byte_names ? wtf8::escape_posix(host, entry.count, names)
                                                       : names.append(host, entry.count);
        if (!appended) return OperationResult<DirectoryListing>::failure(record_of(no_memory, operation));
        const usize count = names.count() - offset;
        if (!is_segment(names.data() + offset, count)) {
            names.shrink(offset);
            unnamed = unnamed + 1;
            continue;
        }
        if (!raw.push(file_backend::RawEntry{offset, count, entry.kind})) {
            return OperationResult<DirectoryListing>::failure(record_of(no_memory, operation));
        }
        kept_bytes = kept_bytes + count;
    }
    const usize kept = raw.count();
    sort_by_name(names.data(), raw.data(), raw.count());
    OperationResult<Buffer<char>> name_bytes = Buffer<char>::allocate_zeroed(kept_bytes);
    OperationResult<Buffer<detail::DirectoryEntryRecord>> records = Buffer<detail::DirectoryEntryRecord>::allocate_zeroed(kept);
    if (name_bytes.failed() || records.failed()) return OperationResult<DirectoryListing>::failure(record_of(no_memory, operation));
    Buffer<char>& name_buffer = *name_bytes.value();
    Buffer<detail::DirectoryEntryRecord>& record_buffer = *records.value();
    usize offset = 0;
    for (usize index = 0; index < kept; ++index) {
        const file_backend::RawEntry entry = raw[index];
        if (entry.count != 0) std::memcpy(name_buffer.mutable_view().data() + offset, names.data() + entry.offset, entry.count);
        record_buffer[index] = detail::DirectoryEntryRecord{offset, entry.count, entry.kind};
        offset = offset + entry.count;
    }
    DirectoryListing listing = detail::DirectoryListingBuilder::make(
        static_cast<OperationResult<Buffer<char>>&&>(name_bytes).take_value(),
        static_cast<OperationResult<Buffer<detail::DirectoryEntryRecord>>&&>(records).take_value(), unnamed);
    OperationResult<DirectoryListing> result = OperationResult<DirectoryListing>::success(static_cast<DirectoryListing&&>(listing));
    return result;
}

OperationResult<Path> temporary_directory() noexcept {
    const Operation operation = "stxlib.files.temporary_directory";
    Growable<char> text;
    const Outcome outcome = file_backend::temporary_directory(text);
    if (!outcome.ok()) return OperationResult<Path>::failure(record_of(outcome, operation));
    OperationResult<Path> path = from_host(text, operation);
    if (path.failed()) return path;
    Growable<char> name;
    const Outcome converted = native_name(*path.value(), name);
    if (!converted.ok()) return OperationResult<Path>::failure(record_of(converted, operation));
    FileInformation information{};
    const Outcome described = file_backend::inspect(name.data(), LinkMode::follow, information);
    if (!described.ok()) return OperationResult<Path>::failure(record_of(described, operation));
    if (information.kind != FileKind::directory) {
        return OperationResult<Path>::failure(record_of(file_backend::failure(FileStatus::not_a_directory), operation));
    }
    return path;
}

OperationResult<Path> working_directory() noexcept {
    const Operation operation = "stxlib.files.working_directory";
    Growable<char> text;
    const Outcome outcome = file_backend::working_directory(text);
    if (!outcome.ok()) return OperationResult<Path>::failure(record_of(outcome, operation));
    OperationResult<Path> result = from_host(text, operation);
    return result;
}

OperationResult<Path> absolute(const Path& path) noexcept {
    if (path.is_absolute()) {
        OperationResult<Path> same = path.copy();
        return same;
    }
    OperationResult<Path> working = working_directory();
    if (working.failed()) return working;
    OperationResult<Path> result = (*working.value()).join(path);
    return result;
}

OperationResult<Path> resolve(const Path& path) noexcept {
    const Operation operation = "stxlib.files.resolve";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<Path>::failure(record_of(converted, operation));
    Growable<char> text;
    const Outcome outcome = file_backend::resolve(name.data(), text);
    if (!outcome.ok()) return OperationResult<Path>::failure(record_of(outcome, operation));
    OperationResult<Path> result = from_host(text, operation);
    return result;
}

OperationResult<Path> read_link(const Path& path) noexcept {
    const Operation operation = "stxlib.files.read_link";
    Growable<char> name;
    const Outcome converted = native_name(path, name);
    if (!converted.ok()) return OperationResult<Path>::failure(record_of(converted, operation));
    Growable<char> text;
    const Outcome outcome = file_backend::read_link(name.data(), text);
    if (!outcome.ok()) return OperationResult<Path>::failure(record_of(outcome, operation));
    OperationResult<Path> result = from_host(text, operation);
    return result;
}

// A relative target is resolved against the link's directory, as the host
// will, to tell whether it names a directory (stx_auxlib's create_link).
OperationStatus create_link(const Path& target, const Path& link) noexcept {
    const Operation operation = "stxlib.files.create_link";
    Growable<char> native_target;
    Growable<char> native_link;
    Outcome converted = native_name(target, native_target);
    if (converted.ok()) converted = native_name(link, native_link);
    if (!converted.ok()) return failed(converted, operation);
    bool to_directory = false;
    OperationResult<Path> link_directory = link.parent();
    if (link_directory.failed()) return OperationStatus::failure(*link_directory.error());
    OperationResult<Path> resolved = target.is_absolute() ? target.copy() : (*link_directory.value()).join(target);
    if (resolved.succeeded()) {
        Growable<char> resolved_name;
        FileInformation information{};
        if (native_name(*resolved.value(), resolved_name).ok() &&
            file_backend::inspect(resolved_name.data(), LinkMode::follow, information).ok()) {
            to_directory = information.kind == FileKind::directory;
        }
    }
    const OperationStatus result =
        status_of(file_backend::create_link(native_target.data(), native_link.data(), to_directory), operation);
    return result;
}

} // namespace stxlib
