// Line breaking over a whole text (include/stxlib/textlayout.hpp,
// find_line_breaks) and the per-code-point queries of the line-break and
// script properties. The automaton and its look-ahead are in
// line_break_kernel.hpp; the rule chain it was generated from, and is
// proved equal to, in line_break_chain.hpp.
//
// The text is walked once. Its items come in two forms, each with its own
// loop (the performance focus study's method: split by form, one fixed
// loop per form, measure first; tools/gen_textlayout_tables.py --stats on
// the CommonMark specification and this repository's documents: AL 78.8%,
// SP 10.7%, LF 4.5% of the code points; no break 84.7% of the decisions,
// by look-ahead 0.011%):
//   letters   a run of ASCII letters after its first: every boundary
//             inside is "no break" and the automaton stays in the letter
//             state, so the run is measured by one search and its
//             decisions are one fill;
//   general   one code point: its symbol, one table entry (decision and
//             next state), one store.
// Nothing is counted in memory: the position and the automaton's state
// are locals.

#include "stxlib/textlayout.hpp"

#include "algorithms/textlayout/line_break_kernel.hpp"
#include "algorithms/textlayout/properties.hpp"
#include "stxlib/error.hpp"

namespace stxlib::textlayout {

namespace {

namespace tables = ::stxlib::textlayout::tables;
namespace kernel = ::stxlib::textlayout::kernel;

static_assert(sizeof(LineBreak) == 1, "stxlib::textlayout: a LineBreak is one byte");

void fill_breaks(LineBreak* const target, const usize length, const LineBreak value) noexcept {
    for (usize index = 0; index < length; ++index) target[index] = value;
}

// Requirement: bytes[0, count) is well-formed UTF-8, count > 0, output
// holds count entries.
void break_utf8(const unsigned char* const bytes, const usize count, LineBreak* const output) noexcept {
    const u32* const ascii = tables::ascii_records;
    const u8* const is_letter = kernel::letters.is_letter;
    u32 state = 0;
    usize index = 0;
    // The first code point: the boundary before it is the start of text.
    {
        const kernel::Decoded decoded = kernel::decode(bytes);
        const u32 symbol = tables::symbol_of(tables::record(decoded.value));
        state = tables::lb_dfa[symbol] & kernel::state_bits;
        fill_breaks(output, decoded.length - 1u, LineBreak::inside_character);
        index = decoded.length;
    }
    // Each step decides the boundary before the code point at `index`,
    // which is after byte index - 1. Bound: index rises by at least one
    // each step.
    while (index < count) {
        const u32 lead = bytes[index];
        if (lead < 0x80u) {
            const u32 symbol = tables::symbol_of(ascii[lead]);
            const u32 entry = tables::lb_dfa[(state << tables::lb_dfa_shift) | symbol];
            u32 decision = entry >> kernel::action_shift;
            state = entry & kernel::state_bits;
            if (decision >= kernel::first_look) decision = kernel::look_utf8(decision, bytes, index + 1u, count, false);
            output[index - 1u] = static_cast<LineBreak>(decision);
            index = index + 1u;
            if (is_letter[lead] == 0) continue;
            // The letters form: the rest of the run of letters.
            usize end = index;
            while (end < count && is_letter[bytes[end]] != 0) end = end + 1u;
            fill_breaks(output + (index - 1u), end - index, LineBreak::prohibited);
            index = end;
            continue;
        }
        const kernel::Decoded decoded = kernel::decode(bytes + index);
        const u32 symbol = tables::symbol_of(tables::record(decoded.value));
        const u32 entry = tables::lb_dfa[(state << tables::lb_dfa_shift) | symbol];
        u32 decision = entry >> kernel::action_shift;
        state = entry & kernel::state_bits;
        const usize after = index + decoded.length;
        if (decision >= kernel::first_look) decision = kernel::look_utf8(decision, bytes, after, count, false);
        output[index - 1u] = static_cast<LineBreak>(decision);
        fill_breaks(output + index, decoded.length - 1u, LineBreak::inside_character);
        index = after;
    }
    output[count - 1u] = LineBreak::mandatory; // LB3
}

// Requirement: count > 0, output holds count entries.
void break_code_points(const u32* const values, const usize count, LineBreak* const output) noexcept {
    u32 state = tables::lb_dfa[tables::symbol_of(tables::record_of_input(values[0]))] & kernel::state_bits;
    // Bound: count - 1 steps.
    for (usize index = 1; index < count; ++index) {
        const u32 symbol = tables::symbol_of(tables::record_of_input(values[index]));
        const u32 entry = tables::lb_dfa[(state << tables::lb_dfa_shift) | symbol];
        u32 decision = entry >> kernel::action_shift;
        state = entry & kernel::state_bits;
        if (decision >= kernel::first_look) {
            decision = kernel::look_code_points(decision, values, index + 1u, count, false);
        }
        output[index - 1u] = static_cast<LineBreak>(decision);
    }
    output[count - 1u] = LineBreak::mandatory; // LB3
}

} // namespace

LineBreakClass line_break_class(const u32 code_point) noexcept {
    if (code_point > tables::highest_code_point) return LineBreakClass::alphabetic;
    const u32 symbol = tables::symbol_of(tables::record(code_point));
    const LineBreakClass result = static_cast<LineBreakClass>(tables::lb_symbol_class[symbol]);
    return result;
}

bool east_asian_wide(const u32 code_point) noexcept {
    if (code_point > tables::highest_code_point) return false;
    const u32 symbol = tables::symbol_of(tables::record(code_point));
    const bool result = tables::lb_symbol_east_asian[symbol] != 0;
    return result;
}

Script script_of(const u32 code_point) noexcept {
    if (code_point > tables::highest_code_point) return Script::unknown;
    const Script result = static_cast<Script>(tables::script_in(tables::record(code_point)));
    return result;
}

bool script_extends_to(const u32 code_point, const Script script) noexcept {
    if (code_point > tables::highest_code_point) return script == Script::unknown;
    const bool result = tables::script_extends(tables::record(code_point), static_cast<u32>(script));
    return result;
}

u32 script_tag(const Script script) noexcept {
    const u32 index = static_cast<u32>(script);
    if (index >= tables::script_count) return tables::script_tags[2];
    const u32 result = tables::script_tags[index];
    return result;
}

Status find_line_breaks(const MutableView<LineBreak> output, const TextView text) noexcept {
    if (output.count() != text.count()) {
        return Status::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.textlayout.find_line_breaks"));
    }
    if (text.count() == 0) return Status::success();
    const unsigned char* const bytes = reinterpret_cast<const unsigned char*>(text.data());
    break_utf8(bytes, text.count(), output.data());
    return Status::success();
}

Status find_line_breaks(const MutableView<LineBreak> output, const View<u32> code_points) noexcept {
    if (output.count() != code_points.count()) {
        return Status::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.textlayout.find_line_breaks"));
    }
    if (code_points.count() == 0) return Status::success();
    break_code_points(code_points.data(), code_points.count(), output.data());
    return Status::success();
}

} // namespace stxlib::textlayout
