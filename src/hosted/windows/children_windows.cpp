// Child processes on Windows (include/stxlib/children.hpp), over
// CreateProcessW. Ported from stx_auxlib's src/hosted/children_windows.cpp
// at b6e330a. The command line is built with the quoting rules the C runtime
// and CommandLineToArgvW parse, so the child receives exactly the requested
// arguments. Only the handles a child should have are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). Windows has no zombies: closing the
// process handle releases a child, which keeps running.
//
// The program (security audit 2026-11-15, A-stxlib-hosted-01-F1 and F2).
// CreateProcessW is always given the program's absolute name as
// lpApplicationName, never a null one: with a null name it searches for the
// command line's first token in the application's directory, then the
// current directory, then the system directories and PATH (Microsoft's
// CreateProcessW page, lpCommandLine), so a file planted in the current
// directory would run. A bare name (no '\', '/' or ':') is looked up with
// SearchPathW over PATH's absolute entries only (a relative entry such as
// "." would be read against the current directory); any other name is made
// absolute with GetFullPathNameW. ".exe" is added to a last component with
// no extension, as CreateProcessW does for the first token. A ':' anywhere
// but after a leading drive letter (an alternate data stream, a device) is
// refused.
//
// A program whose name ends in .bat or .cmd (any case, with the trailing
// dots and spaces Windows ignores) is refused with invalid_argument:
// CreateProcessW runs such a file through cmd.exe /c, which parses the
// whole command line by its own rules (&, |, <, >, ^, %NAME% and quotes),
// and the C runtime's quoting cannot make an argument safe there (the
// "BatBadBut" advisories, CVE-2024-24576, Rust's standard library fixed it
// by quoting for cmd.exe and refusing what cannot be quoted; STXlib refuses
// the program instead). children.hpp states the rule.
//
// Changes from stx_auxlib: the child state is inline in Child (no heap
// block, so no child is terminated for want of memory after it started);
// the command line and attribute list are Growable buffers.

#include "stxlib/children.hpp"

#include "core/growable.hpp"
#include "core/host_failure.hpp"
#include "core/wtf8.hpp"
#include "hosted/children_common.hpp"
#include "hosted/windows/utf16.hpp"

#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace stxlib {

namespace {

ErrorRecord record_of_error(const DWORD error, const Operation operation) noexcept {
    ErrorKind kind = ErrorKind::io_failure;
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_BAD_EXE_FORMAT:
        kind = ErrorKind::not_found;
        break;
    case ERROR_ACCESS_DENIED:
        kind = ErrorKind::permission_denied;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        kind = ErrorKind::out_of_memory;
        break;
    case ERROR_NO_SYSTEM_RESOURCES:
        kind = ErrorKind::limit_exceeded;
        break;
    default:
        break;
    }
    const ErrorRecord result = host_failure::record(
        kind, operation, HostCode::none, ForeignCode::of(ForeignFamily::windows_last_error, static_cast<i64>(error)));
    return result;
}

ErrorRecord last_error(const Operation operation) noexcept {
    const ErrorRecord result = record_of_error(GetLastError(), operation);
    return result;
}

HANDLE handle_of(const i64 value) noexcept {
    const HANDLE result = reinterpret_cast<HANDLE>(static_cast<intptr_t>(value));
    return result;
}

i64 value_of(const HANDLE handle) noexcept {
    const i64 result = static_cast<i64>(reinterpret_cast<intptr_t>(handle));
    return result;
}

// Appends one argument quoted for CommandLineToArgvW: wrapped in quotes when
// it is empty or holds a space, tab or quote; a quote is escaped with a
// backslash, and backslashes are doubled only where they precede a quote
// (stx_auxlib's append_quoted). The argument is WTF-8 (children.hpp): an
// unpaired surrogate becomes that UTF-16 unit, as Path::windows_name makes it.
bool append_quoted(Growable<wchar_t>& line, const PathText argument) noexcept {
    Growable<wchar_t> wide;
    if (!wtf8::to_utf16(argument.data(), argument.count(), wide)) return false;
    bool needs_quotes = wide.empty();
    for (usize index = 0; index < wide.count(); ++index) {
        const wchar_t unit = wide[index];
        if (unit == L' ' || unit == L'\t' || unit == L'"') needs_quotes = true;
    }
    if (!needs_quotes) {
        const bool result = line.append(wide.data(), wide.count());
        return result;
    }
    if (!line.push(L'"')) return false;
    usize backslashes = 0;
    for (usize index = 0; index < wide.count(); ++index) {
        const wchar_t unit = wide[index];
        if (unit == L'\\') {
            backslashes = backslashes + 1;
            continue;
        }
        const usize escapes = unit == L'"' ? backslashes * 2 + 1 : backslashes;
        for (usize count = 0; count < escapes; ++count) {
            if (!line.push(L'\\')) return false;
        }
        backslashes = 0;
        if (!line.push(unit)) return false;
    }
    for (usize count = 0; count < backslashes * 2; ++count) {
        if (!line.push(L'\\')) return false;
    }
    const bool result = line.push(L'"');
    return result;
}

