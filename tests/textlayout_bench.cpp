// Speed of include/stxlib/textlayout.hpp against the three things it
// replaces, and the differences from each: SheenBidi 2.9.0 (the
// bidirectional algorithm and its script locator) and libunibreak 8.0
// (line breaking). Built only when STXLIB_SHEENBIDI_DIR and
// STXLIB_LIBUNIBREAK_DIR name their sources; never part of the library.
//
// Usage: stxlib_textlayout_bench [<directory of BidiCharacterTest.txt and
// LineBreakTest.txt>] [--random N]
//
// Workloads (each a UTF-8 text of paragraphs):
//   ascii     English UI strings (GUI.Forms' text is this form throughout)
//   latin     accented Latin and CJK, no right-to-left text
//   hebrew    Hebrew sentences with numbers, punctuation and brackets
//   mixed     English with embedded Arabic and numbers
//   explicit  isolates and embeddings around mixed text
// Each is timed as the best of 15 repetitions of the whole text, in
// nanoseconds per byte. The library's one pass (analyse_paragraph and
// analyse_line per paragraph: levels, runs, line breaks, script runs) is
// timed against the three run back to back, as GUI.Forms calls them:
// SheenBidi paragraph by paragraph with one line each,
// set_linebreaks_utf8 with "-strict", and SBScriptLocator over the text.
// find_line_breaks alone is timed against set_linebreaks_utf8 alone.
//
// The oracle comparisons: levels on BidiCharacterTest.txt; line breaks on
// LineBreakTest.txt and on random texts; script runs on random texts.
// Every difference is counted under a reason the program can decide for
// itself (the two libraries' property data differ at a code point of the
// text; the end of the text; a rule), and the rule differences are listed
// by the classes around them.

#include "stxlib/textlayout.hpp"
#include "stxlib/text.hpp"

extern "C" {
#include <SheenBidi/SheenBidi.h>
#include <linebreak.h>
#include <linebreakdef.h>
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace text = stxlib::textlayout;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

std::uint64_t sink = 0;

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

std::string repeat_paragraphs(const std::vector<std::string>& lines, const usize target) {
    std::string result;
    usize index = 0;
    while (result.size() < target) {
        result += lines[index % lines.size()];
        result += '\n';
        index = index + 1;
    }
    return result;
}

std::string encode(const std::vector<u32>& values) {
    std::string result;
    for (const u32 value : values) append_utf8(result, value);
    return result;
}

void run_stxlib(const std::string& bytes) {
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    const stxlib::TextView view = *validated.value();
    text::Paragraph paragraph;
    text::Line line;
    usize offset = 0;
    while (offset < bytes.size()) {
        const stxlib::OperationStatus a = text::analyse_paragraph(view, offset,
                                                                  text::BaseDirection::automatic_left_to_right,
                                                                  paragraph);
        if (a.failed()) std::abort();
        const stxlib::OperationStatus b = text::analyse_line(paragraph, paragraph.offset(), paragraph.length(), line);
        if (b.failed()) std::abort();
        sink = sink + line.runs().count();
        offset = offset + paragraph.length();
    }
}

void run_sheenbidi(const std::string& bytes) {
    const SBCodepointSequence sequence{SBStringEncodingUTF8, const_cast<char*>(bytes.data()), bytes.size()};
    const SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
    usize offset = 0;
    while (offset < bytes.size()) {
        const SBParagraphRef paragraph =
            SBAlgorithmCreateParagraph(algorithm, offset, bytes.size() - offset, SBLevelDefaultLTR);
        const usize length = SBParagraphGetLength(paragraph);
        const SBLineRef line = SBParagraphCreateLine(paragraph, offset, length);
        sink = sink + SBLineGetRunCount(line);
        SBLineRelease(line);
        SBParagraphRelease(paragraph);
        offset = offset + length;
    }
    SBAlgorithmRelease(algorithm);
}

void run_libunibreak(const std::string& bytes) {
    static std::vector<char> decisions;
    decisions.resize(bytes.size());
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(bytes.data()), bytes.size(), "-strict", decisions.data());
    sink = sink + static_cast<unsigned char>(decisions[bytes.size() / 2]);
}

