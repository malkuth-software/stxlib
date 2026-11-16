// Tests of include/stxlib/textlayout.hpp (UAX #9, Unicode 18.0.0).
//
// Arguments: <unicode directory> [<conformance directory>] [--exhaustive N]
// [--random N]. The unicode directory is third_party/unicode-18.0.0; the
// conformance directory holds BidiTest.txt and BidiCharacterTest.txt
// (kept off the repository: PROVENANCE.md). Without it the conformance
// properties are reported as not run, plainly, and the rest still runs.
//
// The references are the Unicode files themselves, read here by parsers of
// the test's own (independent of tools/gen_textlayout_tables.py), and
// tests/reference/bidi_reference.hpp, a per-code-point reading of the
// specification independent of src/algorithms/textlayout/analysis.cpp.
//
// Properties:
//   B1  every code point 0 to 0x10FFFF: bidi_class is DerivedBidiClass.txt's
//       value (its @missing defaults, then its listing); bidi_mirrored is
//       UnicodeData.txt's field 9; bidi_mirroring_glyph is
//       BidiMirroring.txt's mapping or the code point; bidi_paired_bracket
//       is BidiBrackets.txt's pair and type, or none. Values above
//       0x10FFFF: L, not mirrored, unchanged, none.
//   B2  BidiTest.txt: every case, at every paragraph direction of its
//       bitset, through analyse_paragraph and analyse_line over code
//       points (one representative code point per class): the levels of
//       the characters not removed by X9 and the visual order from the
//       line's runs are the file's. Pass count reported.
//   B3  BidiCharacterTest.txt: every case: the paragraph level, the levels
//       and the visual order are the file's. Pass count reported.
//   B4  the library equals the reference exactly (every code unit's level,
//       the paragraph level, the line levels after L1 and the visual order)
//       on every sequence of classes of length 1 to N (default 4; 23
//       classes, all four directions), on every sequence of length 1 to 6
//       over an alphabet of brackets and the classes that decide N0, and
//       on random sequences (default 20,000, lengths 1 to 300, a fixed
//       seed) drawn from every class, brackets, and the explicit
//       formatting characters nested past the depth limit.
//   B5  UTF-8 and code points agree: the same random texts, encoded, give
//       per byte the level of the code point the byte belongs to, and the
//       same runs at byte offsets; paragraphs split at each separator, CR
//       LF together.
//   B6  arguments: offsets out of range, not at a boundary, an empty line;
//       the object left empty on failure.
// B1 is exhaustive over its domain; B4's first two parts are exhaustive
// over their stated sets (a proof by enumeration once the test passes);
// B4's random part and B5 are sampled.

#include "stxlib/textlayout.hpp"
#include "stxlib/text.hpp"

#include "reference/bidi_reference.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace text = stxlib::textlayout;
namespace ref = bidi_reference;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;
using stxlib::View;

constexpr u32 code_points = 0x110000;

std::uint64_t failures_reported = 0;

// At most 12 failures printed per property (the property's digit).
std::uint64_t reported_by_property[10] = {};

void report(const char* const property, const std::string& detail) {
    failures_reported = failures_reported + 1;
    const unsigned digit = static_cast<unsigned>(property[1] - '0') % 10u;
    reported_by_property[digit] = reported_by_property[digit] + 1;
    if (reported_by_property[digit] <= 12) std::fprintf(stderr, "%s: %s\n", property, detail.c_str());
}

std::vector<std::string> split(const std::string& line, const char separator) {
    std::vector<std::string> pieces;
    std::string piece;
    for (const char c : line) {
        if (c == separator) {
            pieces.push_back(piece);
            piece.clear();
        } else {
            piece.push_back(c);
        }
    }
    pieces.push_back(piece);
    return pieces;
}

std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> result;
    std::string word;
    for (const char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!word.empty()) result.push_back(word);
            word.clear();
        } else {
            word.push_back(c);
        }
    }
    if (!word.empty()) result.push_back(word);
    return result;
}

