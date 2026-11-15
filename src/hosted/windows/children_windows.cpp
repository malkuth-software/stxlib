// Child processes on Windows (include/stxlib/children.hpp), over
// CreateProcessW. Ported from stx_auxlib's src/hosted/children_windows.cpp
// at b6e330a. The command line is built with the quoting rules the C runtime
// and CommandLineToArgvW parse, so the child receives exactly the requested
// arguments. Only the handles a child should have are inherited
// (PROC_THREAD_ATTRIBUTE_HANDLE_LIST). Windows has no zombies: closing the
// process handle releases a child, which keeps running.
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

Result<Child> Child::start(const ChildRequest& request) noexcept {
    const Operation operation = "stxlib.children.start";
    const Status checked = children_detail::check_request(request);
    if (checked.failed()) return Result<Child>::failure(*checked.error());
    const ErrorRecord exhausted = host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none);

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    OwnedHandle null_input;
    null_input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                   OPEN_EXISTING, 0, nullptr);
    if (null_input.value == INVALID_HANDLE_VALUE) return Result<Child>::failure(last_error(operation));
    OwnedHandle output_reader;
    OwnedHandle output_writer;
    if (request.streams == ChildStreams::capture) {
        HANDLE reader = nullptr;
        HANDLE writer = nullptr;
        if (!CreatePipe(&reader, &writer, &inheritable, 0)) return Result<Child>::failure(last_error(operation));
        output_reader.value = reader;
        output_writer.value = writer;
        (void)SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    } else if (request.streams == ChildStreams::discard) {
        output_writer.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                          OPEN_EXISTING, 0, nullptr);
        if (output_writer.value == INVALID_HANDLE_VALUE) return Result<Child>::failure(last_error(operation));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    const bool inherit = request.streams == ChildStreams::inherit;
    const HANDLE child_output = inherit ? GetStdHandle(STD_OUTPUT_HANDLE) : output_writer.value;
    const HANDLE child_error = inherit ? GetStdHandle(STD_ERROR_HANDLE) : output_writer.value;
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = child_error;
    // Restrict inheritance to the child's own standard handles. In inherit
    // mode only the null input is listed: the parent's own standard handles
    // are passed as they are.
    HANDLE handles[2] = {null_input.value, output_writer.value};
    const usize handle_count = inherit ? 1 : 2;
    AttributeList attributes;
    SIZE_T list_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &list_bytes);
    if (!attributes.storage.resize(list_bytes)) return Result<Child>::failure(exhausted);
    if (!InitializeProcThreadAttributeList(attributes.list(), 1, 0, &list_bytes)) {
        return Result<Child>::failure(last_error(operation));
    }
    attributes.ready = true;
    if (!UpdateProcThreadAttribute(attributes.list(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   handle_count * sizeof(HANDLE), nullptr, nullptr)) {
        return Result<Child>::failure(last_error(operation));
    }
    startup.lpAttributeList = attributes.list();

    Growable<wchar_t> command_line;
    if (!append_quoted(command_line, request.program)) return Result<Child>::failure(exhausted);
    for (usize index = 0; index < request.arguments.count(); ++index) {
        if (!command_line.push(L' ') || !append_quoted(command_line, request.arguments[index])) {
            return Result<Child>::failure(exhausted);
        }
    }
    if (!utf16::terminate(command_line)) return Result<Child>::failure(exhausted);
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (request.detached) flags = flags | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;
    PROCESS_INFORMATION information{};
    // A null application name makes CreateProcessW search PATH for the first
    // token of the (writable) command line.
    if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &startup.StartupInfo, &information)) {
        return Result<Child>::failure(last_error(operation));
    }
    (void)CloseHandle(information.hThread);
    Child child;
    child.process_ = value_of(information.hProcess);
    child.phase_ = Phase::running;
    child.output_ = request.streams == ChildStreams::capture ? value_of(output_reader.release()) : none;
    Result<Child> result = Result<Child>::success(static_cast<Child&&>(child));
    return result;
}

Result<ChildEnd> Child::wait(const Duration timeout) noexcept {
    const Operation operation = "stxlib.children.wait";
    if (phase_ != Phase::running) return Result<ChildEnd>::failure(children_detail::invalid_state(operation));
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
            return Result<ChildEnd>::failure(
                host_failure::record(ErrorKind::limit_exceeded, operation, HostCode::timed_out));
        }
    }
    if (outcome != WAIT_OBJECT_0) return Result<ChildEnd>::failure(last_error(operation));
    DWORD code = 0;
    if (!GetExitCodeProcess(handle_of(process_), &code)) return Result<ChildEnd>::failure(last_error(operation));
    phase_ = Phase::waited;
    return Result<ChildEnd>::success(ChildEnd::exited(static_cast<u32>(code)));
}

Status Child::kill() noexcept {
    const Operation operation = "stxlib.children.kill";
    if (phase_ != Phase::running) return Status::failure(children_detail::invalid_state(operation));
    if (!TerminateProcess(handle_of(process_), 1)) return Status::failure(last_error(operation));
    return Status::success();
}

Result<Buffer<Byte>> Child::read_output(const usize limit) noexcept {
    const Operation operation = "stxlib.children.read_output";
    if (output_ == none) return Result<Buffer<Byte>>::failure(children_detail::invalid_state(operation));
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
    if (failed_error != 0) return Result<Buffer<Byte>>::failure(record_of_error(failed_error, operation));
    if (exhausted) {
        return Result<Buffer<Byte>>::failure(host_failure::record(ErrorKind::out_of_memory, operation, HostCode::none));
    }
    Result<Buffer<Byte>> result = children_detail::finish_output(output, over_limit, operation);
    return result;
}

} // namespace stxlib