void run_script_locator(const std::string& bytes) {
    const SBCodepointSequence sequence{SBStringEncodingUTF8, const_cast<char*>(bytes.data()), bytes.size()};
    const SBScriptLocatorRef locator = SBScriptLocatorCreate();
    SBScriptLocatorLoadCodepoints(locator, &sequence);
    while (SBScriptLocatorMoveNext(locator)) sink = sink + SBScriptLocatorGetAgent(locator)->length;
    SBScriptLocatorRelease(locator);
}

void run_find_line_breaks(const std::string& bytes) {
    static std::vector<text::LineBreak> decisions;
    decisions.resize(bytes.size());
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    const stxlib::OperationStatus status = text::find_line_breaks(
        stxlib::MutableView<text::LineBreak>(decisions.data(), decisions.size()), *validated.value());
    if (status.failed()) std::abort();
    sink = sink + static_cast<unsigned char>(decisions[bytes.size() / 2]);
}

// Validation alone, which every stxlib entry above pays for here and
// which a caller holding a TextView does not pay again.
void run_validate(const std::string& bytes) {
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    sink = sink + (validated.failed() ? 1u : 0u);
}

double best_ns_per_byte(void (*function)(const std::string&), const std::string& bytes) {
    double best = 1e30;
    for (int repetition = 0; repetition < 15; ++repetition) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        function(bytes);
        const std::chrono::steady_clock::time_point stop = std::chrono::steady_clock::now();
        const double ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
        if (ns < best) best = ns;
    }
    const double result = best / static_cast<double>(bytes.size());
    return result;
}

void time_workload(const char* const name, const std::string& bytes) {
    const double ours = best_ns_per_byte(run_stxlib, bytes);
    const double bidi = best_ns_per_byte(run_sheenbidi, bytes);
    const double breaks = best_ns_per_byte(run_libunibreak, bytes);
    const double scripts = best_ns_per_byte(run_script_locator, bytes);
    const double ours_breaks = best_ns_per_byte(run_find_line_breaks, bytes);
    const double validation = best_ns_per_byte(run_validate, bytes);
    const double theirs = bidi + breaks + scripts;
    std::printf("bench %-8s %8zu bytes  one pass: stxlib %.3f ns/byte  three libraries %.3f (sheenbidi %.3f + "
                "libunibreak %.3f + script locator %.3f)  speed-up %.1fx; against sheenbidi alone %.1fx\n",
                name, bytes.size(), ours, theirs, bidi, breaks, scripts, theirs / ours, bidi / ours);
    std::printf("bench %-8s line breaks alone: stxlib %.3f ns/byte  libunibreak %.3f  speed-up %.1fx  "
                "(validate_utf8, inside both stxlib figures: %.3f)\n",
                name, ours_breaks, breaks, breaks / ours_breaks, validation);
}

