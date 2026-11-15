// Proof harness for the platform layer's pure cores (A1 platform-rest):
// exhaustive enumeration of the domains below against independent
// references written here (FORWARD_PROGRAMME section 10).
//
// Properties:
//   W1  For every command line of 0 to 7 bytes over {a, space, tab, '"',
//       '\'} (97,656 lines): split_windows_command_line gives the arguments
//       of the reference splitter below (the documented rules restated as a
//       backslash counter), its counts meet the stated bounds
//       (text_bytes <= count + arguments, arguments <= count / 2 + 1), a
//       measuring call agrees with a writing call, and a writing call into
//       rooms one byte or one start short writes nothing outside them.
//   W2  For every list of 1 to 3 arguments, each of 0 to 3 bytes over the
//       same five bytes (156 + 156^2 + 156^3 = 3,821,532 lists):
//       quoting each with quote_windows_argument after the program name "p"
//       and splitting the line gives back "p" and the list, and each quoting
//       takes at most 2n + 2 bytes.
//   W3  The six examples of Microsoft Learn's "Parsing C++ command-line
//       arguments" table split as documented (the vectors, not exhaustive).
//   S1  For every capacity 0 to 200 and each frame of a fixed set (control
//       bytes, names of 0 to 300 bytes, unknown images, the largest index
//       and addresses): stack_line::format writes at most the capacity, ends
//       with '\n' when the capacity is not zero, holds no other control
//       byte, writes nothing past the capacity, and equals the reference
//       line's prefix (with the newline) when it is cut.
//   P1  pe_image on a synthetic PE32+ image: read_headers, find_section
//       (exact and grouped "name$x" names) and nearest_export give the
//       values the image was built with.
//   P2  For every truncation of that image (0 to its whole size, each in an
//       exactly sized heap block, so an address sanitizer build sees any read
//       past it) and every single-byte corruption of its first 0x4C0 bytes
//       by 0x00, 0x01, 0x7F, 0x80 and 0xFF: every lookup either fails or
//       answers inside the view, and a truncation that succeeds answers as
//       the whole image does.
//
// Out of scope: longer command lines and argument lists (the splitter is
// left to right with one backslash run of lookahead, the reason these
// lengths reach every transition; not itself checked), and PE corruptions of
// two or more bytes (the ESBMC harnesses in proofs/esbmc/platform_rest.cpp
// take every byte nondeterministic, once run).

#include "core/command_line.hpp"
#include "core/pe_image.hpp"
#include "core/stack_line.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using stxlib::Byte;
using stxlib::MutableView;
using stxlib::u16;
using stxlib::u32;
using stxlib::uptr;
using stxlib::usize;
using stxlib::View;

namespace cl = stxlib::command_line;
namespace pe = stxlib::pe_image;
namespace sl = stxlib::stack_line;

std::uint64_t mismatches = 0;
std::uint64_t comparisons = 0;

void expect(const bool condition, const char* const property) {
    comparisons = comparisons + 1;
    if (condition) return;
    mismatches = mismatches + 1;
    if (mismatches <= 10) std::fprintf(stderr, "%s mismatch\n", property);
}

// ---- the command line ------------------------------------------------------------

constexpr char alphabet[5] = {'a', ' ', '\t', '"', '\\'};

bool separator(const char byte) {
    const bool result = byte == ' ' || byte == '\t';
    return result;
}

void append_backslashes(std::string& text, const usize count) {
    text.append(count, '\\');
}