// Whether `name`'s last component ends in ".bat" or ".cmd", ASCII case
// ignored, after the trailing dots and spaces Windows drops from a file name.
bool names_batch_file(const wchar_t* const name, const usize count) noexcept {
    usize end = count;
    while (end > 0 && (name[end - 1] == L'.' || name[end - 1] == L' ')) end = end - 1;
    if (end < 4 || name[end - 4] != L'.') return false;
    wchar_t lowered[3]{};
    for (usize index = 0; index < 3; ++index) {
        const wchar_t unit = name[end - 3 + index];
        lowered[index] = unit >= L'A' && unit <= L'Z' ? static_cast<wchar_t>(unit - L'A' + L'a') : unit;
    }
    const bool bat = lowered[0] == L'b' && lowered[1] == L'a' && lowered[2] == L't';
    const bool cmd = lowered[0] == L'c' && lowered[1] == L'm' && lowered[2] == L'd';
    return bat || cmd;
}

// Whether `entry` (a PATH entry, unquoted) is absolute: a drive root
// ("C:\", "C:/") or a UNC name ("\\server").
bool absolute_entry(const wchar_t* const entry, const usize count) noexcept {
    const bool drive = count >= 3 && ((entry[0] >= L'A' && entry[0] <= L'Z') || (entry[0] >= L'a' && entry[0] <= L'z')) &&
                       entry[1] == L':' && (entry[2] == L'\\' || entry[2] == L'/');
    const bool unc = count >= 2 && (entry[0] == L'\\' || entry[0] == L'/') && (entry[1] == L'\\' || entry[1] == L'/');
    return drive || unc;
}

enum class Resolved : u8 {
    ok = 0,
    host_error = 1,     // GetLastError says why
    refused = 2,        // a batch file or a ':' out of place
    out_of_memory = 3,
};

// PATH's absolute entries, ';'-separated and terminated; empty (no
// terminator) when there are none.
Resolved absolute_search_path(Growable<wchar_t>& output) noexcept {
    output.clear();
    const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (needed == 0) return Resolved::ok;
    Growable<wchar_t> path;
    if (!path.resize(static_cast<usize>(needed))) return Resolved::out_of_memory;
    const DWORD got = GetEnvironmentVariableW(L"PATH", path.data(), needed);
    if (got == 0 || got >= needed) return Resolved::ok;
    usize start = 0;
    const usize length = static_cast<usize>(got);
    while (start <= length) {
        usize stop = start;
        while (stop < length && path[stop] != L';') stop = stop + 1;
        usize first = start;
        usize last = stop;
        if (last - first >= 2 && path[first] == L'"' && path[last - 1] == L'"') {
            first = first + 1;
            last = last - 1;
        }
        if (absolute_entry(path.data() + first, last - first)) {
            if (!output.empty() && !output.push(L';')) return Resolved::out_of_memory;
            if (!output.append(path.data() + first, last - first)) return Resolved::out_of_memory;
        }
        start = stop + 1;
    }
    if (!output.empty() && !utf16::terminate(output)) return Resolved::out_of_memory;
    return Resolved::ok;
}