// The differences between the library and SheenBidi on each case of
// BidiCharacterTest.txt: levels of the characters not removed by X9, and
// the paragraph level.
void compare_file(const char* const path) {
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::printf("oracle: NOT RUN: %s absent\n", path);
        return;
    }
    char buffer[8192];
    std::uint64_t cases = 0;
    std::uint64_t differ = 0;
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        const std::string line(buffer);
        if (line.empty() || line[0] == '#' || line[0] == '\n') continue;
        const usize semicolon = line.find(';');
        if (semicolon == std::string::npos) continue;
        std::vector<u32> values;
        const char* cursor = line.c_str();
        const char* const end = cursor + semicolon;
        while (cursor < end) {
            char* next = nullptr;
            const unsigned long value = std::strtoul(cursor, &next, 16);
            if (next == cursor) break;
            values.push_back(static_cast<u32>(value));
            cursor = next;
        }
        const int file_direction = line[semicolon + 1] - '0';
        const text::BaseDirection direction = file_direction == 0 ? text::BaseDirection::left_to_right
                                              : file_direction == 1 ? text::BaseDirection::right_to_left
                                                                    : text::BaseDirection::automatic_left_to_right;
        const SBLevel sb_level = file_direction == 0 ? 0 : (file_direction == 1 ? 1 : SBLevelDefaultLTR);
        cases = cases + 1;
        text::Paragraph paragraph;
        const stxlib::OperationStatus analysed =
            text::analyse_paragraph(stxlib::View<u32>(values.data(), values.size()), 0, direction, paragraph);
        std::vector<SBUInt32> units(values.begin(), values.end());
        const SBCodepointSequence sequence{SBStringEncodingUTF32, units.data(), units.size()};
        const SBAlgorithmRef algorithm = SBAlgorithmCreate(&sequence);
        const SBParagraphRef sb = SBAlgorithmCreateParagraph(algorithm, 0, units.size(), sb_level);
        bool same = analysed.succeeded() && paragraph.length() == SBParagraphGetLength(sb) &&
                    paragraph.base_level() == SBParagraphGetBaseLevel(sb);
        const SBLevel* const levels = SBParagraphGetLevelsPtr(sb);
        for (usize index = 0; same && index < values.size(); ++index) {
            const text::BidiClass kind = text::bidi_class(values[index]);
            const bool removed = kind == text::BidiClass::boundary_neutral ||
                                 (kind >= text::BidiClass::left_to_right_embedding &&
                                  kind <= text::BidiClass::pop_directional_format);
            if (!removed && paragraph.levels()[index] != levels[index]) same = false;
        }
        if (!same) {
            differ = differ + 1;
            if (differ <= 20) std::printf("oracle differs: %s", buffer);
        }
        SBParagraphRelease(sb);
        SBAlgorithmRelease(algorithm);
    }
    std::fclose(file);
    std::printf("oracle: %llu cases, %llu differ between stxlib and SheenBidi\n", static_cast<unsigned long long>(cases),
                static_cast<unsigned long long>(differ));
}

// --- libunibreak ------------------------------------------------------------

const char* const their_names[] = {
    "??", "OP", "CL", "CP", "QU", "GL", "NS", "EX", "SY", "IS", "PR", "PO", "NU", "AL", "HL", "ID", "IN", "HY", "BA",
    "BB", "B2", "ZW", "CM", "WJ", "H2", "H3", "JL", "JV", "JT", "RI", "EB", "EM", "ZWJ", "AK", "AP", "AS", "VF", "VI",
    "HH", "CB", "AI", "BK", "CJ", "CR", "LF", "NL", "SA", "SG", "SP", "XX",
};

const char* const our_names[text::line_break_class_count] = {
    "BK", "CR", "LF", "NL", "SP", "ZW", "WJ", "GL", "CM", "ZWJ", "OP", "CL", "CP", "QU", "NS",
    "EX", "SY", "IS", "PR", "PO", "NU", "AL", "HL", "ID", "IN",  "HY", "HH", "BA", "BB", "B2",
    "CB", "H2", "H3", "JL", "JV", "JT", "RI", "EB", "EM", "AK",  "AP", "AS", "VF", "VI",
};

const char* our_class(const u32 code) {
    const u32 value = static_cast<u32>(text::line_break_class(code));
    return value < text::line_break_class_count ? our_names[value] : "??";
}

// libunibreak's Line_Break value (its tables, Unicode 17.0.0), before its
// own resolution of AI, SA, SG, XX and CJ.
const char* their_class(const u32 code) {
    unsigned value = LBP_XX;
    if (code < 65536) {
        value = static_cast<unsigned char>(lb_prop_bmp[code]);
    } else {
        for (unsigned index = 0; index < lb_prop_supplementary_len; ++index) {
            if (code >= lb_prop_supplementary[index].start && code <= lb_prop_supplementary[index].end) {
                value = static_cast<unsigned>(lb_prop_supplementary[index].prop);
                break;
            }
        }
    }
    return value < sizeof their_names / sizeof their_names[0] ? their_names[value] : "??";
}