// The reference: the documented rules (command_line.hpp's header) restated
// with a pending-backslash counter instead of a run scan.
std::vector<std::string> reference_split(const std::string& line) {
    std::vector<std::string> arguments;
    const usize count = line.size();
    if (count == 0) return arguments;
    usize index = 0;
    std::string current;
    bool quoted = false;
    while (index < count) {
        const char byte = line[index];
        index = index + 1;
        if (byte == '"') {
            quoted = !quoted;
        } else if (!quoted && separator(byte)) {
            index = index - 1;
            break;
        } else {
            current.push_back(byte);
        }
    }
    arguments.push_back(current);
    for (;;) {
        while (index < count && separator(line[index])) index = index + 1;
        if (index == count) break;
        current.clear();
        quoted = false;
        usize pending = 0;
        while (index < count) {
            const char byte = line[index];
            if (byte == '\\') {
                pending = pending + 1;
                index = index + 1;
                continue;
            }
            if (byte == '"') {
                append_backslashes(current, pending / 2);
                const bool escaped = pending % 2 == 1;
                pending = 0;
                if (escaped) {
                    current.push_back('"');
                    index = index + 1;
                } else if (quoted && index + 1 < count && line[index + 1] == '"') {
                    current.push_back('"');
                    index = index + 2;
                } else {
                    quoted = !quoted;
                    index = index + 1;
                }
                continue;
            }
            append_backslashes(current, pending);
            pending = 0;
            if (!quoted && separator(byte)) break;
            current.push_back(byte);
            index = index + 1;
        }
        append_backslashes(current, pending);
        arguments.push_back(current);
    }
    return arguments;
}

constexpr char guard = '\x5a';
constexpr usize guard_start = static_cast<usize>(0x5a5a5a5a);

// Splits `line` with the core into rooms of the given sizes, guarded past
// their ends; checks the guards; returns the arguments when complete.
bool core_split(const std::string& line, const usize text_room, const usize start_room, cl::SplitCounts& counts,
                std::vector<std::string>& arguments) {
    std::vector<char> text(text_room + 8, guard);
    std::vector<usize> starts(start_room + 4, guard_start);
    const View<char> line_view(line.data(), line.size());
    counts = cl::split_windows_command_line(line_view, MutableView<char>(text.data(), text_room),
                                            MutableView<usize>(starts.data(), start_room));
    bool guards_kept = true;
    for (usize index = text_room; index < text.size(); ++index) guards_kept = guards_kept && text[index] == guard;
    for (usize index = start_room; index < starts.size(); ++index) {
        guards_kept = guards_kept && starts[index] == guard_start;
    }
    expect(guards_kept, "W1 nothing written outside the rooms");
    arguments.clear();
    if (text_room < counts.text_bytes || start_room < counts.argument_count) return false;
    for (usize argument = 0; argument < counts.argument_count; ++argument) {
        const usize start = starts[argument];
        if (start >= counts.text_bytes) return false;
        arguments.push_back(std::string(text.data() + start));
    }
    return true;
}

void check_line(const std::string& line) {
    const std::vector<std::string> expected = reference_split(line);
    cl::SplitCounts measured{};
    std::vector<std::string> unused;
    static_cast<void>(core_split(line, 0, 0, measured, unused));
    expect(measured.argument_count == expected.size(), "W1 argument count");
    expect(measured.text_bytes <= line.size() + measured.argument_count, "W1 text bound");
    expect(measured.argument_count <= line.size() / 2 + 1, "W1 argument bound");

    cl::SplitCounts written{};
    std::vector<std::string> arguments;
    const bool complete = core_split(line, measured.text_bytes, measured.argument_count, written, arguments);
    expect(complete, "W1 complete with measured rooms");
    expect(written.text_bytes == measured.text_bytes && written.argument_count == measured.argument_count,
           "W1 measuring agrees with writing");
    expect(arguments == expected, "W1 arguments equal the reference");

    if (measured.text_bytes > 0) {
        cl::SplitCounts short_text{};
        expect(!core_split(line, measured.text_bytes - 1, measured.argument_count, short_text, arguments),
               "W1 a short text room is incomplete");
    }
    if (measured.argument_count > 0) {
        cl::SplitCounts short_starts{};
        expect(!core_split(line, measured.text_bytes, measured.argument_count - 1, short_starts, arguments),
               "W1 a short start room is incomplete");
    }
}

void every_line() {
    std::string line;
    for (usize length = 0; length <= 7; ++length) {
        usize total = 1;
        for (usize position = 0; position < length; ++position) total = total * 5;
        for (usize number = 0; number < total; ++number) {
            line.clear();
            usize rest = number;
            for (usize position = 0; position < length; ++position) {
                line.push_back(alphabet[rest % 5]);
                rest = rest / 5;
            }
            check_line(line);
        }
    }
}

