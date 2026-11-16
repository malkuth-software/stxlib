#pragma once

// Validating foreign bytes as UTF-8, and owned UTF-8 text.

// Early exits (owner, 2026-11-15, decisions/2026-11-15-early-exit-
// integrity.md): Utf8String calls no consumer code. Its calls outside
// STXlib are the host C library's memcpy and memmove. Growth is form 1 (the
// new block is filled, then adopted); an in-place edit is form 2: between
// the moves the bytes may be briefly not well-formed, but the destructor
// reads only the storage pointer and capacity, which are whole at every
// call, so the partial state is safe to unwind through.

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

// Byte-wise order of two texts: negative when left sorts first, zero when
// equal, positive when right sorts first. For well-formed UTF-8 this is the
// order of the code point sequences (Unicode section 2.5: UTF-8 preserves
// code point order). No normalisation and no locale (D9 a).
[[nodiscard]] i32 compare_text(TextView left, TextView right) noexcept;

// Owned, growable UTF-8 text on STXlib's allocator: the owned vocabulary's
// replacement for std::string at a public surface (owner, D15 before 1.0).
//
// Invariant: the bytes [0, count()) are well-formed UTF-8 and the byte at
// count() is NUL, always. Every operation that adds text takes a TextView,
// which is well-formed by construction, and every offset it takes must be a
// sequence boundary (a byte that does not continue a sequence, or count()),
// so no operation can make the text ill-formed. An offset that is out of
// range fails with out_of_range; one inside a sequence fails with
// invalid_argument; neither changes anything.
//
// Allocation (D8 = a, as answered): every operation that may allocate
// returns a Status or a Result; a refused allocation is out_of_memory (or
// limit_exceeded when the size is not representable) and changes nothing.
// A TextView argument may borrow from this same string.
//
// Storage: text of up to inline_capacity bytes lives inside the object, so
// short text allocates nothing; longer text lives in a block of capacity()
// + 1 bytes (the NUL included). Layout (defined here, the same on every
// host, 32 bytes): the count, the capacity (inline_capacity while the text
// is inline), then a 16-byte union of the block pointer and the inline
// bytes. Copying makes a new string with the same text (owner 2026-11-16,
// notes decisions/2026-11-16-vocabulary-copy.md); text that does not fit
// inline allocates, and an allocation the copy cannot have ends the process
// through fail_fast (copy() and copy_of() report it instead). Moving leaves
// the source empty.
//
// Borrows: view(), c_text() and data borrowed from them stay valid until an
// operation that changes the text, a move from the string, or its
// destruction. A move does not keep them: inline text moves with the object.
class Utf8String final {
public:
    // Bytes of text held without allocating (a 16-byte array with its NUL).
    static constexpr usize inline_capacity = 15;

    // The empty text (inline; allocates nothing).
    Utf8String() noexcept = default;

    // A copy of `text`.
    [[nodiscard]] static Result<Utf8String> copy_of(TextView text) noexcept;
    // Empty text with room for `room` bytes.
    [[nodiscard]] static Result<Utf8String> with_capacity(usize room) noexcept;
    // A named copy of this text.
    [[nodiscard]] Result<Utf8String> copy() const noexcept;

    Utf8String(Utf8String&& other) noexcept;
    Utf8String& operator=(Utf8String&& other) noexcept;
    // The ordinary copy: a new string holding other's text. Exhaustion fails
    // fast with copy_of's out_of_memory record.
    Utf8String(const Utf8String& other) noexcept;
    // The copy is made whole first (failing fast leaves this string as it
    // was), then moved in, so the old text is released only once this
    // string holds the new one; assigning a string to itself changes
    // nothing.
    Utf8String& operator=(const Utf8String& other) noexcept;
    ~Utf8String();

    // ---- queries ----------------------------------------------------------

    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }

    // Borrowed from this string (see "Borrows").
    [[nodiscard]] TextView view() const& noexcept;
    TextView view() const&& = delete;
    // The text followed by its NUL, for a foreign call that wants a C string.
    // Text that holds a NUL byte reads as shorter there. Borrowed as view().
    [[nodiscard]] const char* c_text() const& noexcept;
    const char* c_text() const&& = delete;

    // True when `offset` is count() or the start of a sequence.
    [[nodiscard]] bool is_boundary(usize offset) const noexcept;

    [[nodiscard]] bool equals(TextView text) const noexcept;
    [[nodiscard]] bool operator==(const Utf8String& other) const noexcept;
    [[nodiscard]] bool operator!=(const Utf8String& other) const noexcept;

    // ---- changes ------------------------------------------------------------

    // Room for at least `wanted` bytes; the text is unchanged.
    [[nodiscard]] Status reserve(usize wanted) noexcept;
    // The text becomes `text`.
    [[nodiscard]] Status assign(TextView text) noexcept;
    [[nodiscard]] Status append(TextView text) noexcept;
    // Appends one code point, encoded as UTF-8. A surrogate (U+D800 to
    // U+DFFF) or a value above U+10FFFF fails with invalid_argument.
    [[nodiscard]] Status append_code_point(u32 code_point) noexcept;
    // Inserts `text` before the byte at `offset` (a boundary).
    [[nodiscard]] Status insert(usize offset, TextView text) noexcept;
    // Removes bytes [offset, offset + length); both ends must be boundaries.
    [[nodiscard]] Status erase(usize offset, usize length) noexcept;
    // Replaces bytes [offset, offset + length) with `text`; both ends must
    // be boundaries.
    [[nodiscard]] Status replace(usize offset, usize length, TextView text) noexcept;
    // Keeps the first `length` bytes; length must be a boundary.
    [[nodiscard]] Status truncate(usize length) noexcept;

    // The empty text; the capacity remains.
    void clear() noexcept;
    // The empty text; a block is released and the text is inline again.
    void release() noexcept;

private:
    [[nodiscard]] bool on_heap() const noexcept { return capacity_ > inline_capacity; }
    [[nodiscard]] char* bytes() noexcept;
    [[nodiscard]] const char* bytes() const noexcept;
    [[nodiscard]] Status replace_checked(usize offset, usize length, TextView text, Operation operation) noexcept;

    union Storage {
        char* block;
        char inline_bytes[inline_capacity + 1];

        constexpr Storage() noexcept : inline_bytes{} {}
    };

    usize count_{0};
    usize capacity_{inline_capacity};
    Storage storage_{};
};

static_assert(sizeof(Utf8String) == 2 * sizeof(usize) + 16,
              "Utf8String's layout: count, capacity and 16 inline bytes (32 bytes on 64-bit targets, 24 on wasm32)");

} // namespace stxlib
