#pragma once

// The line-break rule chain: UAX #14 (Unicode 18.0.0), rules LB2 to LB31
// in the specification's order, one code point at a time. This is the
// reference of stx_textlayout's line breaking: the library walks the
// automaton generated from the same reading (tools/
// gen_textlayout_tables.py), and tests/textlayout_tests.cpp
// proves the two equal over every reachable state of this chain and
// compares them on text. Header only; the library itself does not call it.
//
// A symbol is a code point's line-break class after LB1 with what the
// rules ask beyond the class: a quotation mark's General_Category (Pi,
// Pf), East Asian width F, W or H, U+25CC, an unassigned
// Extended_Pictographic (properties.hpp, lb_symbol_*).
//
// Rule LB12a is read as [^SP HY HH] x GL: LineBreakTest-18.0.0.txt keeps
// BA x GL together under rule 12.1 (its cases "3000 x 00A0", "0009 x
// 00A0"), where UAX #14 revision 55 still excepted BA.
//
// No loop except reference_line_breaks's own, which is bounded by its
// input (see there).

#include "algorithms/textlayout/properties.hpp"
#include "stxlib/types.hpp"

namespace stxlib::textlayout::chain {

namespace tables = ::stxlib::textlayout::tables;

// Class numbers (LineBreakClass's order).
inline constexpr u8 BK = 0, CR = 1, LF = 2, NL = 3, SP = 4, ZW = 5, WJ = 6, GL = 7, CM = 8, ZWJ = 9, OP = 10,
                    CL = 11, CP = 12, QU = 13, NS = 14, EX = 15, SY = 16, IS = 17, PR = 18, PO = 19, NU = 20,
                    AL = 21, HL = 22, ID = 23, IN = 24, HY = 25, HH = 26, BA = 27, BB = 28, B2 = 29, CB = 30,
                    H2 = 31, H3 = 32, JL = 33, JV = 34, JT = 35, RI = 36, EB = 37, EM = 38, AK = 39, AP = 40,
                    AS = 41, VF = 42, VI = 43;

// What stands for "no class": the start of text, as a class.
inline constexpr u8 none = 0xff;

inline constexpr u8 sub_initial = 1;
inline constexpr u8 sub_final = 2;
inline constexpr u8 sub_dotted = 3;
inline constexpr u8 sub_pictograph = 4;

// Decisions (LineBreak's values).
inline constexpr u8 no_break = 0;
inline constexpr u8 allowed = 1;
inline constexpr u8 mandatory = 2;

// A symbol position that holds no symbol: the start of text in a state,
// the end of text in a look-ahead.
inline constexpr u8 start_of_text = 0x7f;
inline constexpr u8 end_of_text = static_cast<u8>(tables::lb_symbol_count);

// The context a run of spaces keeps (LB8, LB14 to LB17).
inline constexpr u8 context_none = 0;
inline constexpr u8 context_zero_width = 1;
inline constexpr u8 context_open = 2;
inline constexpr u8 context_initial_quote = 3;
inline constexpr u8 context_close = 4;
inline constexpr u8 context_break_both = 5;

// What the rules need of the text before a boundary.
struct State final {
    u8 base{start_of_text};     // the last base: a character with the marks LB9 joins to it
    u8 before{start_of_text};   // the base before it
    u8 joiner{0};               // 1 when the last code point was ZWJ (LB8a)
    u8 context{context_none};
    u8 number{0};               // LB25: 1 inside NU (SY | IS)*, 2 after its CL or CP
    u8 odd_indicators{0};       // LB30a: 1 when `base` ends an odd run of RI
};

struct Step final {
    u8 decision{no_break};
    u8 looked{0}; // how many look-ahead symbols the rules read: 0, 1 or 2
    State next{};
};

[[nodiscard]] constexpr u8 class_of(const u8 symbol) noexcept {
    if (symbol >= tables::lb_symbol_count) return none;
    return tables::lb_symbol_class[symbol];
}

[[nodiscard]] constexpr u8 subtype_of(const u8 symbol) noexcept {
    if (symbol >= tables::lb_symbol_count) return 0;
    return tables::lb_symbol_subtype[symbol];
}

[[nodiscard]] constexpr bool east_asian(const u8 symbol) noexcept {
    if (symbol >= tables::lb_symbol_count) return false;
    return tables::lb_symbol_east_asian[symbol] != 0;
}

[[nodiscard]] constexpr bool aksara(const u8 symbol) noexcept {
    const u8 kind = class_of(symbol);
    return kind == AK || kind == AS || subtype_of(symbol) == sub_dotted;
}

[[nodiscard]] constexpr bool hard(const u8 kind) noexcept { return kind == BK || kind == CR || kind == LF || kind == NL; }

[[nodiscard]] constexpr bool letter(const u8 kind) noexcept { return kind == AL || kind == HL; }

[[nodiscard]] constexpr bool hyphen(const u8 kind) noexcept { return kind == HY || kind == HH; }

[[nodiscard]] constexpr bool prefix_or_postfix(const u8 kind) noexcept { return kind == PR || kind == PO; }

[[nodiscard]] constexpr bool jamo(const u8 kind) noexcept {
    return kind == JL || kind == JV || kind == JT || kind == H2 || kind == H3;
}

// The state after `base` becomes the last base.
[[nodiscard]] constexpr State advance(const State& state, const u8 base, const bool joiner) noexcept {
    const u8 a = class_of(state.base);
    const u8 b = class_of(base);
    State next{};
    next.base = base;
    next.before = state.base;
    next.joiner = joiner ? 1 : 0;
    next.context = state.context;
    if (b != SP) {
        const bool after_opening = a == none || hard(a) || a == OP || a == QU || a == GL || a == SP || a == ZW;
        if (b == ZW) {
            next.context = context_zero_width;
        } else if (b == OP) {
            next.context = context_open;
        } else if (b == QU && subtype_of(base) == sub_initial && after_opening) {
            next.context = context_initial_quote;
        } else if (b == CL || b == CP) {
            next.context = context_close;
        } else if (b == B2) {
            next.context = context_break_both;
        } else {
            next.context = context_none;
        }
    }
    if (b == NU) {
        next.number = 1;
    } else if (b == SY || b == IS) {
        next.number = state.number == 1 ? 1 : 0;
    } else if (b == CL || b == CP) {
        next.number = state.number == 1 ? 2 : 0;
    } else {
        next.number = 0;
    }
    next.odd_indicators = (b == RI && !(a == RI && state.odd_indicators != 0)) ? 1 : 0;
    return next;
}

// Rules LB11 to LB31 for the boundary between state.base and `base`,
// neither of them absent. first and second are the next two bases after
// `base` (end_of_text when there is none); `looked` records how far the
// rules read.
[[nodiscard]] constexpr u8 rules(const State& state, const u8 base, const u8 first, const u8 second,
                                 u8& looked) noexcept {
    const u8 a = class_of(state.base);
    const u8 b = class_of(base);
    const u8 p = class_of(state.before);
    const u8 a_sub = subtype_of(state.base);
    const u8 b_sub = subtype_of(base);
    if (a == WJ || b == WJ) return no_break;                                        // LB11
    if (a == GL) return no_break;                                                   // LB12
    if (b == GL && a != SP && a != HY && a != HH) return no_break;                  // LB12a
    if (b == CL || b == CP || b == EX || b == SY) return no_break;                  // LB13
    if (state.context == context_open) return no_break;                             // LB14
    if (state.context == context_initial_quote) return no_break;                    // LB15a
    if (b == QU && b_sub == sub_final) {                                            // LB15b
        looked = 1;
        const u8 f = class_of(first);
        if (f == none || f == SP || f == GL || f == WJ || f == CL || f == QU || f == CP || f == EX || f == IS ||
            f == SY || hard(f) || f == ZW) {
            return no_break;
        }
    }
    if (a == SP && b == IS) {                                                       // LB15c
        looked = 1;
        if (class_of(first) == NU) return allowed;
    }
    if (b == IS) return no_break;                                                   // LB15d
    if (state.context == context_close && b == NS) return no_break;                 // LB16
    if (state.context == context_break_both && b == B2) return no_break;            // LB17
    if (a == SP) return allowed;                                                    // LB18
    if (b == QU && b_sub != sub_initial) return no_break;                           // LB19
    if (a == QU && a_sub != sub_final) return no_break;                             // LB19
    if (b == QU) {                                                                  // LB19a
        if (!east_asian(state.base)) return no_break;
        looked = 1;
        if (first == end_of_text || !east_asian(first)) return no_break;
    }
    if (a == QU) {                                                                  // LB19a
        if (!east_asian(base)) return no_break;
        if (state.before == start_of_text || !east_asian(state.before)) return no_break;
    }
    if (a == CB || b == CB) return allowed;                                         // LB20
    const bool word_start = p == none || hard(p) || p == SP || p == ZW || p == CB || p == GL;
    if (hyphen(a) && word_start && letter(b)) return no_break;                      // LB20a
    if (b == BA || b == HH || b == HY || b == NS || a == BB) return no_break;       // LB21
    if (p == HL && hyphen(a) && b != HL) return no_break;                           // LB21a
    if (a == SY && b == HL) return no_break;                                        // LB21b
    if (b == IN) return no_break;                                                   // LB22
    if ((letter(a) && b == NU) || (a == NU && letter(b))) return no_break;          // LB23
    if (a == PR && (b == ID || b == EB || b == EM)) return no_break;                // LB23a
    if ((a == ID || a == EB || a == EM) && b == PO) return no_break;                // LB23a
    if (prefix_or_postfix(a) && letter(b)) return no_break;                         // LB24
    if (letter(a) && prefix_or_postfix(b)) return no_break;                         // LB24
    if (state.number != 0 && prefix_or_postfix(b)) return no_break;                 // LB25: NU (SY|IS)* (CL|CP)? x (PO|PR)
    if (prefix_or_postfix(a) && b == OP) {                                          // LB25: (PO|PR) x OP NU, x OP IS NU
        looked = 1;
        const u8 f = class_of(first);
        if (f == NU) return no_break;
        if (f == IS) {
            looked = 2;
            if (class_of(second) == NU) return no_break;
        }
    }
    if ((prefix_or_postfix(a) || a == HY || a == IS) && b == NU) return no_break;   // LB25
    if (state.number == 1 && b == NU) return no_break;                              // LB25: NU (SY|IS)* x NU
    if (a == JL && (b == JL || b == JV || b == H2 || b == H3)) return no_break;     // LB26
    if ((a == JV || a == H2) && (b == JV || b == JT)) return no_break;              // LB26
    if ((a == JT || a == H3) && b == JT) return no_break;                           // LB26
    if (jamo(a) && b == PO) return no_break;                                        // LB27
    if (a == PR && jamo(b)) return no_break;                                        // LB27
    if (letter(a) && letter(b)) return no_break;                                    // LB28
    const bool a_aksara = aksara(state.base);
    const bool b_aksara = aksara(base);
    if (a == AP && b_aksara) return no_break;                                       // LB28a
    if (a_aksara && (b == VF || b == VI)) return no_break;                          // LB28a
    if (state.before != start_of_text && aksara(state.before) && a == VI && (b == AK || b_sub == sub_dotted)) {
        return no_break;                                                            // LB28a
    }
    if (a_aksara && b_aksara) {                                                     // LB28a
        if (looked < 1) looked = 1;
        if (class_of(first) == VF) return no_break;
    }
    if (a == IS && letter(b)) return no_break;                                      // LB29
    if ((letter(a) || a == NU) && b == OP && !east_asian(base)) return no_break;    // LB30
    if (a == CP && !east_asian(state.base) && (letter(b) || b == NU)) return no_break; // LB30
    if (a == RI && state.odd_indicators != 0 && b == RI) return no_break;           // LB30a
    if (a == EB && b == EM) return no_break;                                        // LB30b
    if (a_sub == sub_pictograph && b == EM) return no_break;                        // LB30b
    return allowed;                                                                 // LB31
}

// The decision for the boundary before `symbol` and the state after it.
// first and second: the symbols of the next two bases after `symbol`,
// marks passed over (end_of_text when there is none). Requirement: symbol
// < lb_symbol_count.
[[nodiscard]] constexpr Step step(const State& state, const u8 symbol, const u8 first, const u8 second) noexcept {
    const u8 c = class_of(symbol);
    const bool mark = c == CM || c == ZWJ;
    // LB10: a mark that joins nothing is AL (its East Asian width kept).
    const u8 base = mark ? tables::lb_symbol_plain_al[east_asian(symbol) ? 1 : 0] : symbol;
    Step result{};
    result.next = advance(state, base, c == ZWJ);
    const u8 a = class_of(state.base);
    if (a == none) return result;                                                   // LB2
    if (a == BK || a == LF || a == NL || (a == CR && c != LF)) {                    // LB4, LB5
        result.decision = mandatory;
        return result;
    }
    if (a == CR) return result;                                                     // LB5: CR x LF
    if (hard(c)) return result;                                                     // LB6
    if (c == SP || c == ZW) return result;                                          // LB7
    if (state.context == context_zero_width) {                                      // LB8
        result.decision = allowed;
        return result;
    }
    if (mark && a != SP && a != ZW) {                                               // LB9 (and LB8a before a mark)
        result.next = state;
        result.next.joiner = c == ZWJ ? 1 : 0;
        return result;
    }
    if (state.joiner != 0) return result;                                           // LB8a
    result.decision = rules(state, base, first, second, result.looked);
    return result;
}

// The break opportunities of symbols[0, count), one decision per symbol:
// output[i] for the boundary after symbol i, the last mandatory (LB3).
// Bound: the outer loop runs count times; each look-ahead search runs
// forward to the next base, at most count steps, so count * count in all
// (a reference, for tests). Requirement: output holds count entries and
// every symbol is below lb_symbol_count.
constexpr void reference_line_breaks(const u8* const symbols, const usize count, u8* const output) noexcept {
    State state{};
    for (usize index = 0; index < count; ++index) {
        u8 look[2] = {end_of_text, end_of_text};
        usize cursor = index + 1;
        for (u32 which = 0; which < 2; ++which) {
            while (cursor < count && tables::lb_symbol_mark[symbols[cursor]] != 0) cursor = cursor + 1;
            if (cursor >= count) break;
            look[which] = symbols[cursor];
            cursor = cursor + 1;
        }
        const Step result = step(state, symbols[index], look[0], look[1]);
        if (index > 0) output[index - 1] = result.decision;
        state = result.next;
    }
    if (count > 0) output[count - 1] = mandatory;
}

} // namespace stxlib::textlayout::chain
