// Tests of include/stxlib/textlayout.hpp: line breaking (UAX #14), scripts
// (UAX #24) and the one walk of analyse_paragraph. The bidi rules have
// their own program, tests/textlayout_bidi_tests.cpp.
//
// Arguments: <unicode directory> [<conformance directory>] [--exhaustive N]
// [--random N] [--fuzz N]. The unicode directory is
// third_party/unicode-18.0.0; the conformance directory holds
// LineBreakTest.txt (kept off the repository: PROVENANCE.md). Without it
// T4 is reported as not run, plainly, and the rest still runs.
//
// The references are the Unicode files, read here by parsers of the test's
// own (independent of tools/gen_textlayout_tables.py); the rule chain
// (src/algorithms/textlayout/line_break_chain.hpp); and a script itemiser
// written here from the header's statement of the rules.
//
// Properties:
//   T1  every code point 0 to 0x10FFFF: line_break_class is LineBreak.txt's
//       value after LB1 (General_Category from UnicodeData.txt for SA);
//       east_asian_wide is EastAsianWidth.txt's F, W or H; script_of is one
//       value per Scripts.txt name and one name per value;
//       script_extends_to is ScriptExtensions.txt's set (by ISO 15924
//       code, through script_tag) or the Script alone. Exhaustive.
//   T2  the automaton equals the rule chain: from the start of text, every
//       reachable pair (chain state, automaton state) is visited; at each,
//       for every symbol, the automaton's decision equals the chain's for
//       every look-ahead the chain reads (every next base, and every base
//       after it where the chain reads two), and the next states pair up
//       again. Exhaustive over the reachable states: a proof by
//       enumeration that the two agree on every symbol sequence of every
//       length.
//   T3  the letters form's facts: from every automaton state the ASCII
//       letters' symbol leads to the letter state, and in it gives "no
//       break" and stays. Exhaustive.
//   T4  LineBreakTest.txt: every case, through the chain, through
//       find_line_breaks over code points and over UTF-8. Pass count
//       reported.
//   T5  find_line_breaks over code points equals the chain on every
//       sequence of symbols of length 1 to N (default 3; one code point
//       per symbol), and on random sequences; UTF-8 equals code points.
//   T6  analyse_paragraph: over random texts of several paragraphs, each
//       paragraph's breaks are find_line_breaks of the paragraph alone;
//       its script runs are the test's itemiser's; UTF-8 and code points
//       agree; a paragraph longer than two blocks of the walk included.
//   T7  a bounded fuzz: random bytes (kept when validate_utf8 accepts
//       them) and random scalar values from the whole range, through
//       every entry, checking the structural invariants. Sampled.
//   T8  arguments.
// T1 to T3 and T5's first part are exhaustive over their stated domains;
// T4 is the conformance suite; T5's random part, T6 and T7 are sampled.

#include "stxlib/text.hpp"
#include "stxlib/textlayout.hpp"

#include "algorithms/textlayout/line_break_chain.hpp"
#include "algorithms/textlayout/properties.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

namespace layout = stxlib::textlayout;
namespace chain = stxlib::textlayout::chain;
namespace tables = stxlib::textlayout::tables;

using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

constexpr u32 code_points = 0x110000;

std::vector<std::string> read_lines(const std::string& path, bool& found) {
    std::vector<std::string> lines;
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    found = file != nullptr;
    if (file == nullptr) return lines;
    std::string current;
    char buffer[65536];
    while (true) {
        const usize got = std::fread(buffer, 1, sizeof buffer, file);
        if (got == 0) break;
        for (usize index = 0; index < got; ++index) {
            if (buffer[index] == '\n') {
                lines.push_back(current);
                current.clear();
            } else if (buffer[index] != '\r') {
                current.push_back(buffer[index]);
            }
        }
    }
    if (!current.empty()) lines.push_back(current);
    std::fclose(file);
    return lines;
}

std::string trimmed(const std::string& text) {
    usize begin = 0;
    usize end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) begin = begin + 1;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) end = end - 1;
    return text.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string& line, const char separator) {
    std::vector<std::string> pieces;
    std::string piece;
    for (const char character : line) {
        if (character == separator) {
            pieces.push_back(trimmed(piece));
            piece.clear();
        } else {
            piece.push_back(character);
        }
    }
    pieces.push_back(trimmed(piece));
    return pieces;
}

std::string strip_comment(const std::string& line) {
    const usize hash = line.find('#');
    return hash == std::string::npos ? line : line.substr(0, hash);
}

u32 hex(const std::string& value) { return static_cast<u32>(std::strtoul(value.c_str(), nullptr, 16)); }

void span_of(const std::string& text, u32& low, u32& high) {
    const usize dots = text.find("..");
    if (dots == std::string::npos) {
        low = hex(text);
        high = low;
        return;
    }
    low = hex(text.substr(0, dots));
    high = hex(text.substr(dots + 2));
}

// A property of every code point from a UCD listing: the default, then the
// file's @missing lines in order, then its listing.
std::vector<std::string> read_property(const std::string& path, const std::string& fallback, bool& found) {
    std::vector<std::string> values(code_points, fallback);
    const std::vector<std::string> lines = read_lines(path, found);
    for (const std::string& line : lines) {
        if (line.rfind("# @missing:", 0) != 0) continue;
        const std::vector<std::string> fields = split(strip_comment(line.substr(11)), ';');
        if (fields.size() < 2 || fields[1] == "<script>") continue;
        u32 low = 0;
        u32 high = 0;
        span_of(fields[0], low, high);
        for (u32 code = low; code <= high && code < code_points; ++code) values[code] = fields[1];
    }
    for (const std::string& line : lines) {
        const std::string body = trimmed(strip_comment(line));
        if (body.empty()) continue;
        const std::vector<std::string> fields = split(body, ';');
        if (fields.size() < 2) continue;
        u32 low = 0;
        u32 high = 0;
        span_of(fields[0], low, high);
        for (u32 code = low; code <= high && code < code_points; ++code) values[code] = fields[1];
    }
    return values;
}