std::vector<std::string> short_arguments() {
    std::vector<std::string> result;
    result.push_back(std::string());
    for (usize length = 1; length <= 3; ++length) {
        usize total = 1;
        for (usize position = 0; position < length; ++position) total = total * 5;
        for (usize number = 0; number < total; ++number) {
            std::string argument;
            usize rest = number;
            for (usize position = 0; position < length; ++position) {
                argument.push_back(alphabet[rest % 5]);
                rest = rest / 5;
            }
            result.push_back(argument);
        }
    }
    return result;
}

std::string quoted(const std::string& argument) {
    const View<char> view(argument.data(), argument.size());
    const usize needed = cl::quote_windows_argument(view, MutableView<char>());
    expect(needed <= 2 * argument.size() + 2, "W2 quoting bound");
    std::string result(needed, '\0');
    const usize written = cl::quote_windows_argument(view, MutableView<char>(result.data(), result.size()));
    expect(written == needed, "W2 quoting measures as it writes");
    return result;
}

void round_trip(const std::vector<std::string>& list) {
    std::string line = "p";
    for (const std::string& argument : list) {
        line.push_back(' ');
        line.append(quoted(argument));
    }
    std::vector<std::string> expected;
    expected.push_back("p");
    for (const std::string& argument : list) expected.push_back(argument);
    cl::SplitCounts measured{};
    std::vector<std::string> arguments;
    static_cast<void>(core_split(line, 0, 0, measured, arguments));
    cl::SplitCounts written{};
    const bool complete = core_split(line, measured.text_bytes, measured.argument_count, written, arguments);
    expect(complete && arguments == expected, "W2 quoting round trip");
}

void every_list() {
    const std::vector<std::string> pieces = short_arguments();
    std::vector<std::string> list;
    for (const std::string& first : pieces) {
        list.assign(1, first);
        round_trip(list);
        for (const std::string& second : pieces) {
            list.assign(1, first);
            list.push_back(second);
            round_trip(list);
            for (const std::string& third : pieces) {
                list.resize(2);
                list.push_back(third);
                round_trip(list);
            }
        }
    }
}

struct DocumentedCase final {
    const char* line;
    const char* arguments[4];
    usize count;
};

void documented_cases() {
    // Microsoft Learn, "Parsing C++ command-line arguments", the results
    // table (after the program name "p").
    const DocumentedCase cases[] = {
        {"p \"a b c\" d e", {"a b c", "d", "e", nullptr}, 3},
        {"p \"ab\\\"c\" \"\\\\\" d", {"ab\"c", "\\", "d", nullptr}, 3},
        {"p a\\\\\\b d\"e f\"g h", {"a\\\\\\b", "de fg", "h", nullptr}, 3},
        {"p a\\\\\\\"b c d", {"a\\\"b", "c", "d", nullptr}, 3},
        {"p a\\\\\\\\\"b c\" d e", {"a\\\\b c", "d", "e", nullptr}, 3},
        {"p a\"b\"\" c d", {"ab\" c d", nullptr, nullptr, nullptr}, 1},
    };
    for (const DocumentedCase& documented : cases) {
        const std::string line(documented.line);
        cl::SplitCounts measured{};
        std::vector<std::string> arguments;
        static_cast<void>(core_split(line, 0, 0, measured, arguments));
        cl::SplitCounts written{};
        const bool complete = core_split(line, measured.text_bytes, measured.argument_count, written, arguments);
        bool equal = complete && arguments.size() == documented.count + 1 && arguments[0] == "p";
        for (usize index = 0; equal && index < documented.count; ++index) {
            equal = arguments[index + 1] == documented.arguments[index];
        }
        expect(equal, "W3 documented example");
        expect(reference_split(line) == arguments, "W3 the reference agrees");
    }
}

// ---- the stack line ----------------------------------------------------------------