// The program's absolute, terminated name for lpApplicationName (the file
// comment says how it is found).
Resolved resolve_program(const PathText program, Growable<wchar_t>& resolved) noexcept {
    resolved.clear();
    Growable<wchar_t> name;
    if (!wtf8::to_utf16(program.data(), program.count(), name)) return Resolved::out_of_memory;
    bool bare = true;
    usize last_component = 0;
    for (usize index = 0; index < name.count(); ++index) {
        if (name[index] == L'/') name[index] = L'\\';
        if (name[index] == L'\\') {
            bare = false;
            last_component = index + 1;
        }
        if (name[index] == L':') {
            const wchar_t first = name[0];
            const bool drive_letter = (first >= L'A' && first <= L'Z') || (first >= L'a' && first <= L'z');
            if (index != 1 || !drive_letter) return Resolved::refused;
            bare = false;
            last_component = 2;
        }
    }
    bool has_extension = false;
    for (usize index = last_component; index < name.count(); ++index) {
        if (name[index] == L'.') has_extension = true;
    }
    if (!has_extension && !name.append(L".exe", 4)) return Resolved::out_of_memory;
    if (!utf16::terminate(name)) return Resolved::out_of_memory;
    if (bare) {
        Growable<wchar_t> search;
        const Resolved listed = absolute_search_path(search);
        if (listed != Resolved::ok) return listed;
        if (search.empty()) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return Resolved::host_error;
        }
        const DWORD needed = SearchPathW(search.data(), name.data(), nullptr, 0, nullptr, nullptr);
        if (needed == 0) return Resolved::host_error;
        if (!resolved.resize(static_cast<usize>(needed))) return Resolved::out_of_memory;
        const DWORD got = SearchPathW(search.data(), name.data(), nullptr, needed, resolved.data(), nullptr);
        if (got == 0) return Resolved::host_error;
        if (got >= needed) {
            SetLastError(ERROR_FILE_NOT_FOUND); // PATH changed between the two calls
            return Resolved::host_error;
        }
        if (!resolved.resize(static_cast<usize>(got))) return Resolved::out_of_memory;
    } else {
        const DWORD needed = GetFullPathNameW(name.data(), 0, nullptr, nullptr);
        if (needed == 0) return Resolved::host_error;
        if (!resolved.resize(static_cast<usize>(needed))) return Resolved::out_of_memory;
        const DWORD got = GetFullPathNameW(name.data(), needed, resolved.data(), nullptr);
        if (got == 0 || got >= needed) return Resolved::host_error;
        if (!resolved.resize(static_cast<usize>(got))) return Resolved::out_of_memory;
    }
    if (names_batch_file(resolved.data(), resolved.count())) return Resolved::refused;
    if (!utf16::terminate(resolved)) return Resolved::out_of_memory;
    return Resolved::ok;
}

// Closes a handle on every exit; null holds none.
class OwnedHandle final {
public:
    OwnedHandle() noexcept = default;
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    ~OwnedHandle() {
        // Lets go before closing (AGENTS.md, rule 1).
        const HANDLE held = value;
        value = nullptr;
        if (held != nullptr && held != INVALID_HANDLE_VALUE) (void)CloseHandle(held);
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE result = value;
        value = nullptr;
        return result;
    }

    HANDLE value{nullptr};
};

// Owns an initialised attribute list.
class AttributeList final {
public:
    AttributeList() noexcept = default;
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;
    ~AttributeList() {
        if (ready) DeleteProcThreadAttributeList(list());
    }

    [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST list() noexcept {
        const LPPROC_THREAD_ATTRIBUTE_LIST result = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        return result;
    }

    Growable<Byte> storage;
    bool ready{false};
};

} // namespace

void Child::release() noexcept {
    // Lets go of each handle before closing it (AGENTS.md, rule 1).
    const i64 output = output_;
    const i64 process = process_;
    output_ = none;
    process_ = none;
    phase_ = Phase::empty;
    if (output != none) (void)CloseHandle(handle_of(output));
    if (process != none) (void)CloseHandle(handle_of(process));
}

