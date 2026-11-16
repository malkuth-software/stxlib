#pragma once

// Two Unicode properties of a code point, for text an application lays out:
// its general category, and canonical composition and decomposition (the
// steps NFC and NFD are made of). The data are the Unicode Character
// Database 18.0's, as the shaper carries them (src/algorithms/glyphs/
// shape_unicode.inc, written by tools/gen_shape_unicode.py from harfrust
// 0.16.0's tables of the same version); tests/unicode_properties_tests.cpp
// checks every code point against third_party/unicode-18.0.0/UnicodeData.txt.
//
// Every function is pure: a fixed number of table reads or one search by
// halving; no allocation, no state, the same answer on every host. Any
// 32-bit number may be asked about; one above 0x10FFFF is unassigned and
// composes and decomposes with nothing.

#include "stxlib/types.hpp"

namespace stxlib::text {

// The general categories, in the order Unicode lists them (UAX #44).
enum class GeneralCategory : u8 {
    uppercase_letter = 0,       // Lu
    lowercase_letter = 1,       // Ll
    titlecase_letter = 2,       // Lt
    modifier_letter = 3,        // Lm
    other_letter = 4,           // Lo
    nonspacing_mark = 5,        // Mn
    spacing_mark = 6,           // Mc
    enclosing_mark = 7,         // Me
    decimal_number = 8,         // Nd
    letter_number = 9,          // Nl
    other_number = 10,          // No
    connector_punctuation = 11, // Pc
    dash_punctuation = 12,      // Pd
    open_punctuation = 13,      // Ps
    close_punctuation = 14,     // Pe
    initial_punctuation = 15,   // Pi
    final_punctuation = 16,     // Pf
    other_punctuation = 17,     // Po
    math_symbol = 18,           // Sm
    currency_symbol = 19,       // Sc
    modifier_symbol = 20,       // Sk
    other_symbol = 21,          // So
    space_separator = 22,       // Zs
    line_separator = 23,        // Zl
    paragraph_separator = 24,   // Zp
    control = 25,               // Cc
    format = 26,                // Cf
    surrogate = 27,             // Cs
    private_use = 28,           // Co
    unassigned = 29,            // Cn
};

[[nodiscard]] GeneralCategory general_category(u32 code_point) noexcept;

// The canonical combining class (0 for a starter).
[[nodiscard]] u32 combining_class(u32 code_point) noexcept;

// One step of canonical decomposition: the one or two code points this one
// is canonically made of (`second` 0 when it is one), Hangul syllables by
// their algorithm. False, both 0: it has no canonical decomposition.
[[nodiscard]] bool canonical_decomposition(u32 code_point, u32& first, u32& second) noexcept;

// The primary composite of two code points (NFC's pairwise composition):
// true and `composed` when `first` then `second` compose to one code point
// (composition exclusions and singletons excluded, as NFC has it; Hangul
// syllables by their algorithm). False, `composed` 0, otherwise. The order
// and blocking of NFC (which marks may reach a starter) are the caller's.
[[nodiscard]] bool canonical_composition(u32 first, u32 second, u32& composed) noexcept;

} // namespace stxlib::text
