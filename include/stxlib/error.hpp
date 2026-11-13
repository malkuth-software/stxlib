#pragma once

// The error record: the one format every STXlib failure takes, and the
// value a failure crosses a boundary as (AGENTS.md: native unwinding never
// crosses; a failure crosses as an error record we own).
//
// Shape: survey D14's recommendation (a), used provisionally until the
// owner answers D14:
// - our closed set of kinds (ErrorKind) and an optional code of ours whose
//   meaning the operation defines;
// - the foreign code, when a foreign interface failed, as a separately
//   typed datum (family and value), never compared with ours;
// - the operation, a name with static storage;
// - a message for people, never parsed, cut to a fixed capacity at a whole
//   UTF-8 sequence;
// - in development builds, the exception's type when a gate recorded an
//   exception (survey S5: release builds carry no type and no trace);
// - a per-thread sequence number, and in development builds a reference
//   to the shadow record (logical frames and native trace) captured for
//   the exception a gate recorded (shadow.hpp; survey S1 = 1).
//
// The record is a fixed-size value (no allocation, no pointer into other
// storage except the operation's static name), so it can be copied out of
// a failing operation, across threads, and through a C ABI by a binding.

#include "stxlib/require.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// Our closed set of failure kinds. A consumer maps them to its own codes
// (GUI.Forms to gf_status); a new kind is an interface change.
enum class ErrorKind : u16 {
    invalid_argument = 1,   // an input failed validation
    out_of_range = 2,       // an index or position outside the valid extent
    limit_exceeded = 3,     // a stated limit or capacity would be exceeded
    out_of_memory = 4,      // an allocation failed
    wrong_thread = 5,       // called from a thread that does not own the object
    disposed = 6,           // the object has been disposed
    stale_identity = 7,     // an identity or handle that is no longer valid
    revoked = 8,            // a callback or capability that has been revoked
    unsupported = 9,        // not supported on this host or in this build
    not_found = 10,         // a named thing does not exist
    permission_denied = 11, // the host refused access
    already_exists = 12,    // a named thing exists and must not
    interrupted = 13,       // stopped by a signal, cancellation or shutdown
    io_failure = 14,        // a read, write or device operation failed
    foreign_failure = 15,   // a foreign interface failed (see the foreign code)
    foreign_exception = 16, // an exception that is not a standard exception
    internal_failure = 17,  // a defect: a broken invariant or logic error
};

// A stable lower-case name for the kind ("invalid_argument"), for reports.
// A value outside the set yields "unknown".
[[nodiscard]] TextView error_kind_name(ErrorKind kind) noexcept;

// Where a foreign code came from. Values of different families are never
// comparable with each other or with our kinds.
enum class ForeignFamily : u16 {
    none = 0,
    posix_errno = 1,        // errno on a POSIX host (values differ between Linux and BSD hosts)
    windows_last_error = 2, // GetLastError
    windows_ntstatus = 3,   // an NTSTATUS
    windows_hresult = 4,    // an HRESULT
    crt_errno = 5,          // the Windows C runtime's errno
};

// A foreign failure code. Made only by the two factories, so a code with
// family none always has value 0.
class ForeignCode final {
public:
    // No foreign code.
    constexpr ForeignCode() noexcept = default;

    [[nodiscard]] static constexpr ForeignCode none() noexcept {
        const ForeignCode result;
        return result;
    }
    // Requirement: family is not none.
    [[nodiscard]] static constexpr ForeignCode of(const ForeignFamily code_family, const i64 code_value) noexcept {
        STXLIB_REQUIRE(code_family != ForeignFamily::none, "ForeignCode::of: family none");
        ForeignCode result;
        result.family_ = code_family;
        result.value_ = code_value;
        return result;
    }

    [[nodiscard]] constexpr bool present() const noexcept { return family_ != ForeignFamily::none; }
    [[nodiscard]] constexpr ForeignFamily family() const noexcept { return family_; }
    // 0 when not present.
    [[nodiscard]] constexpr i64 value() const noexcept { return value_; }

private:
    ForeignFamily family_{ForeignFamily::none};
    i64 value_{0};
};

// The operation a failure belongs to: a dotted name with static storage
// ("stxlib.memory.allocate", "gf.window.create"). Made only from a string
// literal, checked at compile time like TextView's.
class Operation final {
public:
    template <usize Count>
    consteval Operation(const char (&name)[Count]) noexcept : name_(name) {}