const char* const class_names[layout::line_break_class_count] = {
    "BK", "CR", "LF", "NL", "SP", "ZW", "WJ", "GL", "CM", "ZWJ", "OP", "CL", "CP", "QU", "NS",
    "EX", "SY", "IS", "PR", "PO", "NU", "AL", "HL", "ID", "IN",  "HY", "HH", "BA", "BB", "B2",
    "CB", "H2", "H3", "JL", "JV", "JT", "RI", "EB", "EM", "AK",  "AP", "AS", "VF", "VI",
};

std::string tag_text(const u32 tag) {
    std::string result;
    result.push_back(static_cast<char>((tag >> 24) & 0xffu));
    result.push_back(static_cast<char>((tag >> 16) & 0xffu));
    result.push_back(static_cast<char>((tag >> 8) & 0xffu));
    result.push_back(static_cast<char>(tag & 0xffu));
    return result;
}

std::uint64_t reported = 0;

void report(const char* const property, const std::string& detail) {
    reported = reported + 1;
    if (reported <= 40) std::fprintf(stderr, "%s: %s\n", property, detail.c_str());
}

std::string code_text(const u32 code) {
    char piece[16];
    std::snprintf(piece, sizeof piece, "U+%04X", static_cast<unsigned>(code));
    return piece;
}

// --- T1 -------------------------------------------------------------------

bool properties(const std::string& directory) {
    bool found = false;
    const std::vector<std::string> raw = read_property(directory + "/LineBreak.txt", "XX", found);
    if (!found) return false;
    std::vector<std::string> width = read_property(directory + "/EastAsianWidth.txt", "N", found);
    if (!found) return false;
    const std::vector<std::string> scripts = read_property(directory + "/Scripts.txt", "Unknown", found);
    if (!found) return false;
    std::vector<std::string> category(code_points, "Cn");
    const std::vector<std::string> unicode = read_lines(directory + "/UnicodeData.txt", found);
    if (!found) return false;
    u32 first = 0;
    for (const std::string& line : unicode) {
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 3) continue;
        const u32 code = hex(fields[0]);
        if (fields[1].find(", First>") != std::string::npos) {
            first = code;
            continue;
        }
        const u32 low = fields[1].find(", Last>") != std::string::npos ? first : code;
        for (u32 value = low; value <= code && value < code_points; ++value) category[value] = fields[2];
    }
    std::uint64_t mismatches = 0;
    std::map<std::string, int> value_of_name;
    std::map<int, std::string> name_of_value;
    for (u32 code = 0; code < code_points; ++code) {
        std::string expected = raw[code];
        if (expected == "AI" || expected == "SG" || expected == "XX") expected = "AL";
        if (expected == "SA") expected = (category[code] == "Mn" || category[code] == "Mc") ? "CM" : "AL";
        if (expected == "CJ") expected = "NS";
        const u32 got = static_cast<u32>(layout::line_break_class(code));
        const bool class_ok = got < layout::line_break_class_count && expected == class_names[got];
        if (width[code] == "Wide") width[code] = "W";
        const bool wide = width[code] == "F" || width[code] == "W" || width[code] == "H";
        const bool width_ok = layout::east_asian_wide(code) == wide;
        const int script = static_cast<int>(layout::script_of(code));
        const std::map<std::string, int>::iterator by_name = value_of_name.find(scripts[code]);
        bool script_ok = true;
        if (by_name == value_of_name.end()) {
            script_ok = name_of_value.find(script) == name_of_value.end();
            value_of_name[scripts[code]] = script;
            name_of_value[script] = scripts[code];
        } else {
            script_ok = by_name->second == script;
        }
        if (!(class_ok && width_ok && script_ok)) {
            mismatches = mismatches + 1;
            report("T1", code_text(code) + " class " + expected + " width " + width[code] + " script " + scripts[code]);
        }
    }
    CHECK(value_of_name["Common"] == 0 && value_of_name["Inherited"] == 1 && value_of_name["Unknown"] == 2);
    CHECK(tag_text(layout::script_tag(layout::script_of(0x41))) == "Latn");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x3b1))) == "Grek");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x416))) == "Cyrl");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x5d0))) == "Hebr");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x627))) == "Arab");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x915))) == "Deva");
    CHECK(tag_text(layout::script_tag(layout::script_of(0xe01))) == "Thai");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x3042))) == "Hira");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x30a2))) == "Kana");
    CHECK(tag_text(layout::script_tag(layout::script_of(0x4e00))) == "Hani");
    CHECK(tag_text(layout::script_tag(layout::script_of(0xac00))) == "Hang");
    CHECK(tag_text(layout::script_tag(layout::Script::common)) == "Zyyy");
    CHECK(tag_text(layout::script_tag(layout::Script::inherited)) == "Zinh");
    CHECK(tag_text(layout::script_tag(layout::Script::unknown)) == "Zzzz");
    CHECK(tag_text(layout::script_tag(static_cast<layout::Script>(255))) == "Zzzz");
    CHECK(layout::line_break_class(0x110000) == layout::LineBreakClass::alphabetic && !layout::east_asian_wide(0x110000));
    CHECK(layout::script_of(0x110000) == layout::Script::unknown);

    // Script_Extensions: the listed sets by ISO 15924 code, else the Script alone.
    std::vector<std::string> extensions(code_points);
    const std::vector<std::string> listed = read_lines(directory + "/ScriptExtensions.txt", found);
    if (!found) return false;
    for (const std::string& line : listed) {
        const std::string body = trimmed(strip_comment(line));
        if (body.empty()) continue;
        const std::vector<std::string> fields = split(body, ';');
        if (fields.size() < 2) continue;
        u32 low = 0;
        u32 high = 0;
        span_of(fields[0], low, high);
        for (u32 code = low; code <= high && code < code_points; ++code) extensions[code] = " " + fields[1] + " ";
    }
    const u32 script_values = static_cast<u32>(name_of_value.size());
    for (u32 code = 0; code < code_points; ++code) {
        const layout::Script own = layout::script_of(code);
        bool ok = layout::script_extends_to(code, own);
        if (extensions[code].empty()) {
            const layout::Script other = static_cast<layout::Script>((static_cast<u32>(own) + 1u) % script_values);
            ok = ok && !layout::script_extends_to(code, other);
            if (own != layout::Script::common) ok = ok && !layout::script_extends_to(code, layout::Script::common);
        } else {
            for (u32 value = 0; value < 256; ++value) {
                const layout::Script script = static_cast<layout::Script>(value);
                const bool in_set = value < script_values &&
                                    extensions[code].find(" " + tag_text(layout::script_tag(script)) + " ") != std::string::npos;
                if (layout::script_extends_to(code, script) != (in_set || script == own)) ok = false;
            }
        }
        if (!ok) {
            mismatches = mismatches + 1;
            report("T1", code_text(code) + " Script_Extensions");
        }
    }
    CHECK(mismatches == 0);
    std::printf("T1: %u code points checked (class, width, script, extensions; %u scripts), %llu mismatches\n",
                code_points, script_values, static_cast<unsigned long long>(mismatches));
    return true;
}