std::string strip_comment(const std::string& line) {
    const std::size_t hash = line.find('#');
    const std::string result = hash == std::string::npos ? line : line.substr(0, hash);
    return result;
}

std::vector<std::string> read_lines(const std::string& path, bool& found) {
    std::vector<std::string> lines;
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    found = file != nullptr;
    if (file == nullptr) return lines;
    std::string current;
    char buffer[4096];
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        current += buffer;
        if (!current.empty() && current.back() == '\n') {
            current.pop_back();
            lines.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) lines.push_back(current);
    std::fclose(file);
    return lines;
}

const char* const class_names[] = {"L",   "R",   "AL",  "EN",  "ES",  "ET",  "AN",  "CS",
                                   "NSM", "BN",  "B",   "S",   "WS",  "ON",  "LRE", "LRO",
                                   "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"};

int class_of_name(const std::string& name) {
    for (int index = 0; index < 23; ++index) {
        if (name == class_names[index]) return index;
    }
    return -1;
}

// One code point of each class, for BidiTest.txt.
constexpr u32 representative[23] = {0x0061, 0x05d0, 0x0627, 0x0031, 0x002b, 0x0024, 0x0660, 0x002c,
                                    0x0300, 0x00ad, 0x2029, 0x0009, 0x0020, 0x0021, 0x202a, 0x202d,
                                    0x202b, 0x202e, 0x202c, 0x2066, 0x2067, 0x2068, 0x2069};

// ---- the Unicode files --------------------------------------------------

struct Data final {
    std::vector<int> classes;          // per code point
    std::vector<bool> mirrored;        // per code point
    std::vector<u32> mirror;           // per code point (itself when none)
    std::vector<u32> pair;             // per code point (itself when none)
    std::vector<int> bracket_type;     // 0 none, 1 open, 2 close
    std::vector<u32> singleton;        // canonical singleton decomposition, or itself
    bool read{false};
};

u32 hex(const std::string& value) { return static_cast<u32>(std::strtoul(value.c_str(), nullptr, 16)); }

Data read_data(const std::string& directory) {
    Data data;
    data.classes.assign(code_points, 0);
    data.mirrored.assign(code_points, false);
    data.mirror.resize(code_points);
    data.pair.resize(code_points);
    data.singleton.resize(code_points);
    data.bracket_type.assign(code_points, 0);
    for (u32 code = 0; code < code_points; ++code) {
        data.mirror[code] = code;
        data.pair[code] = code;
        data.singleton[code] = code;
    }
    bool found = false;
    const std::vector<std::string> derived = read_lines(directory + "/DerivedBidiClass.txt", found);
    if (!found) return data;
    const char* const long_names[][2] = {{"Left_To_Right", "L"}, {"Right_To_Left", "R"}, {"Arabic_Letter", "AL"},
                                         {"European_Terminator", "ET"}, {"Boundary_Neutral", "BN"}};
    for (const std::string& line : derived) {
        if (line.rfind("# @missing:", 0) != 0) continue;
        const std::vector<std::string> fields = split(strip_comment(line.substr(11)), ';');
        const std::vector<std::string> span = split(words(fields[0])[0], '.');
        std::string value = words(fields[1])[0];
        for (const char* const* name : long_names) {
            if (value == name[0]) value = name[1];
        }
        const int kind = class_of_name(value);
        for (u32 code = hex(span[0]); code <= hex(span.back()); ++code) data.classes[code] = kind;
    }
    for (const std::string& line : derived) {
        const std::string body = strip_comment(line);
        if (words(body).empty()) continue;
        const std::vector<std::string> fields = split(body, ';');
        const std::vector<std::string> span = split(words(fields[0])[0], '.');
        const int kind = class_of_name(words(fields[1])[0]);
        for (u32 code = hex(span[0]); code <= hex(span.back()); ++code) data.classes[code] = kind;
    }
    const std::vector<std::string> unicode = read_lines(directory + "/UnicodeData.txt", found);
    u32 first = 0;
    for (const std::string& line : unicode) {
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 15) continue;
        const u32 code = hex(fields[0]);
        if (fields[1].find(", First>") != std::string::npos) {
            first = code;
            continue;
        }
        const u32 low = fields[1].find(", Last>") != std::string::npos ? first : code;
        for (u32 value = low; value <= code; ++value) data.mirrored[value] = fields[9] == "Y";
        const std::vector<std::string> decomposition = words(fields[5]);
        if (decomposition.size() == 1 && decomposition[0][0] != '<') data.singleton[code] = hex(decomposition[0]);
    }
    const std::vector<std::string> mirrors = read_lines(directory + "/BidiMirroring.txt", found);
    for (const std::string& line : mirrors) {
        const std::string body = strip_comment(line);
        if (words(body).empty()) continue;
        const std::vector<std::string> fields = split(body, ';');
        data.mirror[hex(words(fields[0])[0])] = hex(words(fields[1])[0]);
    }
    const std::vector<std::string> brackets = read_lines(directory + "/BidiBrackets.txt", found);
    for (const std::string& line : brackets) {
        const std::string body = strip_comment(line);
        if (words(body).empty()) continue;
        const std::vector<std::string> fields = split(body, ';');
        const u32 code = hex(words(fields[0])[0]);
        data.pair[code] = hex(words(fields[1])[0]);
        data.bracket_type[code] = words(fields[2])[0] == "o" ? 1 : 2;
    }
    data.read = true;
    return data;
}

