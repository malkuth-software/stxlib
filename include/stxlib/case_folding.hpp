#pragma once

// Simple Unicode case folding (M2 C7; owner choice 9 of 2026-11-15:
// simple folding before 1.0). For caseless matching of text: a search
// field, a file name compared the way a case-insensitive file system
// compares it, a sort key's tie-break. Locale-free (D9 a): the same answer
// in every locale and on every host, unlike towlower, CompareStringOrdinal
// with ignore-case, strcasecmp or ICU under a default locale, which follow
// the host's tables and Unicode version.
//
// The data: Unicode 18.0.0's CaseFolding.txt, mappings with status C and S
// (the file's "Usage" A, simple case folding), 1,533 code points
// (third_party/unicode-18.0.0/, pinned by SHA-256 in PROVENANCE.md; Unicode
// License v3). tools/gen_case_folding.py generates the table and checks the
// properties stated below before writing it.
//
// What simple folding is and is not:
// - every code point maps to one code point (itself when the file does not
//   list it); "ß" stays "ß" and does not match "ss" (that is full folding,
//   status F, not provided);
// - the Turkic dotted and dotless I (status T) are not used: "I" folds to
//   "i" in every language;
// - no normalisation: "é" as one code point and "e" followed by U+0301 are
//   different text before and after folding;
// - idempotent: folding folded text changes nothing;
// - U+212A KELVIN SIGN folds to "k" and U+017F LONG S to "s", so text that
//   is not ASCII can fold to ASCII; ASCII folds only A-Z, to a-z.
//
// Every function here is pure computation: no allocation, no host call, no
// state. In proof scope: tests/case_folding_tests.cpp checks every code
// point 0 to 0x10FFFF against an independent reading of CaseFolding.txt;
// proofs/esbmc/case_folding.cpp states the bounded harnesses.

#include "stxlib/numeric_text.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::text {

// The Unicode version of the table.
inline constexpr u32 case_folding_unicode_major = 18;
inline constexpr u32 case_folding_unicode_minor = 0;
inline constexpr u32 case_folding_unicode_update = 0;

// The simple case folding of one code point. A value that is not a scalar
// value (a surrogate, or above 0x10FFFF) is returned unchanged, as is every
// code point the table does not list.
[[nodiscard]] u32 fold_case_simple(u32 code_point) noexcept;

// The most bytes the folding of `byte_count` bytes of text takes: no
// scalar's UTF-8 form grows by more than half (two bytes to three, the
// largest growth in the table; tools/gen_case_folding.py checks it).
[[nodiscard]] constexpr usize maximum_folded_bytes(const usize byte_count) noexcept {
    return byte_count + byte_count / 2;
}

// The bytes the folding of `text` takes, at most maximum_folded_bytes of its
// count.
[[nodiscard]] usize folded_bytes(TextView text) noexcept;

// Writes the simple case folding of `text`, scalar by scalar, to
// output[0, n) and returns written(that text, well-formed UTF-8); or writes
// nothing and returns too_small(n) when n > output.count(). The result is
// borrowed from `output`. Requirement: `output` does not overlap `text`.
[[nodiscard]] Formatted fold_case(MutableView<char> output, TextView text) noexcept;

// The order of two texts' foldings, scalar value by scalar value, a proper
// prefix first; the same as comparing the folded bytes, since UTF-8
// preserves the order of scalar values. Not a collation: no language's
// alphabetical order.
enum class TextOrder : i8 {
    before = -1,
    same = 0,
    after = 1,
};

// Compares without folding into storage.
[[nodiscard]] TextOrder compare_folded(TextView left, TextView right) noexcept;

// Whether the texts are equal after simple case folding.
[[nodiscard]] bool equal_folded(TextView left, TextView right) noexcept;

} // namespace stxlib::text
