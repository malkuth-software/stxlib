#pragma once

// Public transcoding between UTF-8, UTF-16, UTF-32 and WTF-8 (roadmap M2,
// R15; audit B: codecvt and wstring_convert are deprecated and leave the
// standard in C++26, the mbrtoc functions follow the C locale, and
// consumers call MultiByteToWideChar directly).
//
// UTF-8 is STXlib's text (TextView, Utf8String: well-formed by
// construction). UTF-16 units are u16 and UTF-32 units u32, so the forms
// are the same on every platform (wchar_t is 16 bits on Windows and 32
// elsewhere; a Windows caller passes its wchar_t buffer through a u16 view
// of the same storage). Converting from UTF-8 cannot fail on the text; only
// owned output can run out of memory. Converting to UTF-8 fails with
// invalid_argument on an unpaired surrogate (UTF-16) or a value that is not
// a Unicode scalar value (UTF-32); the record's detail code is the index of
// the offending unit, saturated at 2^32 - 1.
//
// WTF-8 (core/wtf8.hpp, Simon Sapin's encoding) carries any sequence of
// 16-bit units losslessly, unpaired surrogates included, for names the host
// does not check (Windows file names). It is not UTF-8 when it holds a
// surrogate, so it travels as bytes (View<char>, Vector<char>), never as
// TextView. Path uses the same encoding for its Windows names.
//
// No output is written on failure: every conversion into caller storage or
// onto an existing Utf8String checks and counts its input first, then
// writes. tests/transcode_tests.cpp checks every scalar value, every pair of
// 16-bit units and the round trips over its stated domains
// (proofs/REPORT.md).

#include "stxlib/result.hpp"
#include "stxlib/text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

// ---- code points in UTF-8 text ------------------------------------------

// One decoded code point: its value and the byte length of its sequence.
struct DecodedCodePoint final {
    u32 value;
    u32 length;
};

// The number of code points in the text.
[[nodiscard]] usize code_point_count(TextView text) noexcept;

// The code point whose sequence starts at `offset`. out_of_range when
// offset >= count; invalid_argument when offset is not a boundary.
[[nodiscard]] Result<DecodedCodePoint> code_point_at(TextView text, usize offset) noexcept;

// ---- UTF-16 ---------------------------------------------------------------

// The number of UTF-16 units the text needs.
[[nodiscard]] usize utf16_count(TextView text) noexcept;

// The number of UTF-8 bytes the units need; invalid_argument at the first
// unpaired surrogate.
[[nodiscard]] Result<usize> utf8_count_of_utf16(View<u16> units) noexcept;

// Writes the text as UTF-16 to output[0, utf16_count) and returns that
// count. limit_exceeded when output is shorter; output is then unchanged.
[[nodiscard]] Result<usize> write_utf16(TextView text, MutableView<u16> output) noexcept;

// Writes the units as UTF-8 to output[0, n) and returns n (the bytes are
// then well-formed; validate_utf8 adopts them as a TextView).
// invalid_argument for an unpaired surrogate, limit_exceeded when output is
// shorter; output is then unchanged.
[[nodiscard]] Result<usize> write_utf8_of_utf16(View<u16> units, MutableView<char> output) noexcept;

// Owned forms. out_of_memory when the allocation is refused.
[[nodiscard]] Result<Vector<u16>> utf16_of(TextView text) noexcept;
[[nodiscard]] Result<Utf8String> text_of_utf16(View<u16> units) noexcept;
// Appends the units to `text`; on failure `text` is unchanged.
[[nodiscard]] Status append_utf16(Utf8String& text, View<u16> units) noexcept;

// ---- UTF-32 ---------------------------------------------------------------

[[nodiscard]] Result<Vector<u32>> utf32_of(TextView text) noexcept;
// invalid_argument for a surrogate or a value above U+10FFFF.
[[nodiscard]] Result<Utf8String> text_of_utf32(View<u32> code_points) noexcept;
// Appends the code points to `text`; on failure `text` is unchanged.
[[nodiscard]] Status append_utf32(Utf8String& text, View<u32> code_points) noexcept;

// ---- WTF-8 ----------------------------------------------------------------

// Whether the bytes are well-formed WTF-8 (well-formed UTF-8 is).
[[nodiscard]] bool is_wtf8(View<char> bytes) noexcept;

// The units as WTF-8: a high surrogate followed by a low one is one
// supplementary code point, any other surrogate is encoded alone. Only an
// allocation can fail.
[[nodiscard]] Result<Vector<char>> wtf8_of_utf16(View<u16> units) noexcept;

// The 16-bit units well-formed WTF-8 names; invalid_argument when the bytes
// are not WTF-8 (detail code: the offset where reading stopped).
[[nodiscard]] Result<Vector<u16>> utf16_of_wtf8(View<char> bytes) noexcept;

} // namespace stxlib