ref::Bracket reference_bracket(const Data& data, const u32 code) {
    ref::Bracket result;
    if (code >= code_points || data.bracket_type[code] == 0) return result;
    result.type = data.bracket_type[code];
    const u32 opener = result.type == 1 ? code : data.pair[code];
    result.key = data.singleton[opener];
    return result;
}

void lookups(const Data& data) {
    std::uint64_t mismatches = 0;
    for (u32 code = 0; code < code_points; ++code) {
        const bool ok = static_cast<int>(text::bidi_class(code)) == data.classes[code] &&
                        text::bidi_mirrored(code) == data.mirrored[code] &&
                        text::bidi_mirroring_glyph(code) == data.mirror[code];
        const text::BidiBracket bracket = text::bidi_paired_bracket(code);
        const int type = bracket.type == text::BracketType::open ? 1 : (bracket.type == text::BracketType::close ? 2 : 0);
        const bool bracket_ok = bracket.pair == data.pair[code] && type == data.bracket_type[code];
        if (!ok || !bracket_ok) {
            mismatches = mismatches + 1;
            char detail[64];
            std::snprintf(detail, sizeof detail, "U+%04X", static_cast<unsigned>(code));
            report("B1", detail);
        }
    }
    const bool above = text::bidi_class(0x110000) == text::BidiClass::left_to_right && !text::bidi_mirrored(0x110000) &&
                       text::bidi_mirroring_glyph(0x110000) == 0x110000 &&
                       text::bidi_paired_bracket(0x110000).type == text::BracketType::none;
    CHECK(mismatches == 0);
    CHECK(above);
    for (int kind = 0; kind < 23; ++kind) CHECK(static_cast<int>(text::bidi_class(representative[kind])) == kind);
    std::printf("B1: %u code points checked, %llu mismatches\n", code_points, static_cast<unsigned long long>(mismatches));
}

// ---- the library as the tests read it -----------------------------------

struct Outcome final {
    bool ok{false};
    int base_level{0};
    std::vector<int> levels;      // per code unit, before L1
    std::vector<int> line_levels; // per code unit, after L1
    std::vector<usize> order;     // code units in visual order
};