std::string reference_line(const sl::Frame& frame) {
    char number[64];
    std::string line = "  #";
    std::snprintf(number, sizeof(number), "%llu", static_cast<unsigned long long>(frame.index));
    line.append(number);
    std::snprintf(number, sizeof(number), " 0x%016llx ", static_cast<unsigned long long>(frame.address));
    line.append(number);
    if (frame.image_known) {
        for (usize index = 0; index < frame.image.count(); ++index) {
            const unsigned char byte = static_cast<unsigned char>(frame.image.data()[index]);
            line.push_back(byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte));
        }
        std::snprintf(number, sizeof(number), "+0x%llx", static_cast<unsigned long long>(frame.image_offset));
        line.append(number);
    } else {
        line.push_back('?');
    }
    if (!frame.symbol.empty()) {
        line.push_back(' ');
        for (usize index = 0; index < frame.symbol.count(); ++index) {
            const unsigned char byte = static_cast<unsigned char>(frame.symbol.data()[index]);
            line.push_back(byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte));
        }
        std::snprintf(number, sizeof(number), "+0x%llx", static_cast<unsigned long long>(frame.symbol_offset));
        line.append(number);
    }
    line.push_back('\n');
    return line;
}

void check_frame(const sl::Frame& frame) {
    const std::string expected = reference_line(frame);
    for (usize capacity = 0; capacity <= 200; ++capacity) {
        std::vector<char> output(capacity + 8, guard);
        const usize written = sl::format(frame, MutableView<char>(output.data(), capacity));
        bool kept = true;
        for (usize index = capacity; index < output.size(); ++index) kept = kept && output[index] == guard;
        expect(kept, "S1 nothing written past the capacity");
        expect(written <= capacity, "S1 at most the capacity");
        if (capacity == 0) {
            expect(written == 0, "S1 nothing for no room");
            continue;
        }
        expect(written >= 1 && output[written - 1] == '\n', "S1 ends with a newline");
        bool clean = true;
        for (usize index = 0; index + 1 < written; ++index) {
            const unsigned char byte = static_cast<unsigned char>(output[index]);
            clean = clean && byte >= 0x20 && byte != 0x7f;
        }
        expect(clean, "S1 no control byte but the newline");
        const usize body = expected.size() - 1;
        const usize kept_body = body < capacity - 1 ? body : capacity - 1;
        const std::string want = expected.substr(0, kept_body) + "\n";
        expect(std::string(output.data(), written) == want, "S1 the reference line, cut");
    }
}

void every_capacity() {
    const char control_name[] = {'l', 'i', 'b', '\n', 'x', '\x7f', '\x01', '.', 's', 'o'};
    const std::string long_name(300, 'n');
    const char symbol_name[] = {'s', 'y', 'm', '\t', '\xc3', '\xa9'};
    sl::Frame frames[5]{};
    frames[0].index = 0;
    frames[0].address = 0x1234;
    frames[0].image = View<char>(control_name, sizeof(control_name));
    frames[0].image_known = true;
    frames[0].image_offset = 0x34;
    frames[0].symbol = View<char>(symbol_name, sizeof(symbol_name));
    frames[0].symbol_offset = 0;
    frames[1].index = ~static_cast<usize>(0);
    frames[1].address = ~static_cast<uptr>(0);
    frames[1].image_known = false;
    frames[2].index = 7;
    frames[2].address = 0;
    frames[2].image = View<char>(long_name.data(), long_name.size());
    frames[2].image_known = true;
    frames[2].image_offset = ~static_cast<uptr>(0);
    frames[2].symbol = View<char>(long_name.data(), long_name.size());
    frames[2].symbol_offset = 0xabcdef;
    frames[3].index = 12;
    frames[3].address = 0xfeed;
    frames[3].image_known = true; // known, with an empty name
    frames[3].image_offset = 0;
    frames[4].index = 99;
    frames[4].address = 0x10;
    frames[4].image_known = false;
    frames[4].symbol = View<char>(symbol_name, sizeof(symbol_name));
    frames[4].symbol_offset = 0x10;
    for (const sl::Frame& frame : frames) check_frame(frame);
}

// ---- the PE reader ---------------------------------------------------------------------

constexpr usize image_size = 0x1000;

void put16(std::vector<Byte>& image, const usize offset, const u16 value) {
    image[offset] = static_cast<Byte>(value & 0xff);
    image[offset + 1] = static_cast<Byte>(value >> 8);
}

void put32(std::vector<Byte>& image, const usize offset, const u32 value) {
    for (usize index = 0; index < 4; ++index) image[offset + index] = static_cast<Byte>((value >> (8 * index)) & 0xff);
}

void put_text(std::vector<Byte>& image, const usize offset, const char* const text) {
    for (usize index = 0; text[index] != '\0'; ++index) image[offset + index] = static_cast<Byte>(text[index]);
}