// --- T2, T3 ----------------------------------------------------------------

u32 key_of(const chain::State& state) {
    const u32 key = u32{state.base} | (u32{state.before} << 7) | (u32{state.joiner} << 14) | (u32{state.context} << 15) |
                    (u32{state.number} << 18) | (u32{state.odd_indicators} << 20);
    return key;
}

struct Pair final {
    chain::State state;
    u32 automaton;
};

void automaton_equals_chain() {
    std::vector<u8> bases;
    for (u32 symbol = 0; symbol < tables::lb_symbol_count; ++symbol) {
        if (tables::lb_symbol_mark[symbol] == 0) bases.push_back(static_cast<u8>(symbol));
    }
    bases.push_back(chain::end_of_text);
    std::unordered_map<u32, u32> automaton_of;
    std::vector<Pair> order;
    const chain::State start{};
    automaton_of[key_of(start)] = 0;
    order.push_back(Pair{start, 0});
    std::vector<bool> automaton_reached(tables::lb_dfa_state_count, false);
    automaton_reached[0] = true;
    std::uint64_t compared = 0;
    std::uint64_t differ = 0;
    for (usize position = 0; position < order.size(); ++position) {
        const Pair pair = order[position];
        for (u32 symbol = 0; symbol < tables::lb_symbol_count; ++symbol) {
            const u8 s = static_cast<u8>(symbol);
            const u32 entry = tables::lb_dfa[(pair.automaton << tables::lb_dfa_shift) | symbol];
            const u32 action = entry >> 12;
            const u32 target = entry & 0x0fffu;
            const chain::Step plain = chain::step(pair.state, s, chain::end_of_text, chain::end_of_text);
            if (plain.looked == 0) {
                compared = compared + 1;
                if (action != plain.decision) differ = differ + 1;
            } else {
                for (const u8 one : bases) {
                    const chain::Step first = chain::step(pair.state, s, one, chain::end_of_text);
                    const u32 by_first = action >= 3 ? tables::lb_look_first[action - 3][one] : action;
                    if (key_of(first.next) != key_of(plain.next)) differ = differ + 1;
                    if (first.looked < 2 && by_first < 2) {
                        compared = compared + 1;
                        if (by_first != first.decision) differ = differ + 1;
                        continue;
                    }
                    for (const u8 two : bases) {
                        const chain::Step second = chain::step(pair.state, s, one, two);
                        const u32 by_second = by_first >= 2 ? tables::lb_look_second[by_first - 2][two] : by_first;
                        compared = compared + 1;
                        if (by_second != second.decision) differ = differ + 1;
                    }
                }
            }
            if (target >= tables::lb_dfa_state_count) {
                differ = differ + 1;
                continue;
            }
            const u32 key = key_of(plain.next);
            const std::unordered_map<u32, u32>::iterator known = automaton_of.find(key);
            if (known == automaton_of.end()) {
                automaton_of[key] = target;
                automaton_reached[target] = true;
                order.push_back(Pair{plain.next, target});
            } else if (known->second != target) {
                differ = differ + 1;
            }
        }
    }
    u32 reached = 0;
    for (const bool value : automaton_reached) reached = reached + (value ? 1u : 0u);
    CHECK(differ == 0);
    CHECK(reached == tables::lb_dfa_state_count);
    std::printf("T2: %zu chain states paired with %u of %u automaton states, %llu decisions compared, %llu differ\n",
                order.size(), reached, tables::lb_dfa_state_count, static_cast<unsigned long long>(compared),
                static_cast<unsigned long long>(differ));

    // T3.
    u32 wrong = 0;
    for (u32 state = 0; state < tables::lb_dfa_state_count; ++state) {
        const u32 entry = tables::lb_dfa[(state << tables::lb_dfa_shift) | tables::lb_dfa_letter_symbol];
        if ((entry & 0x0fffu) != tables::lb_dfa_letter_state) wrong = wrong + 1;
    }
    const u32 inside = tables::lb_dfa[(tables::lb_dfa_letter_state << tables::lb_dfa_shift) | tables::lb_dfa_letter_symbol];
    if (inside != tables::lb_dfa_letter_state) wrong = wrong + 1; // decision 0 and the letter state
    for (u32 letter = 0; letter < 128; ++letter) {
        const bool is_letter = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
        if (!is_letter) continue;
        const u32 record = tables::ascii_records[letter];
        if (tables::symbol_of(record) != tables::lb_dfa_letter_symbol || tables::script_in(record) != tables::script_latin ||
            (record & tables::class_bits) != 0 || (record >> tables::extension_shift) != 0) {
            wrong = wrong + 1;
        }
    }
    CHECK(wrong == 0);
    std::printf("T3: the letters form's facts over %u states and 52 letters, %u wrong\n", tables::lb_dfa_state_count, wrong);
}

