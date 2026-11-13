#pragma once

// Validating foreign bytes as UTF-8, and owned UTF-8 text.

#include "stxlib/buffer.hpp"
#include "stxlib/error.hpp"
#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// A view of bytes[0, count) when they are well-formed UTF-8; otherwise
// invalid_argument with code = the offset of the first byte that is not
// part of a well-formed sequence (u32_max when the offset does not fit
// in 32 bits). The view borrows the caller's bytes.
// Requirement: bytes is non-null unless count is zero.
[[nodiscard]] Result<TextView> validate_utf8(const char* bytes, usize count) noexcept;

// Owned UTF-8 text on STXlib's allocator: a Buffer<char> whose bytes are
// well-formed UTF-8 by construction. Unique owner; moving leaves the source
// empty.
class Utf8String final {
public:
    // The empty text.
    Utf8String() noexcept = default;

    // A copy of `text`. Fails with out_of_memory or limit_exceeded as
    // Buffer::allocate_zeroed does.
    [[nodiscard]] static Result<Utf8String> copy_of(TextView text) noexcept;

    Utf8String(Utf8String&&) noexcept = default;
    Utf8String& operator=(Utf8String&&) noexcept = default;
    Utf8String(const Utf8String&) = delete;
    Utf8String& operator=(const Utf8String&) = delete;
    ~Utf8String() = default;

    [[nodiscard]] usize count() const noexcept { return bytes_.count(); }
    [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }

    // Borrowed from this string: valid until it is moved from or destroyed.
    [[nodiscard]] TextView view() const& noexcept;
    TextView view() const&& = delete;

private:
    Buffer<char> bytes_;
};

} // namespace stxlib