Outcome run_library(const std::vector<u32>& input, const text::BaseDirection direction) {
    Outcome outcome;
    text::Paragraph paragraph;
    const stxlib::Status analysed =
        text::analyse_paragraph(View<u32>(input.data(), input.size()), 0, direction, paragraph);
    if (analysed.failed() || paragraph.length() != input.size()) return outcome;
    outcome.base_level = paragraph.base_level();
    const View<u8> levels = paragraph.levels();
    for (usize index = 0; index < levels.count(); ++index) outcome.levels.push_back(levels[index]);
    text::Line line;
    const stxlib::Status lined = text::analyse_line(paragraph, 0, input.size(), line);
    if (lined.failed()) return outcome;
    const View<u8> line_levels = line.levels();
    for (usize index = 0; index < line_levels.count(); ++index) outcome.line_levels.push_back(line_levels[index]);
    const View<text::BidiRun> runs = line.runs();
    usize covered = 0;
    for (usize index = 0; index < runs.count(); ++index) {
        const text::BidiRun& run = runs[index];
        covered = covered + run.length;
        if (index > 0 && run.level == runs[index - 1].level) return outcome;
        for (u32 unit = 0; unit < run.length; ++unit) {
            const u32 position = (run.level & 1u) != 0 ? run.offset + run.length - 1 - unit : run.offset + unit;
            if (line_levels[position] != run.level) return outcome;
            outcome.order.push_back(position);
        }
    }
    outcome.ok = covered == input.size();
    return outcome;
}

text::BaseDirection direction_of(const int value) {
    const text::BaseDirection result = value == 0   ? text::BaseDirection::automatic_left_to_right
                                       : value == 1 ? text::BaseDirection::automatic_right_to_left
                                       : value == 2 ? text::BaseDirection::left_to_right
                                                    : text::BaseDirection::right_to_left;
    return result;
}

std::string describe(const std::vector<u32>& input, const int direction) {
    std::string result;
    for (const u32 value : input) {
        char piece[16];
        std::snprintf(piece, sizeof piece, "%04X ", static_cast<unsigned>(value));
        result += piece;
    }
    result += "direction " + std::to_string(direction);
    return result;
}

// ---- B2, B3 -------------------------------------------------------------

bool removed_class(const int kind) {
    return kind == ref::BN || (kind >= ref::LRE && kind <= ref::PDF);
}

// The file's levels and order against the outcome, skipping removed ones.
bool agrees(const Outcome& outcome, const std::vector<int>& classes, const std::vector<std::string>& expected_levels,
            const std::vector<std::string>& expected_order) {
    if (!outcome.ok || expected_levels.size() != classes.size()) return false;
    for (usize index = 0; index < classes.size(); ++index) {
        if (expected_levels[index] == "x") {
            if (!removed_class(classes[index])) return false;
            continue;
        }
        if (std::to_string(outcome.line_levels[index]) != expected_levels[index]) return false;
    }
    std::vector<std::string> order;
    for (const usize position : outcome.order) {
        if (!removed_class(classes[position])) order.push_back(std::to_string(position));
    }
    return order == expected_order;
}

void bidi_test(const std::string& directory) {
    bool found = false;
    const std::vector<std::string> lines = read_lines(directory + "/BidiTest.txt", found);
    if (!found) {
        std::printf("B2: NOT RUN: %s/BidiTest.txt is absent\n", directory.c_str());
        return;
    }
    std::vector<std::string> levels;
    std::vector<std::string> order;
    std::uint64_t cases = 0;
    std::uint64_t passed = 0;
    for (const std::string& raw : lines) {
        if (raw.rfind("@Levels:", 0) == 0) {
            levels = words(raw.substr(8));
            continue;
        }
        if (raw.rfind("@Reorder:", 0) == 0) {
            order = words(raw.substr(9));
            continue;
        }
        const std::string line = strip_comment(raw);
        if (line.empty() || line[0] == '@' || words(line).empty()) continue;
        const std::vector<std::string> fields = split(line, ';');
        std::vector<int> classes;
        std::vector<u32> input;
        for (const std::string& name : words(fields[0])) {
            classes.push_back(class_of_name(name));
            input.push_back(representative[classes.back()]);
        }
        const int bitset = static_cast<int>(std::strtol(words(fields[1])[0].c_str(), nullptr, 16));
        const int directions[3][2] = {{1, 0}, {2, 2}, {4, 3}};
        for (const int* pair : directions) {
            if ((bitset & pair[0]) == 0) continue;
            cases = cases + 1;
            const Outcome outcome = run_library(input, direction_of(pair[1]));
            if (agrees(outcome, classes, levels, order)) {
                passed = passed + 1;
            } else {
                report("B2", line + " direction bit " + std::to_string(pair[0]));
            }
        }
    }
    CHECK(cases > 0 && passed == cases);
    std::printf("B2: BidiTest.txt %llu of %llu cases pass\n", static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(cases));
}