// Whether the two libraries' data give `code` the same class, after LB1.
bool same_data(const u32 code) {
    const std::string theirs = their_class(code);
    const std::string ours = our_class(code);
    if (theirs == "AI" || theirs == "SG" || theirs == "XX") return ours == "AL";
    if (theirs == "CJ") return ours == "NS";
    if (theirs == "SA") return ours == "AL" || ours == "CM";
    return theirs == ours;
}

struct BreakCounts final {
    std::uint64_t texts{0};
    std::uint64_t boundaries{0};
    std::uint64_t end_of_text{0};      // libunibreak: indeterminate; stxlib: mandatory (LB3)
    std::uint64_t texts_differing{0};  // anywhere but the end
    std::uint64_t by_data{0};          // of those: a code point's class differs between the two data sets
    std::uint64_t by_rule{0};
    std::map<std::string, std::uint64_t> causes;
    std::map<std::string, std::uint64_t> signatures;
    std::map<std::string, std::string> examples;
};

bool scalar(const u32 value) { return value <= 0x10ffffu && !(value >= 0xd800u && value <= 0xdfffu); }

void compare_breaks(const std::vector<u32>& values, BreakCounts& counts) {
    std::string bytes;
    std::vector<usize> last;
    for (const u32 value : values) {
        append_utf8(bytes, value);
        last.push_back(bytes.size() - 1);
    }
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    if (validated.failed()) return;
    std::vector<text::LineBreak> ours(bytes.size(), text::LineBreak::prohibited);
    const stxlib::OperationStatus status =
        text::find_line_breaks(stxlib::MutableView<text::LineBreak>(ours.data(), ours.size()), *validated.value());
    if (status.failed()) std::abort();
    std::vector<char> theirs(bytes.size(), 0);
    set_linebreaks_utf8(reinterpret_cast<const utf8_t*>(bytes.data()), bytes.size(), "-strict", theirs.data());
    counts.texts = counts.texts + 1;
    bool data = false;
    for (const u32 value : values) data = data || !same_data(value);
    bool differs = false;
    for (usize index = 0; index < values.size(); ++index) {
        const usize byte = last[index];
        const int mine = static_cast<int>(ours[byte]);
        int other = -1;
        if (theirs[byte] == LINEBREAK_MUSTBREAK) other = 2;
        if (theirs[byte] == LINEBREAK_ALLOWBREAK) other = 1;
        if (theirs[byte] == LINEBREAK_NOBREAK) other = 0;
        counts.boundaries = counts.boundaries + 1;
        if (index + 1 == values.size()) {
            if (other != mine) counts.end_of_text = counts.end_of_text + 1;
            continue;
        }
        if (other == mine) continue;
        if (differs) continue; // the first difference of a text names it
        differs = true;
        if (data) continue;
        // The cause the program can name, then the classes at the
        // boundary: the base before it (marks passed over), the code
        // point after it, and the base after that.
        usize before = index;
        while (before > 0 && (std::strcmp(our_class(values[before]), "CM") == 0 ||
                              std::strcmp(our_class(values[before]), "ZWJ") == 0)) {
            before = before - 1;
        }
        usize following = index + 2;
        while (following < values.size() && (std::strcmp(our_class(values[following]), "CM") == 0 ||
                                             std::strcmp(our_class(values[following]), "ZWJ") == 0)) {
            following = following + 1;
        }
        const std::string left = our_class(values[before]);
        const std::string right = our_class(values[index + 1]);
        const std::string beyond = following < values.size() ? our_class(values[following]) : "eot";
        const bool marks = before != index || following != index + 2;
        std::string cause = "other";
        if (other == 0 && ((values[index + 1] == '+' && left == "PR") || (values[index + 1] == '`' && left == "HY"))) {
            cause = "libunibreak's tailoring (no break in \"++\" and \"-`\")";
        } else if (mine == 0 && left == "BA" && right == "GL") {
            cause = "LB12a at 18.0.0 (BA x GL)";
        } else if (right == "QU" || left == "QU") {
            cause = marks ? "quotation marks (LB15a, LB15b, LB19, LB19a), combining marks near" : "quotation marks (LB15a, LB15b, LB19, LB19a)";
        } else if (right == "IS" && std::strcmp(our_class(values[index]), "SP") == 0) {
            cause = "LB15c (SP before IS)";
        } else if ((left == "PR" || left == "PO") && right == "OP") {
            cause = "LB25 ((PR | PO) x OP NU)";
        } else if (marks) {
            cause = "other, combining marks near";
        }
        std::string signature = cause + ": " + left + (text::east_asian_wide(values[before]) ? "w" : "") +
                                (before != index ? " CM" : "") + " | " + right +
                                (text::east_asian_wide(values[index + 1]) ? "w " : " ") + beyond + ", ";
        counts.causes[cause] = counts.causes[cause] + 1;
        signature += "stxlib " + std::to_string(mine) + " libunibreak " + std::to_string(other);
        counts.signatures[signature] = counts.signatures[signature] + 1;
        if (counts.examples.find(signature) == counts.examples.end()) {
            std::string example;
            for (const u32 value : values) {
                char piece[16];
                std::snprintf(piece, sizeof piece, "%04X ", static_cast<unsigned>(value));
                example += piece;
            }
            counts.examples[signature] = example;
        }
    }
    if (differs) {
        counts.texts_differing = counts.texts_differing + 1;
        if (data) {
            counts.by_data = counts.by_data + 1;
        } else {
            counts.by_rule = counts.by_rule + 1;
        }
    }
}