// --- running the three line breakers ---------------------------------------

void append_utf8(std::string& bytes, const u32 value) {
    if (value < 0x80u) {
        bytes.push_back(static_cast<char>(value));
    } else if (value < 0x800u) {
        bytes.push_back(static_cast<char>(0xc0u | (value >> 6)));
        bytes.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else if (value < 0x10000u) {
        bytes.push_back(static_cast<char>(0xe0u | (value >> 12)));
        bytes.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
        bytes.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else {
        bytes.push_back(static_cast<char>(0xf0u | (value >> 18)));
        bytes.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3fu)));
        bytes.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
        bytes.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    }
}

bool scalar(const u32 value) { return value <= 0x10ffffu && !(value >= 0xd800u && value <= 0xdfffu); }

std::vector<u8> by_chain(const std::vector<u32>& input) {
    std::vector<u8> symbols;
    for (const u32 value : input) symbols.push_back(static_cast<u8>(tables::symbol_of(tables::record_of_input(value))));
    std::vector<u8> output(input.size(), 0);
    chain::reference_line_breaks(symbols.data(), symbols.size(), output.data());
    return output;
}

std::vector<u8> by_code_points(const std::vector<u32>& input) {
    std::vector<layout::LineBreak> breaks(input.size(), layout::LineBreak::inside_character);
    const stxlib::OperationStatus status =
        layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(breaks.data(), breaks.size()),
                                 stxlib::View<u32>(input.data(), input.size()));
    std::vector<u8> output;
    if (status.failed()) return output;
    for (const layout::LineBreak value : breaks) output.push_back(static_cast<u8>(value));
    return output;
}

// Per code point, from the per-byte result; empty when the structure is
// wrong (inside_character anywhere but before a continuation byte).
std::vector<u8> by_utf8(const std::vector<u32>& input) {
    std::string bytes;
    std::vector<usize> last_byte;
    for (const u32 value : input) {
        append_utf8(bytes, value);
        last_byte.push_back(bytes.size() - 1);
    }
    std::vector<u8> output;
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    if (validated.failed()) return output;
    std::vector<layout::LineBreak> breaks(bytes.size(), layout::LineBreak::prohibited);
    const stxlib::OperationStatus status = layout::find_line_breaks(
        stxlib::MutableView<layout::LineBreak>(breaks.data(), breaks.size()), *validated.value());
    if (status.failed()) return output;
    usize next = 0;
    for (usize byte = 0; byte < bytes.size(); ++byte) {
        const bool ends = byte == last_byte[next];
        if (ends != (breaks[byte] != layout::LineBreak::inside_character)) return std::vector<u8>();
        if (ends) {
            output.push_back(static_cast<u8>(breaks[byte]));
            next = next + 1;
        }
    }
    return output;
}

std::string describe(const std::vector<u32>& input) {
    std::string result;
    for (const u32 value : input) {
        char piece[16];
        std::snprintf(piece, sizeof piece, "%04X ", static_cast<unsigned>(value));
        result += piece;
    }
    return result;
}

// --- T4 -------------------------------------------------------------------

void line_break_test(const std::string& directory) {
    bool found = false;
    const std::vector<std::string> lines = read_lines(directory + "/LineBreakTest.txt", found);
    if (!found) {
        std::printf("T4: NOT RUN: %s/LineBreakTest.txt absent\n", directory.c_str());
        return;
    }
    std::uint64_t cases = 0;
    std::uint64_t passed = 0;
    for (const std::string& line : lines) {
        const std::string body = strip_comment(line);
        std::vector<u32> input;
        std::vector<u8> expected;
        usize index = 0;
        bool first_mark = true;
        while (index < body.size()) {
            const unsigned char character = static_cast<unsigned char>(body[index]);
            if (character == 0xc3u && index + 1 < body.size()) {
                // U+00D7 (no break) is C3 97; U+00F7 (break) is C3 B7.
                const bool breaks = static_cast<unsigned char>(body[index + 1]) == 0xb7u;
                if (!first_mark) expected.push_back(breaks ? 1 : 0);
                first_mark = false;
                index = index + 2;
                continue;
            }
            if (character == ' ' || character == '\t') {
                index = index + 1;
                continue;
            }
            usize end = index;
            while (end < body.size() && body[end] != ' ' && body[end] != '\t') end = end + 1;
            input.push_back(hex(body.substr(index, end - index)));
            index = end;
        }
        if (input.empty() || expected.size() != input.size()) continue;
        cases = cases + 1;
        const std::vector<u8> reference = by_chain(input);
        const std::vector<u8> library = by_code_points(input);
        bool ok = reference.size() == input.size() && library == reference;
        for (usize position = 0; ok && position < input.size(); ++position) {
            if ((reference[position] != 0) != (expected[position] != 0)) ok = false;
        }
        bool all_scalar = true;
        for (const u32 value : input) all_scalar = all_scalar && scalar(value);
        if (ok && all_scalar) ok = by_utf8(input) == reference;
        if (ok && reference.back() != 2) ok = false;
        if (ok) {
            passed = passed + 1;
        } else {
            report("T4", describe(input));
        }
    }
    CHECK(cases > 0 && passed == cases);
    std::printf("T4: LineBreakTest.txt: %llu of %llu cases pass\n", static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(cases));
}

// --- T5 -------------------------------------------------------------------

struct Random final {
    std::uint64_t state{0x9e3779b97f4a7c15u};
    u32 next(const u32 bound) {
        // Shifts of the masked state, so no bit is shifted out (the
        // integer sanitizer reports that even for unsigned values).
        state ^= (state & 0x0007ffffffffffffu) << 13;
        state ^= state >> 7;
        state ^= (state & 0x00007fffffffffffu) << 17;
        const u32 result = static_cast<u32>((state >> 32) % bound);
        return result;
    }
};

