// Speed of include/stxlib/bidi.hpp against SheenBidi 2.9.0 (the oracle it
// replaces), and the differences between the two on BidiCharacterTest.txt.
// Built only when STXLIB_SHEENBIDI_DIR names SheenBidi's source (stx_text's
// vendored copy); never part of the library.
//
// Usage: stxlib_bidi_bench [<BidiCharacterTest.txt>]
//
// Workloads (each a UTF-8 text of paragraphs, analysed paragraph by
// paragraph, one line per paragraph, as GUI.Forms calls SheenBidi):
//   ascii     English UI strings (GUI.Forms' text is this form throughout)
//   latin     accented Latin and CJK, no right-to-left text
//   hebrew    Hebrew sentences with numbers, punctuation and brackets
//   mixed     English with embedded Arabic and numbers
//   explicit  isolates and embeddings around mixed text
// Each is timed as the best of 15 repetitions of the whole text; reported
// in nanoseconds per byte for both, and the ratio.

#include "stxlib/bidi.hpp"
#include "stxlib/text.hpp"

extern "C" {
#include <SheenBidi/SheenBidi.h>
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace text = stxlib::text;
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
    const stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(bytes.data(), bytes.size());
    const stxlib::TextView view = *validated.value();
    text::BidiParagraph paragraph;
    text::BidiLine line;
    usize offset = 0;
    while (offset < bytes.size()) {
        const stxlib::Status a = text::analyse_paragraph(view, offset, text::BaseDirection::automatic_left_to_right, paragraph);
        if (a.failed()) std::abort();
        const stxlib::Status b = text::analyse_line(paragraph, paragraph.offset(), paragraph.length(), line);
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
    const double theirs = best_ns_per_byte(run_sheenbidi, bytes);
    std::printf("bench %-8s %8zu bytes  stxlib %.3f ns/byte  sheenbidi %.3f ns/byte  speed-up %.1fx\n", name,
                bytes.size(), ours, theirs, theirs / ours);
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
        text::BidiParagraph paragraph;
        const stxlib::Status analysed =
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
    if (argc > 1) compare_file(argv[1]);
    std::printf("sink %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}