void bidi_character_test(const std::string& directory) {
    bool found = false;
    const std::vector<std::string> lines = read_lines(directory + "/BidiCharacterTest.txt", found);
    if (!found) {
        std::printf("B3: NOT RUN: %s/BidiCharacterTest.txt is absent\n", directory.c_str());
        return;
    }
    std::uint64_t cases = 0;
    std::uint64_t passed = 0;
    for (const std::string& raw : lines) {
        const std::string line = strip_comment(raw);
        if (words(line).empty()) continue;
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 5) continue;
        std::vector<u32> input;
        std::vector<int> classes;
        for (const std::string& value : words(fields[0])) {
            input.push_back(hex(value));
            classes.push_back(static_cast<int>(text::bidi_class(input.back())));
        }
        const int file_direction = std::atoi(fields[1].c_str());
        const int direction = file_direction == 0 ? 2 : (file_direction == 1 ? 3 : 0);
        cases = cases + 1;
        const Outcome outcome = run_library(input, direction_of(direction));
        const bool level_ok = outcome.ok && std::to_string(outcome.base_level) == words(fields[2])[0];
        if (level_ok && agrees(outcome, classes, words(fields[3]), words(fields[4]))) {
            passed = passed + 1;
        } else {
            report("B3", line);
        }
    }
    CHECK(cases > 0 && passed == cases);
    std::printf("B3: BidiCharacterTest.txt %llu of %llu cases pass\n", static_cast<unsigned long long>(passed),
                static_cast<unsigned long long>(cases));
}

// ---- B4 -----------------------------------------------------------------

std::uint64_t compared = 0;
std::uint64_t differing = 0;

std::uint64_t skipped_paragraphs = 0;

void compare(const Data& data, const std::vector<u32>& input) {
    // One paragraph per comparison: a separator only at the end (the
    // splitting itself is B5's).
    for (usize index = 0; index + 1 < input.size(); ++index) {
        if (data.classes[input[index]] == ref::B) {
            skipped_paragraphs = skipped_paragraphs + 1;
            return;
        }
    }
    std::vector<int> classes;
    std::vector<ref::Bracket> brackets;
    for (const u32 value : input) {
        classes.push_back(data.classes[value]);
        brackets.push_back(reference_bracket(data, value));
    }
    for (int direction = 0; direction < 4; ++direction) {
        compared = compared + 1;
        const Outcome outcome = run_library(input, direction_of(direction));
        const ref::Result expected = ref::resolve(classes, brackets, direction);
        bool same = outcome.ok && outcome.base_level == expected.base_level && outcome.levels == expected.levels;
        if (same) {
            const std::vector<int> line = ref::line_levels(expected, classes, 0, classes.size());
            const std::vector<std::size_t> order = ref::visual_order(line);
            same = outcome.line_levels == line && outcome.order.size() == order.size();
            for (usize index = 0; same && index < order.size(); ++index) same = outcome.order[index] == order[index];
        }
        if (!same) {
            differing = differing + 1;
            report("B4", describe(input, direction));
        }
    }
}

