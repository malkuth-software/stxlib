#pragma once

// The writers of an ErrorRecord's text fields (ErrorRecord's friend). Every
// write keeps the record's invariant: each text field holds well-formed
// UTF-8, cut at a whole sequence, and every byte past its count is zero.

#include "stxlib/error.hpp"
#include "stxlib/types.hpp"

namespace stxlib::detail {

class ErrorRecordWriter final {
public:
    // Copies `message`, cut to error_message_capacity.
    static void copy_message(ErrorRecord& record, TextView message) noexcept;

    // Copies a NUL-terminated foreign string (an exception's what()): its
    // longest well-formed prefix that fits; message_truncated() when that is
    // not all of it. Reads at most error_message_capacity + 1 bytes of it.
    static void copy_foreign_message(ErrorRecord& record, const char* text) noexcept;

    // Copies a NUL-terminated mangled type name the same way, cut to
    // error_type_capacity (no truncation flag: a cut name still identifies
    // most types, and the reader sees that it does not demangle).
    static void copy_exception_type(ErrorRecord& record, const char* name) noexcept;

    // Attaches a shadow record (exception_shadow.cpp, development builds).
    static void set_shadow(ErrorRecord& record, ShadowReference shadow) noexcept;
};

} // namespace stxlib::detail