    [[nodiscard]] constexpr TextView name() const noexcept { return name_; }

private:
    TextView name_;
};

// Bytes of message an ErrorRecord keeps; longer text is cut at the last
// whole UTF-8 sequence that fits, and message_truncated() says so.
inline constexpr usize error_message_capacity = 200;
// Bytes of exception type name (the C++ runtime's mangled name, demangled
// after the fact by the reader, never in the failing process).
inline constexpr usize error_type_capacity = 64;

// Optional parts of a record, with valid defaults (house style, section 11:
// designated initializers for independent options).
struct ErrorDetail final {
    u32 code{0};
    ForeignCode foreign{};
    TextView message{};
};

// The reference to a shadow record (logical frames, and in a development
// build a native trace), read with read_shadow (shadow.hpp). Sequence 0
// means none: a record no gate made from an exception, a build without
// exceptions, or a thread whose shadow counter is exhausted.
struct ShadowReference final {
    u64 sequence{0};

    [[nodiscard]] constexpr bool present() const noexcept { return sequence != 0; }
};

namespace detail {
class ErrorRecordWriter;
} // namespace detail

class ErrorRecord final {
public:
    // A failure of `operation` with kind `kind` and the given detail. The
    // message is copied and cut to error_message_capacity at a whole
    // sequence. The record takes the calling thread's next sequence number.
    [[nodiscard]] static ErrorRecord make(ErrorKind kind, Operation operation, const ErrorDetail& detail) noexcept;
    [[nodiscard]] static ErrorRecord make(ErrorKind kind, Operation operation) noexcept;

    [[nodiscard]] ErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] u32 code() const noexcept { return code_; }
    [[nodiscard]] ForeignCode foreign() const noexcept { return foreign_; }
    [[nodiscard]] Operation operation() const noexcept { return operation_; }

    // Borrowed from this record: valid while the record is alive.
    [[nodiscard]] TextView message() const& noexcept {
        const TextView result = detail::TextViewAccess::adopt_validated(message_, message_count_);
        return result;
    }
    TextView message() const&& = delete;
    [[nodiscard]] bool message_truncated() const noexcept { return message_truncated_; }

    // The exception's mangled type name when a gate recorded an exception in
    // a development build; empty otherwise. Borrowed from this record.
    [[nodiscard]] TextView exception_type() const& noexcept {
        const TextView result = detail::TextViewAccess::adopt_validated(exception_type_, exception_type_count_);
        return result;
    }
    TextView exception_type() const&& = delete;

    // The calling thread's sequence number when the record was made: 1 for
    // its first record, increasing. 0 once the thread's counter is
    // exhausted (2^64 - 1 records), which is reported, never wrapped.
    [[nodiscard]] u64 sequence() const noexcept { return sequence_; }
    [[nodiscard]] ShadowReference shadow() const noexcept { return shadow_; }

private:
    friend class detail::ErrorRecordWriter;

    ErrorRecord(const ErrorKind failure_kind, const Operation failed_operation) noexcept
        : kind_(failure_kind), operation_(failed_operation) {}

    ErrorKind kind_;
    u16 message_count_{0};
    u16 exception_type_count_{0};
    bool message_truncated_{false};
    u32 code_{0};
    ForeignCode foreign_{};
    Operation operation_;
    u64 sequence_{0};
    ShadowReference shadow_{};
    // Every byte is initialised, so a copy reads no indeterminate value.
    char message_[error_message_capacity]{};
    char exception_type_[error_type_capacity]{};
};

static_assert(error_message_capacity <= u16_max && error_type_capacity <= u16_max,
              "the counts are stored in 16 bits");

// Room for a demangled exception type (exception_type_for_display).
inline constexpr usize exception_type_display_capacity = 256;

// The record's exception type for people: demangled into `output` when the
// C++ runtime can ("std::invalid_argument" for "St16invalid_argument"),
// else the mangled name; empty when the record has none. The result is
// borrowed from `output` or from `record`, so both must outlive it.
// Demangling happens here, at print time, never at capture, so the capture
// path stays free of allocation. Cost: one abi::__cxa_demangle call, which
// allocates its result with malloc (Itanium C++ ABI, section 3.4); it is
// copied into `output` (cut to its capacity) and freed. A mangled name the
// record cut at its capacity does not demangle and is shown as recorded.
[[nodiscard]] TextView exception_type_for_display(const ErrorRecord& record, MutableView<char> output) noexcept;

} // namespace stxlib