void exhaustive(const Data& data, const int longest) {
    std::vector<u32> input;
    for (int length = 1; length <= longest; ++length) {
        std::vector<int> digits(static_cast<usize>(length), 0);
        while (true) {
            input.clear();
            for (const int digit : digits) input.push_back(representative[digit]);
            compare(data, input);
            int place = length - 1;
            while (place >= 0 && digits[static_cast<usize>(place)] == 22) {
                digits[static_cast<usize>(place)] = 0;
                place = place - 1;
            }
            if (place < 0) break;
            digits[static_cast<usize>(place)] = digits[static_cast<usize>(place)] + 1;
        }
    }
}

// Brackets (U+2329 and U+3009 pair by canonical equivalence) and what
// decides N0: strong classes, numbers, a mark, a neutral, an isolate.
constexpr u32 bracket_alphabet[] = {0x0028, 0x0029, 0x005b, 0x005d, 0x2329, 0x3009,
                                    0x0061, 0x05d0, 0x0031, 0x0660, 0x0300, 0x0020, 0x2067, 0x2069};

void exhaustive_brackets(const Data& data, const int longest) {
    const int letters = static_cast<int>(sizeof bracket_alphabet / sizeof bracket_alphabet[0]);
    std::vector<u32> input;
    for (int length = 1; length <= longest; ++length) {
        std::vector<int> digits(static_cast<usize>(length), 0);
        while (true) {
            input.clear();
            for (const int digit : digits) input.push_back(bracket_alphabet[digit]);
            compare(data, input);
            int place = length - 1;
            while (place >= 0 && digits[static_cast<usize>(place)] == letters - 1) {
                digits[static_cast<usize>(place)] = 0;
                place = place - 1;
            }
            if (place < 0) break;
            digits[static_cast<usize>(place)] = digits[static_cast<usize>(place)] + 1;
        }
    }
}

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

// Weighted towards the classes texts are made of, with every class, the
// brackets, CR, LF and a long run of embeddings to pass the depth limit.
constexpr u32 random_alphabet[] = {
    0x0061, 0x0062, 0x0061, 0x05d0, 0x05d1, 0x05d0, 0x0627, 0x0628, 0x0031, 0x0032, 0x0660, 0x06f1,
    0x002b, 0x002d, 0x0024, 0x0025, 0x002c, 0x002e, 0x003a, 0x0300, 0x0301, 0x00ad, 0x200d, 0x0009,
    0x0020, 0x0020, 0x0021, 0x0022, 0x0028, 0x0029, 0x005b, 0x005d, 0x007b, 0x007d, 0x2329, 0x3009,
    0x3008, 0x232a, 0x202a, 0x202b, 0x202c, 0x202d, 0x202e, 0x2066, 0x2067, 0x2068, 0x2069, 0x10fffd,
};

void random_sequences(const Data& data, const int count, Random& random) {
    const u32 letters = static_cast<u32>(sizeof random_alphabet / sizeof random_alphabet[0]);
    std::vector<u32> input;
    for (int index = 0; index < count; ++index) {
        input.clear();
        const u32 length = 1 + random.next(300);
        const bool deep = random.next(8) == 0;
        for (u32 position = 0; position < length; ++position) {
            if (deep && position < 140) {
                input.push_back(random.next(2) == 0 ? 0x202b : 0x2067);
                continue;
            }
            input.push_back(random_alphabet[random.next(letters)]);
        }
        compare(data, input);
    }
}

// ---- B5 -----------------------------------------------------------------