void print_breaks(const char* const name, const BreakCounts& counts) {
    std::printf("libunibreak %s: %llu texts, %llu boundaries; end of text differs on %llu (indeterminate against "
                "mandatory); %llu texts differ elsewhere: %llu with a code point whose class differs between the "
                "data sets, %llu by rule\n",
                name, static_cast<unsigned long long>(counts.texts), static_cast<unsigned long long>(counts.boundaries),
                static_cast<unsigned long long>(counts.end_of_text), static_cast<unsigned long long>(counts.texts_differing),
                static_cast<unsigned long long>(counts.by_data), static_cast<unsigned long long>(counts.by_rule));
    std::vector<std::pair<std::uint64_t, std::string>> sorted;
    for (const std::pair<const std::string, std::uint64_t>& entry : counts.signatures) {
        sorted.push_back(std::pair<std::uint64_t, std::string>(entry.second, entry.first));
    }
    for (usize a = 0; a < sorted.size(); ++a) {
        for (usize b = a + 1; b < sorted.size(); ++b) {
            if (sorted[b].first > sorted[a].first) std::swap(sorted[a], sorted[b]);
        }
    }
    for (const std::pair<const std::string, std::uint64_t>& entry : counts.causes) {
        std::printf("libunibreak %s: cause %8llu  %s\n", name, static_cast<unsigned long long>(entry.second),
                    entry.first.c_str());
    }
    std::printf("libunibreak %s: %zu kinds of rule difference (base before, '|', code point after, base after that; "
                "0 no break, 1 allowed, 2 mandatory); the first 14 of each cause:\n", name, sorted.size());
    for (const std::pair<const std::string, std::uint64_t>& cause : counts.causes) {
        usize shown = 0;
        for (usize index = 0; index < sorted.size() && shown < 14; ++index) {
            if (sorted[index].second.compare(0, cause.first.size() + 1, cause.first + ":") != 0) continue;
            shown = shown + 1;
            std::printf("  %8llu  %s   e.g. %s\n", static_cast<unsigned long long>(sorted[index].first),
                        sorted[index].second.c_str(), counts.examples.find(sorted[index].second)->second.c_str());
        }
    }
}

