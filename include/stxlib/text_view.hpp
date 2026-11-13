#pragma once

// TextView: borrowed text that is well-formed UTF-8 (detail/utf8.hpp).
//
// A TextView is made in one of three ways, and each establishes the
// encoding before the view exists:
// - from a string literal, checked by the compiler (the constructor is
//   consteval, so an ill-formed literal does not compile, and a literal has
//   static storage, so the view never dangles);
// - by validate_utf8 (text.hpp), which checks foreign bytes and fails with a
//   result when they are not UTF-8;
// - by STXlib's own types that hold validated text (ErrorRecord, Utf8String),
//   borrowing from them.
//
// Layout: a pointer to the first byte, then the byte count. No terminating
// NUL is promised; count() is the length. Lifetime: as the storage it was
// made from (a literal: the whole program).

#include "stxlib/detail/utf8.hpp"
#include "stxlib/types.hpp"

namespace stxlib {

namespace detail {
class TextViewAccess;
} // namespace detail

class TextView final {
public:
    // The empty text.
    constexpr TextView() noexcept = default;

    // A string literal, checked at compile time: it must end with its NUL
    // (which is not part of the view) and be well-formed UTF-8. Implicit,
    // since a literal and its view mean the same text.
    template <usize Count>
    consteval TextView(const char (&literal)[Count]) noexcept : bytes_(literal), count_(Count - 1) {
        if (literal[Count - 1] != '\0') detail::literal_is_not_utf8();
        if (!detail::utf8_well_formed(literal, Count - 1)) detail::literal_is_not_utf8();
    }

    [[nodiscard]] constexpr const char* data() const noexcept { return bytes_; }
    [[nodiscard]] constexpr usize count() const noexcept { return count_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0; }

    // Byte-wise equality: the same bytes in the same order. No
    // normalisation (two spellings of one character are different text).
    [[nodiscard]] constexpr bool operator==(const TextView& other) const noexcept {
        if (count_ != other.count_) return false;
        for (usize index = 0; index < count_; ++index) {
            if (bytes_[index] != other.bytes_[index]) return false;
        }
        return true;
    }
    [[nodiscard]] constexpr bool operator!=(const TextView& other) const noexcept {
        const bool equal = *this == other;
        return !equal;
    }

private:
    friend class detail::TextViewAccess;

    // Bytes already known to be well-formed UTF-8 (TextViewAccess only).
    constexpr TextView(const char* const validated_bytes, const usize byte_count) noexcept
        : bytes_(validated_bytes), count_(byte_count) {}

    const char* bytes_{nullptr};
    usize count_{0};
};

namespace detail {

// For STXlib's own code that has validated the bytes it adopts: the
// validator (src/core/text.cpp) and the types that store validated text.
class TextViewAccess final {
public:
    // Requirement: bytes[0, count) is well-formed UTF-8 and outlives the view.
    [[nodiscard]] static constexpr TextView adopt_validated(const char* const bytes, const usize count) noexcept {
        const TextView result(bytes, count);
        return result;
    }
};

} // namespace detail

} // namespace stxlib
