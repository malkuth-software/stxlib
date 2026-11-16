#pragma once

// The line-break automaton's pieces shared by analyse_paragraph
// (analysis.cpp) and find_line_breaks (line_break.cpp): the UTF-8 decoder,
// the table of ASCII letters and the look-ahead.
//
// The automaton (properties.hpp, lb_dfa) takes one symbol per code point
// and gives the decision for the boundary before it and the next state. A
// few entries (0.01% of the code points of the measured text) depend on
// the next one or two bases: for those the look-ahead reads forward,
// passing over combining marks and ZWJ. Bound of one look-ahead: it reads
// the code points after the current one up to the second base at most; a
// code point is therefore read by the look-aheads of the two bases before
// it at most, and the walk as a whole reads each code point at most three
// times.

#include "algorithms/textlayout/properties.hpp"
#include "stxlib/types.hpp"

namespace stxlib::textlayout::kernel {

namespace tables = ::stxlib::textlayout::tables;

inline constexpr u32 state_bits = 0x0fffu;
inline constexpr u32 action_shift = 12;
inline constexpr u32 first_look = 3;

// The automaton's and the look tables' value ranges (A-stxlib-textlayout-01-F4):
// every entry's state is a row, every action at first_look or above names
// a row of lb_look_first, every lb_look_first entry is 0, 1 or 2 + a row of
// lb_look_second, and lb_look_second gives 0 or 1.
[[nodiscard]] constexpr bool automaton_in_range() noexcept {
    for (const u16 entry : tables::lb_dfa) {
        if ((entry & state_bits) >= tables::lb_dfa_state_count) return false;
        const u32 action = static_cast<u32>(entry) >> action_shift;
        if (action >= first_look && action - first_look >= tables::lb_look_first_count) return false;
    }
    for (const u8 (&row)[tables::lb_symbol_count + 1] : tables::lb_look_first) {
        for (const u8 decision : row) {
            if (decision >= 2 && decision - 2u >= tables::lb_look_second_count) return false;
        }
    }
    for (const u8 (&row)[tables::lb_symbol_count + 1] : tables::lb_look_second) {
        for (const u8 decision : row) {
            if (decision >= 2) return false;
        }
    }
    return true;
}

static_assert(tables::lb_dfa_letter_state < tables::lb_dfa_state_count && tables::lb_dfa_letter_symbol < tables::lb_symbol_count,
              "stx_textlayout: the letters form's state and symbol");
static_assert(automaton_in_range(), "stx_textlayout: the automaton and look tables keep their ranges");

// One code point of well-formed UTF-8 at `bytes`.
struct Decoded final {
    u32 value;
    u32 length;
};

// Requirement: `bytes` is at the first byte of a sequence of well-formed
// UTF-8 (a TextView's bytes), so its continuation bytes are there.
[[nodiscard]] inline Decoded decode(const unsigned char* const bytes) noexcept {
    const u32 lead = bytes[0];
    Decoded result{lead, 1};
    if (lead < 0x80u) return result;
    if (lead < 0xe0u) {
        result.value = ((lead & 0x1fu) << 6) | (bytes[1] & 0x3fu);
        result.length = 2;
        return result;
    }
    if (lead < 0xf0u) {
        result.value = ((lead & 0x0fu) << 12) | ((bytes[1] & 0x3fu) << 6) | (bytes[2] & 0x3fu);
        result.length = 3;
        return result;
    }
    result.value = ((lead & 0x07u) << 18) | ((bytes[1] & 0x3fu) << 12) | ((bytes[2] & 0x3fu) << 6) | (bytes[3] & 0x3fu);
    result.length = 4;
    return result;
}

// 1 for the bytes A-Z and a-z: one line-break symbol (plain AL), Latin,
// left to right (the generator checks all three, and that the symbol
// leads to lb_dfa_letter_state from every state and stays there with no
// break).
struct LetterTable final {
    u8 is_letter[256];
};

[[nodiscard]] constexpr LetterTable make_letter_table() noexcept {
    LetterTable table{};
    for (u32 byte = 0; byte < 256; ++byte) {
        const bool upper = byte >= 0x41u && byte <= 0x5au;
        const bool lower = byte >= 0x61u && byte <= 0x7au;
        table.is_letter[byte] = (upper || lower) ? 1 : 0;
    }
    return table;
}

inline constexpr LetterTable letters = make_letter_table();

// The symbol of the next base at or after a position, and where the
// search stopped (after that base).
struct Peek final {
    u32 symbol; // lb_symbol_count at the end of the text
    usize next;
};

// Over UTF-8. within_paragraph: a mark whose Bidi_Class is B (U+001C to
// U+001E) ends the paragraph, so the look-ahead ends there too.
// Bound: at most count - index steps.
[[nodiscard]] inline Peek peek_utf8(const unsigned char* const bytes, usize index, const usize count,
                                    const bool within_paragraph) noexcept {
    while (index < count) {
        const Decoded decoded = decode(bytes + index);
        const u32 properties = tables::record(decoded.value);
        const u32 symbol = tables::symbol_of(properties);
        index = index + decoded.length;
        if (tables::lb_symbol_mark[symbol] == 0) {
            const Peek found{symbol, index};
            return found;
        }
        if (within_paragraph && (properties & tables::class_bits) == 10u) break;
    }
    const Peek end{tables::lb_symbol_count, count};
    return end;
}

[[nodiscard]] inline Peek peek_code_points(const u32* const values, usize index, const usize count,
                                           const bool within_paragraph) noexcept {
    while (index < count) {
        const u32 properties = tables::record_of_input(values[index]);
        const u32 symbol = tables::symbol_of(properties);
        index = index + 1;
        if (tables::lb_symbol_mark[symbol] == 0) {
            const Peek found{symbol, index};
            return found;
        }
        if (within_paragraph && (properties & tables::class_bits) == 10u) break;
    }
    const Peek end{tables::lb_symbol_count, count};
    return end;
}

// The decision of a look-ahead entry (action >= first_look) for the code
// point that ends at `after`.
[[nodiscard]] inline u32 look_utf8(const u32 action, const unsigned char* const bytes, const usize after,
                                   const usize count, const bool within_paragraph) noexcept {
    const Peek one = peek_utf8(bytes, after, count, within_paragraph);
    u32 decision = tables::lb_look_first[action - first_look][one.symbol];
    if (decision >= 2) {
        const Peek two = peek_utf8(bytes, one.next, count, within_paragraph);
        decision = tables::lb_look_second[decision - 2][two.symbol];
    }
    return decision;
}

[[nodiscard]] inline u32 look_code_points(const u32 action, const u32* const values, const usize after,
                                          const usize count, const bool within_paragraph) noexcept {
    const Peek one = peek_code_points(values, after, count, within_paragraph);
    u32 decision = tables::lb_look_first[action - first_look][one.symbol];
    if (decision >= 2) {
        const Peek two = peek_code_points(values, one.next, count, within_paragraph);
        decision = tables::lb_look_second[decision - 2][two.symbol];
    }
    return decision;
}

} // namespace stxlib::textlayout::kernel