// One scalar value per line-break symbol (the first that has it).
std::vector<u32> representatives() {
    std::vector<u32> result(tables::lb_symbol_count, 0xffffffffu);
    for (u32 code = 0; code < code_points; ++code) {
        if (!scalar(code)) continue;
        const u32 symbol = tables::symbol_of(tables::record(code));
        if (result[symbol] == 0xffffffffu) result[symbol] = code;
    }
    return result;
}

std::uint64_t compared = 0;
std::uint64_t differing = 0;

void compare(const std::vector<u32>& input, const bool with_utf8) {
    const std::vector<u8> reference = by_chain(input);
    bool same = by_code_points(input) == reference;
    if (same && with_utf8) same = by_utf8(input) == reference;
    compared = compared + 1;
    if (!same) {
        differing = differing + 1;
        report("T5", describe(input));
    }
}

void exhaustive(const std::vector<u32>& alphabet, const int longest) {
    std::vector<u32> input;
    for (int length = 1; length <= longest; ++length) {
        std::vector<usize> digits(static_cast<usize>(length), 0);
        input.assign(static_cast<usize>(length), alphabet[0]);
        while (true) {
            compare(input, length <= 2);
            usize position = 0;
            while (position < digits.size()) {
                digits[position] = digits[position] + 1;
                if (digits[position] < alphabet.size()) {
                    input[position] = alphabet[digits[position]];
                    break;
                }
                digits[position] = 0;
                input[position] = alphabet[0];
                position = position + 1;
            }
            if (position == digits.size()) break;
        }
    }
}

// The pool random texts are drawn from: every symbol's representative,
// and the characters texts are mostly made of several times over.
std::vector<u32> make_pool(const std::vector<u32>& alphabet) {
    std::vector<u32> pool = alphabet;
    const u32 common[] = {'a', 'b', 'e', 't', 'Z', ' ', ' ', ' ', ',', '.', '-', '1', '2', '(', ')', '"', '\'', '$', '%',
                          '/', 0x300, 0x200d, 0x2019, 0x201c, 0x201d, 0x4e00, 0x3042, 0x5d0, 0x627, 0x915, 0x94d, 0x1f1e6,
                          0x3001, 0xa0, 0x2014, 0x2010, 'x', 'y', '[', ']', 0xe9, 0x430, 0x3b1};
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const u32 value : common) pool.push_back(value);
    }
    return pool;
}

void random_sequences(const std::vector<u32>& pool, const int count, Random& random) {
    std::vector<u32> input;
    for (int index = 0; index < count; ++index) {
        const u32 length = 1 + random.next(64);
        input.clear();
        for (u32 position = 0; position < length; ++position) input.push_back(pool[random.next(static_cast<u32>(pool.size()))]);
        compare(input, true);
    }
}

// --- T6 -------------------------------------------------------------------

// The script runs of code points [0, count), by the header's rules,
// written apart from the library: per code point offsets.
struct ReferenceRun final {
    usize offset;
    usize length;
    layout::Script script;
};

std::vector<ReferenceRun> reference_scripts(const std::vector<u32>& input) {
    std::vector<ReferenceRun> runs;
    struct Open final {
        u32 closer;
        layout::Script script;
    };
    std::vector<Open> open;
    layout::Script current = layout::Script::common;
    usize start = 0;
    for (usize index = 0; index < input.size(); ++index) {
        const u32 value = scalar(input[index]) ? input[index] : 0xfffdu;
        layout::Script script = layout::script_of(value);
        const bool weak = script == layout::Script::common || script == layout::Script::inherited;
        bool from_bracket = false;
        if (weak) {
            const layout::BidiBracket bracket = layout::bidi_paired_bracket(value);
            if (bracket.type == layout::BracketType::open) {
                if (open.size() == 32) open.clear();
                open.push_back(Open{bracket.pair, current});
                continue;
            }
            if (bracket.type != layout::BracketType::close) continue;
            // The closer the opener names, compared by canonical
            // equivalence: U+232A and U+3009 close U+2329 and U+3008.
            const u32 canonical = value == 0x232au ? 0x3009u : value;
            while (!open.empty()) {
                const u32 wanted = open.back().closer == 0x232au ? 0x3009u : open.back().closer;
                if (wanted == canonical) break;
                open.pop_back();
            }
            if (open.empty()) continue;
            script = open.back().script;
            open.pop_back();
            from_bracket = true;
            if (script == layout::Script::common || script == layout::Script::inherited) continue;
        }
        const bool unsettled = current == layout::Script::common || current == layout::Script::inherited;
        if (unsettled) {
            current = script;
            for (Open& entry : open) entry.script = script;
            continue;
        }
        if (script == current) continue;
        if (!from_bracket && layout::script_extends_to(value, current)) continue;
        runs.push_back(ReferenceRun{start, index - start, current});
        start = index;
        current = script;
    }
    runs.push_back(ReferenceRun{start, input.size() - start, current});
    return runs;
}

std::uint64_t paragraphs_checked = 0;
std::uint64_t paragraph_failures = 0;