void append_utf8(std::string& bytes, const u32 value) {
    if (value < 0x80) {
        bytes.push_back(static_cast<char>(value));
    } else if (value < 0x800) {
        bytes.push_back(static_cast<char>(0xc0 | (value >> 6)));
        bytes.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else if (value < 0x10000) {
        bytes.push_back(static_cast<char>(0xe0 | (value >> 12)));
        bytes.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        bytes.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    } else {
        bytes.push_back(static_cast<char>(0xf0 | (value >> 18)));
        bytes.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
        bytes.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
        bytes.push_back(static_cast<char>(0x80 | (value & 0x3f)));
    }
}

void utf8_agrees(const int count, Random& random) {
    const u32 letters = static_cast<u32>(sizeof random_alphabet / sizeof random_alphabet[0]);
    const u32 extra[] = {0x000d, 0x000a, 0x2029, 0x00e9, 0x4e2d, 0x1f600};
    std::uint64_t paragraphs = 0;
    std::uint64_t mismatches = 0;
    for (int index = 0; index < count; ++index) {
        std::vector<u32> input;
        std::string bytes;
        std::vector<usize> byte_of; // first byte of each code point
        const u32 length = 1 + random.next(200);
        for (u32 position = 0; position < length; ++position) {
            const u32 value = random.next(6) == 0 ? extra[random.next(6)] : random_alphabet[random.next(letters)];
            input.push_back(value);
            byte_of.push_back(bytes.size());
            append_utf8(bytes, value);
        }
        byte_of.push_back(bytes.size());
        const stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
        if (validated.failed()) {
            mismatches = mismatches + 1;
            continue;
        }
        const stxlib::TextView view = *validated.value();
        usize point = 0;
        const text::BaseDirection direction = direction_of(static_cast<int>(random.next(4)));
        while (point < input.size()) {
            text::Paragraph by_point;
            text::Paragraph by_byte;
            const stxlib::Status a = text::analyse_paragraph(View<u32>(input.data(), input.size()), point, direction, by_point);
            const stxlib::Status b = text::analyse_paragraph(view, byte_of[point], direction, by_byte);
            paragraphs = paragraphs + 1;
            bool same = a.succeeded() && b.succeeded() && by_point.base_level() == by_byte.base_level() &&
                        by_byte.offset() == byte_of[point] &&
                        by_byte.offset() + by_byte.length() == byte_of[point + by_point.length()];
            for (usize k = 0; same && k < by_point.length(); ++k) {
                for (usize unit = byte_of[point + k]; unit < byte_of[point + k + 1]; ++unit) {
                    same = same && by_byte.levels()[unit - by_byte.offset()] == by_point.levels()[k];
                }
            }
            if (same) {
                text::Line line_point;
                text::Line line_byte;
                const stxlib::Status c = text::analyse_line(by_point, by_point.offset(), by_point.length(), line_point);
                const stxlib::Status d = text::analyse_line(by_byte, by_byte.offset(), by_byte.length(), line_byte);
                same = c.succeeded() && d.succeeded() && line_point.runs().count() == line_byte.runs().count();
                for (usize r = 0; same && r < line_point.runs().count(); ++r) {
                    const text::BidiRun& p = line_point.runs()[r];
                    const text::BidiRun& q = line_byte.runs()[r];
                    same = q.offset == byte_of[p.offset] && q.offset + q.length == byte_of[p.offset + p.length] &&
                           q.level == p.level;
                }
            }
            // A paragraph ends after a separator, CR LF together.
            const usize end = point + by_point.length();
            if (same && end < input.size()) {
                const u32 last = input[end - 1];
                same = text::bidi_class(last) == text::BidiClass::paragraph_separator &&
                       !(last == 0x0d && input[end] == 0x0a);
            }
            if (!same) {
                mismatches = mismatches + 1;
                report("B5", describe(input, static_cast<int>(direction)));
                break;
            }
            point = end;
        }
    }
    CHECK(mismatches == 0);
    std::printf("B5: %llu paragraphs, %llu mismatches\n", static_cast<unsigned long long>(paragraphs),
                static_cast<unsigned long long>(mismatches));
}

// ---- B6 -----------------------------------------------------------------

void arguments() {
    const char bytes[] = "a\xd7\x90" "b";
    const stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(bytes, 4);
    CHECK(validated.succeeded());
    const stxlib::TextView view = *validated.value();
    text::Paragraph paragraph;
    CHECK(text::analyse_paragraph(view, 4, text::BaseDirection::left_to_right, paragraph).failed());
    CHECK(text::analyse_paragraph(view, 2, text::BaseDirection::left_to_right, paragraph).failed());
    CHECK(paragraph.length() == 0 && paragraph.levels().count() == 0);
    CHECK(text::analyse_paragraph(view, 0, text::BaseDirection::left_to_right, paragraph).succeeded());
    CHECK(paragraph.length() == 4);
    CHECK(paragraph.levels().count() == 4 && paragraph.levels()[1] == 1 && paragraph.levels()[2] == 1);
    text::Line line;
    CHECK(text::analyse_line(paragraph, 0, 0, line).failed());
    CHECK(text::analyse_line(paragraph, 0, 5, line).failed());
    CHECK(text::analyse_line(paragraph, 2, 2, line).failed()); // inside U+05D0
    CHECK(line.runs().count() == 0 && line.length() == 0);
    CHECK(text::analyse_line(paragraph, 1, 2, line).succeeded());
    CHECK(line.runs().count() == 1 && line.runs()[0].offset == 1 && line.runs()[0].length == 2 && line.runs()[0].level == 1);
    // A uniform paragraph beyond ASCII still checks boundaries.
    const char latin[] = "\xc3\xa9t\xc3\xa9";
    const stxlib::Result<stxlib::TextView> latin_view = stxlib::validate_utf8(latin, 5);
    CHECK(latin_view.succeeded());
    CHECK(text::analyse_paragraph(*latin_view.value(), 0, text::BaseDirection::automatic_left_to_right, paragraph).succeeded());
    CHECK(text::analyse_line(paragraph, 1, 2, line).failed());
    CHECK(text::analyse_line(paragraph, 2, 3, line).succeeded());
    CHECK(line.runs().count() == 1 && line.runs()[0].level == 0);
    // Mirroring and brackets of one code point.
    CHECK(text::bidi_mirroring_glyph(0x28) == 0x29 && text::bidi_mirrored(0x221b) && text::bidi_mirroring_glyph(0x221b) == 0x221b);
    CHECK(text::bidi_paired_bracket(0x5b).pair == 0x5d && text::bidi_paired_bracket(0x5b).type == text::BracketType::open);
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stxlib_textlayout_bidi_tests <unicode directory> [<conformance directory>] [--exhaustive N] [--random N]\n");
        return 2;
    }
    std::string conformance;
    int longest = 4;
    int random_count = 20000;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--exhaustive" && index + 1 < argc) {
            longest = std::atoi(argv[index + 1]);
            index = index + 1;
        } else if (argument == "--random" && index + 1 < argc) {
            random_count = std::atoi(argv[index + 1]);
            index = index + 1;
        } else {
            conformance = argument;
        }
    }
    const Data data = read_data(argv[1]);
    CHECK(data.read);
    if (!data.read) return stxlib_tests::finish("stxlib_textlayout_bidi_tests");
    lookups(data);
    if (conformance.empty()) {
        std::printf("B2, B3: NOT RUN: no conformance directory given (STXLIB_UNICODE_TEST_DATA)\n");
    } else {
        bidi_test(conformance);
        bidi_character_test(conformance);
    }
    exhaustive(data, longest);
    const std::uint64_t after_classes = compared;
    exhaustive_brackets(data, 6);
    const std::uint64_t after_brackets = compared;
    Random random;
    random_sequences(data, random_count, random);
    CHECK(differing == 0);
    std::printf("B4: %llu sequences with a separator before the end left to B5\n",
                static_cast<unsigned long long>(skipped_paragraphs));
    std::printf("B4: %llu comparisons (classes to length %d: %llu; brackets to length 6: %llu; random: %llu), %llu differ\n",
                static_cast<unsigned long long>(compared), longest, static_cast<unsigned long long>(after_classes),
                static_cast<unsigned long long>(after_brackets - after_classes),
                static_cast<unsigned long long>(compared - after_brackets), static_cast<unsigned long long>(differing));
    utf8_agrees(random_count / 4, random);
    arguments();
    return stxlib_tests::finish("stxlib_textlayout_bidi_tests");
}