// A PE32+ image: NT headers at 0x80, three sections (".text", ".stxtest",
// ".data$a"), an export directory at 0x400 (size 0x100) with four names:
// alpha 0x210, beta 0x250, fwd 0x410 (inside the directory: a forwarder),
// gamma 0x300.
std::vector<Byte> synthetic_image() {
    std::vector<Byte> image(image_size, Byte{0});
    put16(image, 0, 0x5A4D);
    put32(image, 0x3C, 0x80);
    put32(image, 0x80, 0x00004550);
    const usize file_header = 0x84;
    put16(image, file_header, 0x8664);
    put16(image, file_header + 2, 3);
    put16(image, file_header + 16, 0xF0);
    const usize optional = file_header + 20;
    put16(image, optional, 0x20B);
    put32(image, optional + 56, static_cast<u32>(image_size));
    put32(image, optional + 108, 16);
    put32(image, optional + 112, 0x400);
    put32(image, optional + 116, 0x100);
    const usize sections = optional + 0xF0;
    put_text(image, sections, ".text");
    put32(image, sections + 8, 0x200);
    put32(image, sections + 12, 0x200);
    put_text(image, sections + 40, ".stxtest");
    put32(image, sections + 40 + 8, 0x10);
    put32(image, sections + 40 + 12, 0x600);
    put_text(image, sections + 80, ".data$a");
    put32(image, sections + 80 + 8, 0x20);
    put32(image, sections + 80 + 12, 0x700);
    const usize directory = 0x400;
    put32(image, directory + 20, 4);
    put32(image, directory + 24, 4);
    put32(image, directory + 28, 0x440);
    put32(image, directory + 32, 0x460);
    put32(image, directory + 36, 0x480);
    const u32 functions[4] = {0x210, 0x250, 0x410, 0x300};
    const u32 names[4] = {0x4A0, 0x4A8, 0x4B0, 0x4B8};
    for (usize index = 0; index < 4; ++index) {
        put32(image, 0x440 + 4 * index, functions[index]);
        put32(image, 0x460 + 4 * index, names[index]);
        put16(image, 0x480 + 2 * index, static_cast<u16>(index));
    }
    put_text(image, 0x4A0, "alpha");
    put_text(image, 0x4A8, "beta");
    put_text(image, 0x4B0, "fwd");
    put_text(image, 0x4B8, "gamma");
    return image;
}

struct Answers final {
    bool headers{false};
    pe::Headers read{};
    bool text{false};
    pe::Section text_section{};
    bool test{false};
    pe::Section test_section{};
    bool data{false};
    pe::Section data_section{};
    bool exports[4]{};
    pe::Export found[4]{};
};

constexpr u32 export_targets[4] = {0x260, 0x20F, 0x420, 0x300};

View<char> name_view(const char* const name) {
    const View<char> result(name, std::strlen(name));
    return result;
}

bool section_inside(const View<Byte> image, const pe::Section& section) {
    const bool result = section.rva <= image.count() && image.count() - section.rva >= section.size;
    return result;
}

Answers look_up(const View<Byte> image) {
    Answers answers{};
    answers.headers = pe::read_headers(image, answers.read);
    if (!answers.headers) return answers;
    answers.text = pe::find_section(image, answers.read, name_view(".text"), answers.text_section);
    answers.test = pe::find_section(image, answers.read, name_view(".stxtest"), answers.test_section);
    answers.data = pe::find_section(image, answers.read, name_view(".data"), answers.data_section);
    if (answers.text) expect(section_inside(image, answers.text_section), "P2 section inside the view");
    if (answers.test) expect(section_inside(image, answers.test_section), "P2 section inside the view");
    if (answers.data) expect(section_inside(image, answers.data_section), "P2 section inside the view");
    for (usize index = 0; index < 4; ++index) {
        answers.exports[index] = pe::nearest_export(image, answers.read, export_targets[index], answers.found[index]);
        if (!answers.exports[index]) continue;
        const pe::Export& found = answers.found[index];
        expect(found.rva <= export_targets[index], "P2 export at or below the target");
        expect(found.name_rva < image.count() && image.count() - found.name_rva > found.name_count,
               "P2 export name inside the view");
        expect(found.name_count < pe::maximum_export_name, "P2 export name bound");
    }
    return answers;
}