OperationResult<Child> Child::start(const ChildRequest& request) noexcept {
    const Operation operation = "stxlib.children.start";
    const OperationStatus checked = children_detail::check_request(request);
    if (checked.failed()) return OperationResult<Child>::failure(*checked.error());
    const ErrorRecord exhausted = host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none);
    Growable<wchar_t> application;
    const Resolved found = resolve_program(request.program, application);
    if (found == Resolved::out_of_memory) return OperationResult<Child>::failure(exhausted);
    if (found == Resolved::host_error) return OperationResult<Child>::failure(last_error(operation));
    if (found == Resolved::refused) {
        return OperationResult<Child>::failure(host_failure::record(ErrorKind::invalid_argument, operation,
                                                                    HostCode::invalid_value));
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    OwnedHandle null_input;
    null_input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                   OPEN_EXISTING, 0, nullptr);
    if (null_input.value == INVALID_HANDLE_VALUE) return OperationResult<Child>::failure(last_error(operation));
    OwnedHandle output_reader;
    OwnedHandle output_writer;
    if (request.streams == ChildStreams::capture) {
        HANDLE reader = nullptr;
        HANDLE writer = nullptr;
        if (!CreatePipe(&reader, &writer, &inheritable,
                        0)) return OperationResult<Child>::failure(last_error(operation));
        output_reader.value = reader;
        output_writer.value = writer;
        (void)SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    } else if (request.streams == ChildStreams::discard) {
        output_writer.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                          OPEN_EXISTING, 0, nullptr);
        if (output_writer.value == INVALID_HANDLE_VALUE) return OperationResult<Child>::failure(last_error(operation));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    const bool inherit = request.streams == ChildStreams::inherit;
    // Inherit mode (A-stxlib-hosted-01-F3). With PROC_THREAD_ATTRIBUTE_HANDLE_LIST
    // only the listed handles are inherited (Microsoft's
    // UpdateProcThreadAttribute page), so the parent's output and error are
    // duplicated as inheritable and listed, as Python's subprocess and Go's
    // os/exec do; the duplicates are closed when start returns, the child
    // having its own. A missing or unduplicable standard handle is passed
    // as it is and not listed (the child then has none, as before).
    OwnedHandle inherited_output;
    OwnedHandle inherited_error;
    HANDLE child_output = output_writer.value;
    HANDLE child_error = output_writer.value;
    if (inherit) {
        const HANDLE own_output = GetStdHandle(STD_OUTPUT_HANDLE);
        const HANDLE own_error = GetStdHandle(STD_ERROR_HANDLE);
        child_output = own_output;
        child_error = own_error;
        const HANDLE self = GetCurrentProcess();
        HANDLE duplicate = nullptr;
        if (own_output != nullptr && own_output != INVALID_HANDLE_VALUE &&
            DuplicateHandle(self, own_output, self, &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            inherited_output.value = duplicate;
            child_output = duplicate;
        }
        if (own_error == own_output) {
            child_error = child_output;
        } else if (own_error != nullptr && own_error != INVALID_HANDLE_VALUE &&
                   DuplicateHandle(self, own_error, self, &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            inherited_error.value = duplicate;
            child_error = duplicate;
        }
    }
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = child_error;
    // Restrict inheritance to the child's own standard handles, each listed
    // once.
    HANDLE handles[3] = {null_input.value, nullptr, nullptr};
    usize handle_count = 1;
    if (!inherit) {
        handles[handle_count] = output_writer.value;
        handle_count = handle_count + 1;
    } else {
        if (inherited_output.value != nullptr) {
            handles[handle_count] = inherited_output.value;
            handle_count = handle_count + 1;
        }
        if (inherited_error.value != nullptr) {
            handles[handle_count] = inherited_error.value;
            handle_count = handle_count + 1;
        }
    }
    AttributeList attributes;
    SIZE_T list_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &list_bytes);
    if (!attributes.storage.resize(list_bytes)) return OperationResult<Child>::failure(exhausted);
    if (!InitializeProcThreadAttributeList(attributes.list(), 1, 0, &list_bytes)) {
        return OperationResult<Child>::failure(last_error(operation));
    }
    attributes.ready = true;
    if (!UpdateProcThreadAttribute(attributes.list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   handle_count * sizeof(HANDLE), nullptr, nullptr)) {
        return OperationResult<Child>::failure(last_error(operation));
    }
    startup.lpAttributeList = attributes.list();

    Growable<wchar_t> command_line;
    if (!append_quoted(command_line, request.program)) return OperationResult<Child>::failure(exhausted);
    for (usize index = 0; index < request.arguments.count(); ++index) {
        if (!command_line.push(L' ') || !append_quoted(command_line, request.arguments[index])) {
            return OperationResult<Child>::failure(exhausted);
        }
    }
    if (!utf16::terminate(command_line)) return OperationResult<Child>::failure(exhausted);
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (request.detached) flags = flags | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    PROCESS_INFORMATION information{};
    // The resolved absolute name, so CreateProcessW searches nothing (the
    // file comment); the command line still begins with the program as given.
    if (!CreateProcessW(application.data(), command_line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &startup.StartupInfo, &information)) {
        return OperationResult<Child>::failure(last_error(operation));
    }
    (void)CloseHandle(information.hThread);
    Child child;
    child.process_ = value_of(information.hProcess);
    child.phase_ = Phase::running;
    child.output_ = request.streams == ChildStreams::capture ? value_of(output_reader.release()) : none;
    OperationResult<Child> result = OperationResult<Child>::success(static_cast<Child&&>(child));
    return result;
}

