// The console (include/stxlib/console.hpp): serialization and line
// assembly over the family's backend (hosted/console_backend.hpp). Ported
// from stx_auxlib's src/hosted/console.cpp at b6e330a.
//
// Changes from stx_auxlib: the locks and the read-ahead bytes live in
// stxlib_state (state.hpp), so every module shares them; the read-ahead
// block comes from the host's C library, which every module shares, and is
// never released; failures are error records.

#include "stxlib/console.hpp"

#include "core/host_failure.hpp"
#include "core/spin_lock.hpp"
#include "hosted/console_backend.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"
#include "stxlib/memory.hpp"

#include <cstring>

namespace stxlib {

namespace {

constexpr usize read_chunk_bytes = 4096;

Status status_of(const console_backend::Report report, const Operation operation) noexcept {
    switch (report.outcome) {
    case console_backend::Outcome::ok:
        return Status::success();
    case console_backend::Outcome::closed:
        return host_failure::failed(ErrorKind::io_failure, operation, HostCode::stream_closed, report.foreign);
    case console_backend::Outcome::invalid_text:
        return host_failure::failed(ErrorKind::invalid_argument, operation, HostCode::invalid_text);
    case console_backend::Outcome::out_of_memory:
        return host_failure::failed(ErrorKind::out_of_memory, operation, HostCode::none);
    case console_backend::Outcome::io_error:
        break;
    }
    return host_failure::failed(ErrorKind::io_failure, operation, HostCode::none, report.foreign);
}

// The process's read-ahead bytes (state.hpp), grown in blocks of STXlib's
// allocator, which every module shares.
bool reserve_pending(state::ProcessState& process, const usize wanted) noexcept {
    if (wanted <= process.console_pending_capacity) return true;
    usize next = process.console_pending_capacity < read_chunk_bytes ? read_chunk_bytes
                                                                      : process.console_pending_capacity;
    while (next < wanted) {
        if (next > usize_max / 2) return false;
        next = next * 2;
    }
    void* const block = memory::allocate(next, 16);
    if (block == nullptr) return false;
    if (process.console_pending_count != 0) {
        std::memcpy(block, process.console_pending, process.console_pending_count);
    }
    memory::release(process.console_pending);
    process.console_pending = static_cast<char*>(block);
    process.console_pending_capacity = next;
    return true;
}

void drop_front(state::ProcessState& process, const usize count) noexcept {
    const usize rest = process.console_pending_count - count;
    if (rest != 0) std::memmove(process.console_pending, process.console_pending + count, rest);
    process.console_pending_count = rest;
}

usize find_newline(const state::ProcessState& process) noexcept {
    for (usize index = 0; index < process.console_pending_count; ++index) {
        if (process.console_pending[index] == '\n') return index;
    }
    return usize_max;
}

Result<ConsoleLine> line_from(const char* const bytes, const usize count) noexcept {
    Result<TextView> valid = validate_utf8(bytes, count);
    if (valid.failed()) {
        const ErrorRecord invalid = host_failure::record(ErrorKind::invalid_argument, "stxlib.console.read_line",
                                                         HostCode::invalid_text);
        return Result<ConsoleLine>::failure(invalid);
    }
    Result<Utf8String> copied = Utf8String::copy_of(*valid.value());
    if (copied.failed()) return Result<ConsoleLine>::failure(*copied.error());
    ConsoleLine line = ConsoleLine::line(static_cast<Result<Utf8String>&&>(copied).take_value());
    return Result<ConsoleLine>::success(static_cast<ConsoleLine&&>(line));
}

} // namespace

ConsoleLine ConsoleLine::line(Utf8String&& text) noexcept {
    ConsoleLine result;
    result.text_ = static_cast<Utf8String&&>(text);
    return result;
}

ConsoleLine ConsoleLine::end_of_input() noexcept {
    ConsoleLine result;
    result.ended_ = true;
    return result;
}

Status write_console(const ConsoleStream stream, const TextView text) noexcept {
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.console_output_lock);
    host::flush_c_stream(stream == ConsoleStream::error);
    const console_backend::Report report = console_backend::write(stream, text.data(), text.count());
    const Status result = status_of(report, "stxlib.console.write");
    return result;
}

Status write_console_line(const ConsoleStream stream, const TextView text) noexcept {
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.console_output_lock);
    host::flush_c_stream(stream == ConsoleStream::error);
    const console_backend::Report report = console_backend::write_line(stream, text.data(), text.count());
    const Status result = status_of(report, "stxlib.console.write_line");
    return result;
}

bool console_is_terminal(const ConsoleStream stream) noexcept {
    const bool result = console_backend::is_terminal(stream);
    return result;
}

// Takes the next line from the read-ahead bytes, reading more as needed. A
// line over `limit` is discarded through its ending, so the next call
// starts at the following line.
Result<ConsoleLine> read_console_line(const usize limit) noexcept {
    state::ProcessState& process = state::current_process();
    const spin::Guard lock(process.console_input_lock);
    char chunk[read_chunk_bytes];
    bool over_limit = false;
    usize length = 0;
    for (;;) {
        const usize ending = find_newline(process);
        if (ending != usize_max) {
            length = ending;
            if (length > 0 && process.console_pending[length - 1] == '\r') length = length - 1;
            if (length > limit) over_limit = true;
            // The line stays in place until it is copied; its ending is
            // dropped with it below.
            if (over_limit) {
                drop_front(process, ending + 1);
                break;
            }
            Result<ConsoleLine> result = line_from(process.console_pending, length);
            drop_front(process, ending + 1);
            return result;
        }
        if (process.console_input_ended) {
            if (process.console_pending_count == 0 && !over_limit) {
                return Result<ConsoleLine>::success(ConsoleLine::end_of_input());
            }
            length = process.console_pending_count;
            if (length > 0 && process.console_pending[length - 1] == '\r') length = length - 1;
            if (length > limit) over_limit = true;
            if (over_limit) {
                process.console_pending_count = 0;
                break;
            }
            Result<ConsoleLine> result = line_from(process.console_pending, length);
            process.console_pending_count = 0;
            return result;
        }
        // Discard what is already known to be too long (more than `limit`
        // bytes besides a possible '\r'), keeping memory bounded by the
        // limit while the rest of the line arrives.
        if (process.console_pending_count > limit && process.console_pending_count - limit > 1) {
            over_limit = true;
            process.console_pending_count = 0;
        }
        usize transferred = 0;
        const console_backend::Report report = console_backend::read(chunk, read_chunk_bytes, transferred);
        if (report.outcome != console_backend::Outcome::ok) {
            const Status failed = status_of(report, "stxlib.console.read_line");
            return Result<ConsoleLine>::failure(*failed.error());
        }
        if (transferred == 0) {
            process.console_input_ended = true;
            continue;
        }
        if (process.console_pending_count > usize_max - transferred ||
            !reserve_pending(process, process.console_pending_count + transferred)) {
            const ErrorRecord exhausted =
                host_failure::record(ErrorKind::out_of_memory, "stxlib.console.read_line", HostCode::none);
            return Result<ConsoleLine>::failure(exhausted);
        }
        std::memcpy(process.console_pending + process.console_pending_count, chunk, transferred);
        process.console_pending_count = process.console_pending_count + transferred;
    }
    const ErrorRecord too_long =
        host_failure::record(ErrorKind::limit_exceeded, "stxlib.console.read_line", HostCode::too_long);
    return Result<ConsoleLine>::failure(too_long);
}

} // namespace stxlib