bool same_section(const bool left_found, const pe::Section& left, const bool right_found, const pe::Section& right) {
    if (!left_found) return true;
    const bool result = right_found && left.rva == right.rva && left.size == right.size;
    return result;
}

void synthetic_lookups() {
    const std::vector<Byte> image = synthetic_image();
    const View<Byte> view(image.data(), image.size());
    const Answers answers = look_up(view);
    expect(answers.headers, "P1 headers");
    expect(answers.read.section_count == 3 && answers.read.size_of_image == image_size, "P1 header values");
    expect(answers.read.export_rva == 0x400 && answers.read.export_size == 0x100, "P1 export directory");
    expect(answers.text && answers.text_section.rva == 0x200 && answers.text_section.size == 0x200, "P1 .text");
    expect(answers.test && answers.test_section.rva == 0x600 && answers.test_section.size == 0x10, "P1 .stxtest");
    expect(answers.data && answers.data_section.rva == 0x700 && answers.data_section.size == 0x20,
           "P1 grouped .data$a");
    pe::Section absent{};
    expect(!pe::find_section(view, answers.read, name_view(".tex"), absent), "P1 a prefix is not a match");
    expect(!pe::find_section(view, answers.read, name_view(""), absent), "P1 an empty name");
    expect(!pe::find_section(view, answers.read, name_view(".toolongx"), absent), "P1 a name over 8 bytes");
    expect(answers.exports[0] && answers.found[0].rva == 0x250 && answers.found[0].name_count == 4,
           "P1 beta below 0x260");
    expect(!answers.exports[1], "P1 nothing below the first export");
    expect(answers.exports[2] && answers.found[2].rva == 0x300 && answers.found[2].name_count == 5,
           "P1 gamma, the forwarder skipped");
    expect(answers.exports[3] && answers.found[3].rva == 0x300, "P1 an export at the target");
    if (answers.exports[0]) {
        expect(std::memcmp(image.data() + answers.found[0].name_rva, "beta", 4) == 0, "P1 beta's name");
    }
}

void truncations_and_corruptions() {
    const std::vector<Byte> image = synthetic_image();
    const Answers whole = look_up(View<Byte>(image.data(), image.size()));
    for (usize length = 0; length <= image.size(); ++length) {
        // An exactly sized block, so a sanitizer sees a read past it.
        std::vector<Byte> cut(image.begin(), image.begin() + static_cast<std::ptrdiff_t>(length));
        cut.shrink_to_fit();
        const Answers answers = look_up(View<Byte>(cut.data(), cut.size()));
        expect(same_section(answers.text, answers.text_section, whole.text, whole.text_section) &&
                   same_section(answers.test, answers.test_section, whole.test, whole.test_section) &&
                   same_section(answers.data, answers.data_section, whole.data, whole.data_section),
               "P2 a truncation answers as the whole image");
        for (usize index = 0; index < 4; ++index) {
            if (!answers.exports[index]) continue;
            expect(whole.exports[index] && answers.found[index].rva == whole.found[index].rva &&
                       answers.found[index].name_count == whole.found[index].name_count,
                   "P2 a truncation's export is the whole image's");
        }
    }
    const Byte values[5] = {Byte{0x00}, Byte{0x01}, Byte{0x7F}, Byte{0x80}, Byte{0xFF}};
    std::vector<Byte> damaged = image;
    for (usize offset = 0; offset < 0x4C0; ++offset) {
        for (const Byte value : values) {
            damaged[offset] = value;
            static_cast<void>(look_up(View<Byte>(damaged.data(), damaged.size())));
        }
        damaged[offset] = image[offset];
    }
}

} // namespace

int main() {
    every_line();
    every_list();
    documented_cases();
    every_capacity();
    synthetic_lookups();
    truncations_and_corruptions();
    std::printf("stxlib_platform_rest_exhaustive: %llu comparisons, %llu mismatches\n",
                static_cast<unsigned long long>(comparisons), static_cast<unsigned long long>(mismatches));
    const int status = mismatches == 0 ? 0 : 1;
    return status;
}