OperationResult<ChildEnd> Child::wait(const Duration timeout) noexcept {
    const Operation operation = "stxlib.children.wait";
    if (phase_ != Phase::running) return OperationResult<ChildEnd>::failure(children_detail::invalid_state(operation));
    // timed_out means IntervalTime::now() has reached the deadline
    // (children.hpp), as on POSIX hosts. WaitForSingleObject's interval is
    // whole milliseconds counted on the system timer, whose accuracy
    // "depends on the resolution of the system clock" (Microsoft's
    // WaitForSingleObject page; from the published page, not re-read), so
    // it can end before IntervalTime (QueryPerformanceCounter) has seen the
    // interval pass: CI run 43432600851 saw a 50 ms wait end early. The wait
    // is therefore repeated, rounded up to whole milliseconds, until the
    // deadline has passed on IntervalTime.
    const IntervalTime deadline = IntervalTime::now().after(timeout.negative() ? Duration() : timeout);
    DWORD outcome = WAIT_TIMEOUT;
    for (;;) {
        const IntervalTime now = IntervalTime::now();
        const Duration left = now < deadline ? deadline.since(now) : Duration();
        const i64 nanoseconds = left.count_nanoseconds();
        const i64 whole = nanoseconds / 1000000;
        const i64 rounded = nanoseconds % 1000000 == 0 ? whole : whole + 1;
        constexpr i64 longest = static_cast<i64>(INFINITE) - 1;
        const DWORD wait_milliseconds = static_cast<DWORD>(rounded > longest ? longest : rounded);
        outcome = WaitForSingleObject(handle_of(process_), wait_milliseconds);
        if (outcome != WAIT_TIMEOUT) break;
        if (!(IntervalTime::now() < deadline)) {
            return OperationResult<ChildEnd>::failure(
                host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::timed_out));
        }
    }
    if (outcome != WAIT_OBJECT_0) return OperationResult<ChildEnd>::failure(last_error(operation));
    DWORD code = 0;
    if (!GetExitCodeProcess(handle_of(process_),
                            &code)) return OperationResult<ChildEnd>::failure(last_error(operation));
    phase_ = Phase::waited;
    return OperationResult<ChildEnd>::success(ChildEnd::exited(static_cast<u32>(code)));
}

OperationStatus Child::kill() noexcept {
    const Operation operation = "stxlib.children.kill";
    if (phase_ != Phase::running) return OperationStatus::failure(children_detail::invalid_state(operation));
    if (!TerminateProcess(handle_of(process_), 1)) return OperationStatus::failure(last_error(operation));
    return OperationStatus::success();
}

OperationResult<Buffer<Byte>> Child::read_output(const usize limit) noexcept {
    const Operation operation = "stxlib.children.read_output";
    if (output_ == none) return OperationResult<Buffer<Byte>>::failure(children_detail::invalid_state(operation));
    const HANDLE pipe = handle_of(output_);
    Growable<Byte> output;
    bool over_limit = false;
    Byte chunk[4096]{};
    DWORD failed_error = 0;
    bool exhausted = false;
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(pipe, chunk, static_cast<DWORD>(sizeof chunk), &count, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) failed_error = error;
            break;
        }
        if (count == 0) break;
        if (!children_detail::keep_output(chunk, static_cast<usize>(count), limit, output, over_limit)) {
            exhausted = true;
            break;
        }
    }
    output_ = none; // let go before closing (AGENTS.md, rule 1)
    (void)CloseHandle(pipe);
    if (failed_error != 0) return OperationResult<Buffer<Byte>>::failure(record_of_error(failed_error, operation));
    if (exhausted) {
        return OperationResult<Buffer<Byte>>::failure(host_failure::record(ErrorKind::out_of_memory, operation,
                                                                           HostCode::none));
    }
    OperationResult<Buffer<Byte>> result = children_detail::finish_output(output, over_limit, operation);
    return result;
}

} // namespace stxlib