void compare_line_break_test(const std::string& path) {
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        std::printf("libunibreak LineBreakTest: NOT RUN: %s absent\n", path.c_str());
        return;
    }
    BreakCounts counts;
    char buffer[16384];
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        std::string line(buffer);
        const usize hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::vector<u32> values;
        usize index = 0;
        bool usable = true;
        while (index < line.size()) {
            const unsigned char character = static_cast<unsigned char>(line[index]);
            const bool digit = (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F');
            if (!digit) {
                index = index + 1;
                continue;
            }
            usize end = index;
            while (end < line.size() && line[end] != ' ' && line[end] != '\t') end = end + 1;
            const u32 value = static_cast<u32>(std::strtoul(line.substr(index, end - index).c_str(), nullptr, 16));
            usable = usable && scalar(value);
            values.push_back(value);
            index = end;
        }
        if (usable && !values.empty()) compare_breaks(values, counts);
    }
    std::fclose(file);
    print_breaks("LineBreakTest", counts);
}

struct Random final {
    std::uint64_t state{0x9e3779b97f4a7c15u};
    u32 next(const u32 bound) {
        state ^= (state & 0x0007ffffffffffffu) << 13;
        state ^= state >> 7;
        state ^= (state & 0x00007fffffffffffu) << 17;
        const u32 result = static_cast<u32>((state >> 32) % bound);
        return result;
    }
};

// Up to three scalar values of each (class, wide) pair, the characters
// the rules name one by one, and the common ones several times over.
std::vector<u32> break_pool() {
    std::vector<u32> pool;
    std::map<u32, int> seen;
    for (u32 code = 0; code < 0x110000u; ++code) {
        if (!scalar(code)) continue;
        const u32 key = (static_cast<u32>(text::line_break_class(code)) << 1) | (text::east_asian_wide(code) ? 1u : 0u);
        if (seen[key] < 3) {
            seen[key] = seen[key] + 1;
            pool.push_back(code);
        }
    }
    const u32 common[] = {'a', 'b', 'e', 't', 'Z', ' ', ' ', ' ', ' ', ',', '.', '-', '1', '2', '(', ')', '"', '\'', '$',
                          '%', '/', '+', '`', 0x300, 0x200d, 0x2018, 0x2019, 0x201c, 0x201d, 0xab, 0xbb, 0x25cc, 0x4e00,
                          0x3042, 0x5d0, 0x627, 0x915, 0x94d, 0x1f1e6, 0x3001, 0xa0, 0x2014, 0x2010, 0x1f600, 0x1f3fb};
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const u32 value : common) pool.push_back(value);
    }
    return pool;
}

void compare_random_breaks(const int count) {
    const std::vector<u32> pool = break_pool();
    BreakCounts counts;
    Random random;
    std::vector<u32> values;
    for (int index = 0; index < count; ++index) {
        const u32 length = 1 + random.next(24);
        values.clear();
        for (u32 position = 0; position < length; ++position) values.push_back(pool[random.next(static_cast<u32>(pool.size()))]);
        compare_breaks(values, counts);
    }
    print_breaks("random", counts);
}

// --- SheenBidi's script locator ----------------------------------------------

std::string tag_text(const u32 tag) {
    std::string result;
    result.push_back(static_cast<char>((tag >> 24) & 0xffu));
    result.push_back(static_cast<char>((tag >> 16) & 0xffu));
    result.push_back(static_cast<char>((tag >> 8) & 0xffu));
    result.push_back(static_cast<char>(tag & 0xffu));
    return result;
}

// The Script of every code point as an ISO 15924 tag, in both.
void compare_script_property() {
    std::map<std::string, std::uint64_t> pairs;
    std::uint64_t differ = 0;
    for (u32 code = 0; code < 0x110000u; ++code) {
        const u32 ours = text::script_tag(text::script_of(code));
        const u32 theirs = SBScriptGetUnicodeTag(SBCodepointGetScript(code));
        if (ours == theirs) continue;
        differ = differ + 1;
        const std::string key = "stxlib " + tag_text(ours) + " SheenBidi " + tag_text(theirs);
        pairs[key] = pairs[key] + 1;
    }
    std::printf("script property: %llu code points differ between stxlib (Unicode 18.0.0) and SheenBidi, in %zu kinds\n",
                static_cast<unsigned long long>(differ), pairs.size());
    for (const std::pair<const std::string, std::uint64_t>& entry : pairs) {
        std::printf("  %8llu  %s\n", static_cast<unsigned long long>(entry.second), entry.first.c_str());
    }
}

