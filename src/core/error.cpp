// ErrorRecord (include/stxlib/error.hpp).

#include "stxlib/error.hpp"

#include "core/error_writer.hpp"
#include "hosted/host.hpp"
#include "state/state.hpp"

#include "stxlib/detail/utf8.hpp"
#include "stxlib/text.hpp"

#include <cstring>

namespace stxlib {

namespace {

// The next record sequence number of the calling thread, or 0 once 2^64 - 1
// have been issued: the counter refuses rather than wraps (house style,
// section 3). The counter is in stxlib_state, so every module's copy of
// this code counts in one sequence per thread.
u64 next_sequence() noexcept {
    // In the anchor (state.hpp, ThreadFlags), so numbering needs no
    // allocation and continues on the failure reserve.
    state::ThreadFlags& flags = state::current_flags();
    if (flags.last_record_sequence == u64_max) return 0;
    flags.last_record_sequence = flags.last_record_sequence + 1;
    const u64 result = flags.last_record_sequence;
    return result;
}

// The length of a NUL-terminated foreign string, reading at most `limit`
// bytes; limit when no NUL is found within it.
usize bounded_length(const char* const text, const usize limit) noexcept {
    usize length = 0;
    while (length < limit && text[length] != '\0') ++length;
    return length;
}

} // namespace

namespace detail {

void ErrorRecordWriter::copy_message(ErrorRecord& record, const TextView message) noexcept {
    const usize kept = utf8_prefix(message.data(), message.count(), error_message_capacity);
    std::memset(record.message_, 0, error_message_capacity);
    if (kept != 0) std::memcpy(record.message_, message.data(), kept);
    record.message_count_ = static_cast<u16>(kept);
    record.message_truncated_ = kept != message.count();
}

void ErrorRecordWriter::copy_foreign_message(ErrorRecord& record, const char* const text) noexcept {
    std::memset(record.message_, 0, error_message_capacity);
    record.message_count_ = 0;
    record.message_truncated_ = false;
    if (text == nullptr) return;
    // One byte past the capacity tells "exactly full" from "longer".
    const usize length = bounded_length(text, error_message_capacity + 1);
    const usize kept = utf8_prefix(text, length, error_message_capacity);
    if (kept != 0) std::memcpy(record.message_, text, kept);
    record.message_count_ = static_cast<u16>(kept);
    record.message_truncated_ = kept != length;
}

void ErrorRecordWriter::copy_exception_type(ErrorRecord& record, const char* const name) noexcept {
    std::memset(record.exception_type_, 0, error_type_capacity);
    record.exception_type_count_ = 0;
    if (name == nullptr) return;
    const usize length = bounded_length(name, error_type_capacity);
    const usize kept = utf8_prefix(name, length, error_type_capacity);
    if (kept != 0) std::memcpy(record.exception_type_, name, kept);
    record.exception_type_count_ = static_cast<u16>(kept);
}

void ErrorRecordWriter::set_shadow(ErrorRecord& record, const ShadowReference shadow) noexcept {
    record.shadow_ = shadow;
}

} // namespace detail

ErrorRecord ErrorRecord::make(const ErrorKind kind, const Operation operation, const ErrorDetail& detail) noexcept {
    ErrorRecord record(kind, operation);
    record.code_ = detail.code;
    record.foreign_ = detail.foreign;
    record.sequence_ = next_sequence();
    detail::ErrorRecordWriter::copy_message(record, detail.message);
    return record;
}

ErrorRecord ErrorRecord::make(const ErrorKind kind, const Operation operation) noexcept {
    const ErrorDetail none{};
    const ErrorRecord record = make(kind, operation, none);
    return record;
}

TextView exception_type_for_display(const ErrorRecord& record, const MutableView<char> output) noexcept {
    const TextView mangled = record.exception_type();
    if (mangled.empty() || output.count() == 0) return mangled;
    char terminated[error_type_capacity + 1]{};
    std::memcpy(terminated, mangled.data(), mangled.count());
    const usize written = host::demangle_type_name(terminated, output.data(), output.count());
    if (written == 0) return mangled;
    Result<TextView> readable = validate_utf8(output.data(), written);
    if (readable.failed()) return mangled;
    const TextView result = *readable.value();
    return result;
}

TextView error_kind_name(const ErrorKind kind) noexcept {
    switch (kind) {
    case ErrorKind::invalid_argument: return "invalid_argument";
    case ErrorKind::out_of_range: return "out_of_range";
    case ErrorKind::limit_exceeded: return "limit_exceeded";
    case ErrorKind::out_of_memory: return "out_of_memory";
    case ErrorKind::wrong_thread: return "wrong_thread";
    case ErrorKind::disposed: return "disposed";
    case ErrorKind::stale_identity: return "stale_identity";
    case ErrorKind::revoked: return "revoked";
    case ErrorKind::unsupported: return "unsupported";
    case ErrorKind::not_found: return "not_found";
    case ErrorKind::permission_denied: return "permission_denied";
    case ErrorKind::already_exists: return "already_exists";
    case ErrorKind::interrupted: return "interrupted";
    case ErrorKind::io_failure: return "io_failure";
    case ErrorKind::foreign_failure: return "foreign_failure";
    case ErrorKind::foreign_exception: return "foreign_exception";
    case ErrorKind::internal_failure: return "internal_failure";
    }
    // A value outside the closed set (read from foreign memory or a cast):
    // the one fallback (house style, section 2).
    return "unknown";
}

} // namespace stxlib