// Every paragraph of `input`, through UTF-8 and through code points.
void check_paragraphs(const std::vector<u32>& input) {
    std::string bytes;
    std::vector<usize> first_byte;
    for (const u32 value : input) {
        first_byte.push_back(bytes.size());
        append_utf8(bytes, value);
    }
    first_byte.push_back(bytes.size());
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    if (validated.failed()) {
        paragraph_failures = paragraph_failures + 1;
        return;
    }
    const stxlib::TextView view = *validated.value();
    layout::Paragraph by_bytes;
    layout::Paragraph by_points;
    layout::Line line;
    usize point = 0;
    while (point < input.size()) {
        bool ok = true;
        const usize offset = first_byte[point];
        const stxlib::OperationStatus a =
            layout::analyse_paragraph(view, offset, layout::BaseDirection::automatic_left_to_right, by_bytes);
        const stxlib::OperationStatus b = layout::analyse_paragraph(stxlib::View<u32>(input.data(), input.size()),
                                                                    point,
                                                                    layout::BaseDirection::automatic_left_to_right,
                                                                    by_points);
        if (a.failed() || b.failed() || by_bytes.offset() != offset || by_points.offset() != point ||
            by_points.length() == 0) {
            paragraph_failures = paragraph_failures + 1;
            report("T6", "analyse_paragraph failed: " + describe(input));
            return;
        }
        const usize points = by_points.length();
        ok = ok && offset + by_bytes.length() == first_byte[point + points];
        const std::vector<u32> alone(input.begin() + static_cast<std::ptrdiff_t>(point),
                                     input.begin() + static_cast<std::ptrdiff_t>(point + points));
        const std::vector<u8> reference = by_chain(alone);
        const stxlib::View<layout::LineBreak> breaks_points = by_points.breaks();
        const stxlib::View<layout::LineBreak> breaks_bytes = by_bytes.breaks();
        ok = ok && breaks_points.count() == points && breaks_bytes.count() == by_bytes.length();
        for (usize index = 0; ok && index < points; ++index) {
            ok = static_cast<u8>(breaks_points[index]) == reference[index];
            const usize begin = first_byte[point + index] - offset;
            const usize end = first_byte[point + index + 1] - offset;
            for (usize byte = begin; ok && byte + 1 < end; ++byte) {
                ok = breaks_bytes[byte] == layout::LineBreak::inside_character;
            }
            ok = ok && static_cast<u8>(breaks_bytes[end - 1]) == reference[index];
        }
        // Scripts.
        const std::vector<ReferenceRun> expected = reference_scripts(alone);
        const stxlib::View<layout::ScriptRun> runs_points = by_points.script_runs();
        const stxlib::View<layout::ScriptRun> runs_bytes = by_bytes.script_runs();
        ok = ok && runs_points.count() == expected.size() && runs_bytes.count() == expected.size();
        for (usize index = 0; ok && index < expected.size(); ++index) {
            const ReferenceRun& run = expected[index];
            ok = runs_points[index].offset == point + run.offset && runs_points[index].length == run.length &&
                 runs_points[index].script == run.script;
            ok = ok && runs_bytes[index].offset == first_byte[point + run.offset] &&
                 runs_bytes[index].offset + runs_bytes[index].length == first_byte[point + run.offset + run.length] &&
                 runs_bytes[index].script == run.script;
        }
        // Levels agree, and the line over the whole paragraph is made.
        ok = ok && by_bytes.base_level() == by_points.base_level();
        for (usize index = 0; ok && index < points; ++index) {
            ok = by_bytes.levels()[first_byte[point + index] - offset] == by_points.levels()[index];
        }
        const stxlib::OperationStatus lined = layout::analyse_line(by_bytes, offset, by_bytes.length(), line);
        ok = ok && lined.succeeded();
        paragraphs_checked = paragraphs_checked + 1;
        if (!ok) {
            paragraph_failures = paragraph_failures + 1;
            report("T6", describe(alone));
        }
        point = point + points;
    }
}

void paragraph_texts(const std::vector<u32>& pool, const int count, Random& random) {
    const u32 separators[] = {0x0a, 0x0a, 0x0d, 0x1c, 0x1d, 0x1e, 0x85, 0x2029};
    std::vector<u32> input;
    for (int index = 0; index < count; ++index) {
        const u32 length = 1 + random.next(120);
        input.clear();
        for (u32 position = 0; position < length; ++position) {
            if (random.next(12) == 0) {
                const u32 separator = separators[random.next(8)];
                input.push_back(separator);
                if (separator == 0x0d && random.next(2) == 0) input.push_back(0x0a);
            } else {
                input.push_back(pool[random.next(static_cast<u32>(pool.size()))]);
            }
        }
        check_paragraphs(input);
    }
    // Paragraphs longer than two blocks of the walk (65,536 code units
    // each): letters and spaces, then the whole pool.
    for (int kind = 0; kind < 2; ++kind) {
        input.clear();
        for (u32 position = 0; position < 150000; ++position) {
            u32 value = pool[random.next(static_cast<u32>(pool.size()))];
            if (kind == 0) value = random.next(7) == 0 ? ' ' : static_cast<u32>('a' + random.next(26));
            const layout::BidiClass kind_of = layout::bidi_class(value);
            if (kind_of == layout::BidiClass::paragraph_separator) value = 'q';
            input.push_back(value);
        }
        check_paragraphs(input);
    }
    CHECK(paragraph_failures == 0);
    std::printf("T6: %llu paragraphs checked (breaks, script runs, UTF-8 against code points), %llu fail\n",
                static_cast<unsigned long long>(paragraphs_checked), static_cast<unsigned long long>(paragraph_failures));
}

// --- T7 -------------------------------------------------------------------