void compare_scripts(const int count) {
    // The pool: two code points of every script, common and inherited
    // marks, brackets and quotes.
    std::vector<u32> pool;
    std::map<u32, int> seen;
    for (u32 code = 0; code < 0x110000u; ++code) {
        if (!scalar(code) || text::bidi_class(code) == text::BidiClass::paragraph_separator) continue;
        const u32 key = static_cast<u32>(text::script_of(code));
        if (seen[key] < 2) {
            seen[key] = seen[key] + 1;
            pool.push_back(code);
        }
    }
    const u32 common[] = {'a', 'b', ' ', ' ', ',', '.', '1', '(', ')', '[', ']', '{', '}', 0x300, 0x301, 0x3b1, 0x3b2,
                          0x416, 0x5d0, 0x627, 0x64b, 0x915, 0x951, 0x3042, 0x30a2, 0x30fc, 0x4e00, 0x3001, 0x300c, 0x300d,
                          0xac00, 0x2018, 0x201c, 0xab, 0xbb, 0x1f600};
    for (int repeat = 0; repeat < 4; ++repeat) {
        for (const u32 value : common) pool.push_back(value);
    }
    std::uint64_t texts = 0;
    std::uint64_t differ = 0;
    std::uint64_t by_data = 0;
    std::uint64_t by_extensions = 0;
    std::uint64_t by_brackets = 0;
    std::uint64_t by_other = 0;
    Random random;
    std::vector<u32> values;
    text::Paragraph paragraph;
    const SBScriptLocatorRef locator = SBScriptLocatorCreate();
    for (int index = 0; index < count; ++index) {
        const u32 length = 1 + random.next(16);
        values.clear();
        for (u32 position = 0; position < length; ++position) values.push_back(pool[random.next(static_cast<u32>(pool.size()))]);
        const stxlib::OperationStatus analysed = text::analyse_paragraph(stxlib::View<u32>(values.data(),
                                                                                           values.size()), 0,
                                                                         text::BaseDirection::left_to_right, paragraph);
        if (analysed.failed() || paragraph.length() != values.size()) std::abort();
        const stxlib::View<text::ScriptRun> runs = paragraph.script_runs();
        std::vector<SBUInt32> units(values.begin(), values.end());
        const SBCodepointSequence sequence{SBStringEncodingUTF32, units.data(), units.size()};
        SBScriptLocatorLoadCodepoints(locator, &sequence);
        usize run = 0;
        bool same = true;
        while (SBScriptLocatorMoveNext(locator)) {
            const SBScriptAgent* const agent = SBScriptLocatorGetAgent(locator);
            if (run >= runs.count() || runs[run].offset != agent->offset || runs[run].length != agent->length ||
                text::script_tag(runs[run].script) != SBScriptGetUnicodeTag(agent->script)) {
                same = false;
            }
            run = run + 1;
        }
        same = same && run == runs.count();
        texts = texts + 1;
        if (same) continue;
        differ = differ + 1;
        bool data = false;
        bool extensions = false;
        bool brackets = false;
        for (const u32 value : values) {
            const text::Script own = text::script_of(value);
            if (text::script_tag(own) != SBScriptGetUnicodeTag(SBCodepointGetScript(value))) data = true;
            for (u32 other = 0; other < 256; ++other) {
                const text::Script script = static_cast<text::Script>(other);
                if (script != own && text::script_extends_to(value, script)) extensions = true;
            }
            const SBGeneralCategory category = SBCodepointGetGeneralCategory(value);
            if (category == SBGeneralCategoryPS || category == SBGeneralCategoryPE ||
                text::bidi_paired_bracket(value).type != text::BracketType::none) {
                brackets = true;
            }
        }
        if (data) {
            by_data = by_data + 1;
        } else if (extensions) {
            by_extensions = by_extensions + 1;
        } else if (brackets) {
            by_brackets = by_brackets + 1;
        } else {
            by_other = by_other + 1;
            if (by_other <= 20) {
                std::printf("script locator differs:");
                for (const u32 value : values) std::printf(" %04X", static_cast<unsigned>(value));
                std::printf("\n");
            }
        }
    }
    SBScriptLocatorRelease(locator);
    std::printf("script locator: %llu texts, %llu differ: %llu with a code point whose Script differs between the data "
                "sets, %llu with a code point that has Script_Extensions, %llu with brackets, %llu other\n",
                static_cast<unsigned long long>(texts), static_cast<unsigned long long>(differ),
                static_cast<unsigned long long>(by_data), static_cast<unsigned long long>(by_extensions),
                static_cast<unsigned long long>(by_brackets), static_cast<unsigned long long>(by_other));
}

} // namespace

int main(const int argc, char** const argv) {
    const std::vector<std::string> ascii = {
        "Open the file you want to edit, then press Save.",
        "Window width: 1280 px (minimum 640), height 720 px.",
        "Error 404: the page \"settings/display\" was not found [retry?]",
        "x = 3.25; y = -17; total = $1,024.00 (approx.)",
    };
    std::vector<std::string> latin;
    latin.push_back(encode({0xc9, 't', 0xe9, ' ', 0xe0, ' ', 'P', 'a', 'r', 'i', 's', ',', ' ', 0x4e2d, 0x6587, 0x6587,
                            0x5b57, ' ', '(', 'U', 'T', 'F', '-', '8', ')', '.'}));
    std::vector<std::string> hebrew;
    hebrew.push_back(encode({0x05e9, 0x05dc, 0x05d5, 0x05dd, ' ', 0x05e2, 0x05d5, 0x05dc, 0x05dd, ' ', '(', '1', '2', '3',
                             ')', ' ', 0x05d1, 0x05d9, 0x05ea, ',', ' ', '4', '5', '%', ' ', 0x05e1, 0x05e4, 0x05e8, '.'}));
    std::vector<std::string> mixed;
    mixed.push_back(encode({'T', 'h', 'e', ' ', 'w', 'o', 'r', 'd', ' ', 0x0645, 0x0631, 0x062d, 0x0628, 0x0627, ' ', 'm',
                            'e', 'a', 'n', 's', ' ', 'h', 'e', 'l', 'l', 'o', ' ', '[', 0x0661, 0x0662, ']', ' ', 'o', 'k',
                            '.'}));
    std::vector<std::string> explicit_text;
    explicit_text.push_back(encode({'U', 's', 'e', 'r', ' ', 0x2067, 0x05d3, 0x05e0, 0x05d4, ' ', '4', '2', 0x2069, ' ',
                                    's', 'a', 'i', 'd', ' ', 0x202b, 'h', 'i', ' ', 0x05e9, 0x202c, ' ', 'x', '.'}));
    const usize size = 4u << 20;
    time_workload("ascii", repeat_paragraphs(ascii, size));
    time_workload("latin", repeat_paragraphs(latin, size));
    time_workload("hebrew", repeat_paragraphs(hebrew, size));
    time_workload("mixed", repeat_paragraphs(mixed, size));
    time_workload("explicit", repeat_paragraphs(explicit_text, size));
    std::string directory;
    int random_count = 2000000;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--random" && index + 1 < argc) {
            random_count = std::atoi(argv[index + 1]);
            index = index + 1;
        } else {
            directory = argument;
        }
    }
    if (directory.empty()) {
        std::printf("oracle: NOT RUN on the conformance files: no directory given\n");
    } else {
        compare_file((directory + "/BidiCharacterTest.txt").c_str());
        compare_line_break_test(directory + "/LineBreakTest.txt");
    }
    compare_random_breaks(random_count);
    compare_script_property();
    compare_scripts(random_count);
    std::printf("sink %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}