void fuzz(const int count, Random& random) {
    std::uint64_t accepted = 0;
    std::uint64_t failures = 0;
    std::vector<u32> input;
    layout::Paragraph paragraph;
    layout::Line line;
    for (int index = 0; index < count; ++index) {
        std::string bytes;
        const u32 length = random.next(96);
        const u32 kind = random.next(4);
        if (kind == 0) {
            // Raw bytes, weighted towards lead and continuation bytes.
            for (u32 position = 0; position < length; ++position) {
                const u32 choice = random.next(8);
                u32 byte = random.next(128);
                if (choice == 0) byte = 0xc2u + random.next(0x33);
                if (choice == 1) byte = 0x80u + random.next(0x40);
                bytes.push_back(static_cast<char>(byte));
            }
        } else {
            for (u32 position = 0; position < length; ++position) {
                u32 value = random.next(0x110000);
                if (kind == 2) value = random.next(0x3100);
                if (kind == 3 && random.next(3) != 0) value = random.next(0x80);
                if (!scalar(value)) value = 0x2028;
                append_utf8(bytes, value);
            }
        }
        const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
        if (validated.failed()) continue;
        accepted = accepted + 1;
        const stxlib::TextView view = *validated.value();
        bool ok = true;
        std::vector<layout::LineBreak> breaks(bytes.size(), layout::LineBreak::prohibited);
        const stxlib::OperationStatus broke =
            layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(breaks.data(), breaks.size()), view);
        ok = ok && broke.succeeded();
        for (usize byte = 0; ok && byte < bytes.size(); ++byte) {
            const bool continues = byte + 1 < bytes.size() && (static_cast<unsigned char>(bytes[byte + 1]) & 0xc0u) == 0x80u;
            ok = continues == (breaks[byte] == layout::LineBreak::inside_character);
            ok = ok && static_cast<u8>(breaks[byte]) <= 3;
        }
        if (ok && !bytes.empty()) ok = breaks.back() == layout::LineBreak::mandatory;
        usize offset = 0;
        while (ok && offset < bytes.size()) {
            const stxlib::OperationStatus analysed =
                layout::analyse_paragraph(view, offset, layout::BaseDirection::automatic_left_to_right, paragraph);
            ok = analysed.succeeded() && paragraph.offset() == offset && paragraph.length() > 0 &&
                 paragraph.length() <= bytes.size() - offset;
            if (!ok) break;
            const usize length_here = paragraph.length();
            ok = paragraph.breaks()[length_here - 1] == layout::LineBreak::mandatory && paragraph.base_level() <= 1;
            for (usize unit = 0; ok && unit < length_here; ++unit) {
                ok = paragraph.levels()[unit] <= layout::bidi_max_depth + 1u;
                const bool inside = paragraph.breaks()[unit] == layout::LineBreak::inside_character;
                ok = ok && inside == (breaks[offset + unit] == layout::LineBreak::inside_character);
            }
            const stxlib::View<layout::ScriptRun> runs = paragraph.script_runs();
            usize covered = offset;
            for (usize run = 0; ok && run < runs.count(); ++run) {
                ok = runs[run].offset == covered && runs[run].length > 0;
                covered = covered + runs[run].length;
            }
            ok = ok && covered == offset + length_here;
            const stxlib::OperationStatus lined = layout::analyse_line(paragraph, offset, length_here, line);
            ok = ok && lined.succeeded();
            usize in_runs = 0;
            for (usize run = 0; ok && run < line.runs().count(); ++run) in_runs = in_runs + line.runs()[run].length;
            ok = ok && in_runs == length_here;
            offset = offset + length_here;
        }
        if (!ok) {
            failures = failures + 1;
            std::string detail;
            for (const char character : bytes) {
                char piece[8];
                std::snprintf(piece, sizeof piece, "%02X ", static_cast<unsigned>(static_cast<unsigned char>(character)));
                detail += piece;
            }
            report("T7", detail);
        }
    }
    CHECK(failures == 0);
    std::printf("T7: fuzz: %d inputs, %llu well-formed and analysed, %llu fail\n", count,
                static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(failures));
}

// --- T8 -------------------------------------------------------------------

void arguments() {
    layout::LineBreak storage[4] = {layout::LineBreak::inside_character, layout::LineBreak::inside_character,
                                    layout::LineBreak::inside_character, layout::LineBreak::inside_character};
    const stxlib::TextView text("ab c");
    const stxlib::OperationStatus too_small =
        layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(storage, 3), text);
    CHECK(too_small.failed() && storage[0] == layout::LineBreak::inside_character);
    const stxlib::OperationStatus exact = layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(storage, 4),
                                                                   text);
    CHECK(exact.succeeded());
    CHECK(storage[0] == layout::LineBreak::prohibited && storage[1] == layout::LineBreak::prohibited);
    CHECK(storage[2] == layout::LineBreak::allowed && storage[3] == layout::LineBreak::mandatory);
    const stxlib::OperationStatus empty = layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(),
                                                                   stxlib::TextView());
    CHECK(empty.succeeded());
    const u32 values[3] = {'a', 0x0a, 'b'};
    const stxlib::OperationStatus points = layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(storage, 3),
                                                                    stxlib::View<u32>(values, 3));
    CHECK(points.succeeded() && storage[0] == layout::LineBreak::prohibited && storage[1] == layout::LineBreak::mandatory &&
          storage[2] == layout::LineBreak::mandatory);
    const stxlib::OperationStatus wrong = layout::find_line_breaks(stxlib::MutableView<layout::LineBreak>(storage, 4),
                                                                   stxlib::View<u32>(values, 3));
    CHECK(wrong.failed());
    layout::Paragraph paragraph;
    CHECK(paragraph.breaks().count() == 0 && paragraph.script_runs().count() == 0);
    // A sentence in two scripts with brackets: Latin, Greek in parentheses.
    const stxlib::TextView mixed("ab (\xce\xb1\xce\xb2) c");
    const stxlib::OperationStatus analysed =
        layout::analyse_paragraph(mixed, 0, layout::BaseDirection::automatic_left_to_right, paragraph);
    CHECK(analysed.succeeded());
    const stxlib::View<layout::ScriptRun> runs = paragraph.script_runs();
    CHECK(runs.count() == 3);
    if (runs.count() == 3) {
        // "ab (" Latin; the Greek letters; ") c" Latin again, the closing
        // parenthesis with the script of its opener.
        CHECK(runs[0].offset == 0 && runs[0].length == 4 && tag_text(layout::script_tag(runs[0].script)) == "Latn");
        CHECK(runs[1].offset == 4 && runs[1].length == 4 && tag_text(layout::script_tag(runs[1].script)) == "Grek");
        CHECK(runs[2].offset == 8 && runs[2].length == 3 && tag_text(layout::script_tag(runs[2].script)) == "Latn");
    }
    paragraph.clear();
    CHECK(paragraph.breaks().count() == 0 && paragraph.script_runs().count() == 0 && paragraph.length() == 0);

    // A moved-from paragraph or line is empty (A-stxlib-textlayout-01-F2):
    // its accessors give empty views and analyse_line refuses it.
    const stxlib::TextView hebrew("ab \xd7\x90\xd7\x91 c");
    CHECK(layout::analyse_paragraph(hebrew, 0, layout::BaseDirection::automatic_left_to_right, paragraph).succeeded());
    layout::Paragraph moved(static_cast<layout::Paragraph&&>(paragraph));
    CHECK(moved.length() == hebrew.count() && moved.levels().count() == hebrew.count());
    CHECK(paragraph.length() == 0 && paragraph.levels().count() == 0 && paragraph.breaks().count() == 0 &&
          paragraph.script_runs().count() == 0);
    layout::Line line;
    CHECK(layout::analyse_line(paragraph, 0, hebrew.count(), line).failed());
    CHECK(layout::analyse_line(moved, 0, moved.length(), line).succeeded() && line.runs().count() == 3);
    layout::Line moved_line;
    moved_line = static_cast<layout::Line&&>(line);
    CHECK(moved_line.runs().count() == 3 && moved_line.levels().count() == hebrew.count());
    CHECK(line.runs().count() == 0 && line.levels().count() == 0 && line.length() == 0);
    paragraph = static_cast<layout::Paragraph&&>(moved);
    CHECK(paragraph.length() == hebrew.count() && moved.length() == 0 && moved.levels().count() == 0);
    // release() empties it and frees its storage; it can be analysed into again.
    paragraph.release();
    moved_line.release();
    CHECK(paragraph.length() == 0 && paragraph.levels().count() == 0 && moved_line.runs().count() == 0);
    CHECK(layout::analyse_paragraph(hebrew, 0, layout::BaseDirection::automatic_left_to_right, paragraph).succeeded());
    CHECK(paragraph.length() == hebrew.count());

    // bytes_held is the storage's capacity, bytes_peak its most since the
    // last reset_peak or release; 1,000 analyses of the same paragraph and
    // line hold steady (GUI.Forms' text_layout ledger).
    {
        layout::Paragraph held_paragraph;
        layout::Line held_line;
        CHECK(held_paragraph.bytes_held() == 0 && held_paragraph.bytes_peak() == 0 && held_line.bytes_held() == 0);
        CHECK(layout::analyse_paragraph(hebrew, 0, layout::BaseDirection::automatic_left_to_right, held_paragraph)
                  .succeeded());
        CHECK(layout::analyse_line(held_paragraph, 0, held_paragraph.length(), held_line).succeeded());
        const stxlib::usize paragraph_held = held_paragraph.bytes_held();
        const stxlib::usize line_held = held_line.bytes_held();
        CHECK(paragraph_held > 0 && held_paragraph.bytes_peak() == paragraph_held);
        CHECK(line_held > 0 && held_line.bytes_peak() == line_held);
        bool steady = true;
        for (int round = 0; round < 1000; ++round) {
            steady = steady &&
                     layout::analyse_paragraph(hebrew, 0, layout::BaseDirection::automatic_left_to_right, held_paragraph)
                         .succeeded() &&
                     layout::analyse_line(held_paragraph, 0, held_paragraph.length(), held_line).succeeded() &&
                     held_paragraph.bytes_held() == paragraph_held && held_line.bytes_held() == line_held &&
                     held_paragraph.bytes_peak() == paragraph_held && held_line.bytes_peak() == line_held;
        }
        CHECK(steady);
        layout::Paragraph taken(static_cast<layout::Paragraph&&>(held_paragraph));
        CHECK(taken.bytes_held() == paragraph_held && taken.bytes_peak() == paragraph_held);
        CHECK(held_paragraph.bytes_held() == 0 && held_paragraph.bytes_peak() == 0);
        taken.release();
        held_line.release();
        CHECK(taken.bytes_held() == 0 && taken.bytes_peak() == 0 && held_line.bytes_held() == 0 &&
              held_line.bytes_peak() == 0);
        CHECK(layout::analyse_paragraph(hebrew, 0, layout::BaseDirection::automatic_left_to_right, taken).succeeded());
        taken.reset_peak();
        CHECK(taken.bytes_peak() == taken.bytes_held() && taken.bytes_held() > 0);
    }
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stxlib_textlayout_tests <unicode directory> [<conformance directory>] "
                             "[--exhaustive N] [--random N] [--fuzz N]\n");
        return 2;
    }
    std::string conformance;
    int longest = 3;
    int random_count = 20000;
    int fuzz_count = 200000;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--exhaustive" && index + 1 < argc) {
            longest = std::atoi(argv[index + 1]);
            index = index + 1;
        } else if (argument == "--random" && index + 1 < argc) {
            random_count = std::atoi(argv[index + 1]);
            index = index + 1;
        } else if (argument == "--fuzz" && index + 1 < argc) {
            fuzz_count = std::atoi(argv[index + 1]);
            index = index + 1;
        } else {
            conformance = argument;
        }
    }
    const bool read = properties(argv[1]);
    CHECK(read);
    automaton_equals_chain();
    if (conformance.empty()) {
        std::printf("T4: NOT RUN: no conformance directory given (STXLIB_UNICODE_TEST_DATA)\n");
    } else {
        line_break_test(conformance);
    }
    const std::vector<u32> alphabet = representatives();
    bool every_symbol = true;
    for (const u32 value : alphabet) every_symbol = every_symbol && value != 0xffffffffu;
    CHECK(every_symbol);
    if (every_symbol) {
        exhaustive(alphabet, longest);
        const std::uint64_t after_exhaustive = compared;
        Random random;
        const std::vector<u32> pool = make_pool(alphabet);
        random_sequences(pool, random_count, random);
        CHECK(differing == 0);
        std::printf("T5: %llu comparisons with the chain (symbols to length %d: %llu; random: %llu), %llu differ\n",
                    static_cast<unsigned long long>(compared), longest, static_cast<unsigned long long>(after_exhaustive),
                    static_cast<unsigned long long>(compared - after_exhaustive), static_cast<unsigned long long>(differing));
        paragraph_texts(pool, random_count / 4, random);
        fuzz(fuzz_count, random);
    }
    arguments();
    return stxlib_tests::finish("stxlib_textlayout_tests");
}
