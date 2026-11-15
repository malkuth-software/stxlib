// Tests of include/stxlib/markdown.hpp.
//
// Usage: stxlib_markdown_tests <spec.txt> <UnicodeData.txt> <CaseFolding.txt> [--list]
// (CMake passes third_party/commonmark-0.31.2/spec.txt and the two files
// of third_party/unicode-18.0.0). --list prints every specification
// example whose HTML differs, with both texts.
//
// Properties:
//   S1  every example of the CommonMark 0.31.2 specification, read through
//       the HTML writer kept here, gives the specification's HTML; except
//       the examples of the sections "HTML blocks" and "Raw HTML" and the
//       examples listed in html_examples, whose expected HTML passes raw
//       HTML through, which this reader reads as text by design;
//   S2  for every example (all 652), and for every prefix of every example,
//       the tape is well formed: it begins and ends with the document
//       block, blocks and spans nest, every range lies in the source,
//       text ranges do not go backwards, and attribute and reference
//       indices are valid;
//   C1  the byte classes are their definitions, for all 256 bytes;
//   C2  decode_at agrees with a reference decoder on every scalar value's
//       UTF-8 form, and refuses every other sequence of up to four bytes
//       drawn from the boundary bytes; decode_before finds what decode_at
//       wrote;
//   C3  scan_to_newline agrees with a byte loop at every offset of buffers
//       that hold line endings at every position mod 8;
//   U1  is_unicode_whitespace and is_unicode_punctuation agree with
//       UnicodeData.txt, read here by a parser of its own, at every code
//       point 0 to 0x10FFFF;
//   U2  fold_case_full agrees with CaseFolding.txt (status C and F) at
//       every code point;
//   E1  known character references decode; names that are not references
//       stay text;
//   L1  limits and the stop signal: each named failure, and an empty
//       document after it;
//   D1  the places this reader differs from MD4C on purpose behave as
//       CommonMark says (lines.cpp, inlines.cpp).
// C1, C3 (over its buffers), U1 and U2 are exhaustive over their domains.

#include "stxlib/markdown.hpp"

#include "algorithms/markdown/characters.hpp"
#include "algorithms/markdown/entities.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace markdown = stxlib::markdown;
namespace detail = stxlib::markdown::detail;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

// ---- reading a source into a tape ------------------------------------------------------

[[nodiscard]] bool read(const std::string& source, const markdown::Options& options, markdown::Document& document) {
    const markdown::Limits limits{};
    const stxlib::View<char> view(source.data(), source.size());
    const stxlib::Status status = markdown::parse(view, options, limits, document, nullptr);
    const bool succeeded = status.succeeded();
    return succeeded;
}

// ---- the HTML writer (for S1 only; the library writes no HTML) -----------------------

void append_escaped(std::string& out, const char* const text, const usize count) {
    for (usize index = 0; index < count; ++index) {
        const char ch = text[index];
        if (ch == '&') {
            out += "&amp;";
        } else if (ch == '<') {
            out += "&lt;";
        } else if (ch == '>') {
            out += "&gt;";
        } else if (ch == '"') {
            out += "&quot;";
        } else {
            out += ch;
        }
    }
}

// A destination as cmark writes it: bytes outside a safe set as %XX.
void append_url(std::string& out, const stxlib::View<char> url) {
    static const char safe[] = "~-_.+!*'(),%#@?=;:/$";
    static const char hex[] = "0123456789ABCDEF";
    for (usize index = 0; index < url.count(); ++index) {
        const char ch = url[index];
        const bool alphanumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (alphanumeric || (ch != '\0' && std::strchr(safe, ch) != nullptr)) {
            out += ch;
        } else if (ch == '&') {
            out += "&amp;";
        } else {
            const unsigned char byte = static_cast<unsigned char>(ch);
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 0x0f];
        }
    }
}

void line_start(std::string& out) {
    if (!out.empty() && out.back() != '\n') out += '\n';
}

void append_text_event(std::string& out, const std::string& source, const markdown::Document& document,
                       const markdown::Event& event) {
    const markdown::TextType type = static_cast<markdown::TextType>(event.type);
    if (type == markdown::TextType::literal || type == markdown::TextType::code) {
        append_escaped(out, source.data() + event.begin, event.end - event.begin);
    } else if (type == markdown::TextType::reference) {
        const stxlib::View<char> decoded = document.reference_text(event);
        append_escaped(out, decoded.data(), decoded.count());
    } else if (type == markdown::TextType::null_character) {
        out += "\xEF\xBF\xBD";
    } else if (type == markdown::TextType::soft_break || type == markdown::TextType::line_end) {
        out += '\n';
    } else if (type == markdown::TextType::hard_break) {
        out += "<br />\n";
    } else if (type == markdown::TextType::code_space) {
        out += ' ';
    } else if (type == markdown::TextType::code_indent) {
        out.append(event.detail, ' ');
    }
}

[[nodiscard]] std::string to_html(const std::string& source, const markdown::Document& document) {
    std::string out{};
    // Inside an image only the text counts, as its alt attribute.
    usize image_depth = 0;
    const usize count = document.events.count();
    for (usize index = 0; index < count; ++index) {
        const markdown::Event& event = document.events[index];
        if (event.kind == markdown::EventKind::text) {
            append_text_event(out, source, document, event);
            continue;
        }
        if (event.kind == markdown::EventKind::enter_span || event.kind == markdown::EventKind::leave_span) {
            const bool enter = event.kind == markdown::EventKind::enter_span;
            const markdown::SpanType type = static_cast<markdown::SpanType>(event.type);
            if (type == markdown::SpanType::image) {
                if (enter) {
                    if (image_depth == 0) {
                        out += "<img src=\"";
                        append_url(out, document.attribute(event.detail));
                        out += "\" alt=\"";
                    }
                    image_depth = image_depth + 1;
                } else {
                    image_depth = image_depth - 1;
                    if (image_depth == 0) {
                        out += '"';
                        const stxlib::View<char> title = document.attribute(event.detail + 1);
                        if (!title.empty()) {
                            out += " title=\"";
                            append_escaped(out, title.data(), title.count());
                            out += '"';
                        }
                        out += " />";
                    }
                }
                continue;
            }
            if (image_depth != 0) continue;
            if (type == markdown::SpanType::emphasis) {
                out += enter ? "<em>" : "</em>";
            } else if (type == markdown::SpanType::strong) {
                out += enter ? "<strong>" : "</strong>";
            } else if (type == markdown::SpanType::code) {
                out += enter ? "<code>" : "</code>";
            } else if (type == markdown::SpanType::strikethrough) {
                out += enter ? "<del>" : "</del>";
            } else if (type == markdown::SpanType::link) {
                if (enter) {
                    out += "<a href=\"";
                    if ((event.flags & markdown::event_flags::mail_address) != 0) out += "mailto:";
                    append_url(out, document.attribute(event.detail));
                    out += '"';
                    const stxlib::View<char> title = document.attribute(event.detail + 1);
                    if (!title.empty()) {
                        out += " title=\"";
                        append_escaped(out, title.data(), title.count());
                        out += '"';
                    }
                    out += '>';
                } else {
                    out += "</a>";
                }
            }
            continue;
        }
        const bool enter = event.kind == markdown::EventKind::enter_block;
        const markdown::BlockType type = static_cast<markdown::BlockType>(event.type);
        if (type == markdown::BlockType::document) continue;
        if (type == markdown::BlockType::quote) {
            line_start(out);
            out += enter ? "<blockquote>\n" : "</blockquote>\n";
        } else if (type == markdown::BlockType::bullet_list) {
            if (enter) line_start(out);
            out += enter ? "<ul>\n" : "</ul>\n";
        } else if (type == markdown::BlockType::ordered_list) {
            if (enter) {
                line_start(out);
                if (event.detail == 1) {
                    out += "<ol>\n";
                } else {
                    out += "<ol start=\"" + std::to_string(event.detail) + "\">\n";
                }
            } else {
                out += "</ol>\n";
            }
        } else if (type == markdown::BlockType::list_item) {
            if (enter) {
                line_start(out);
                out += "<li>";
            } else {
                out += "</li>\n";
            }
        } else if (type == markdown::BlockType::rule) {
            if (enter) {
                line_start(out);
                out += "<hr />\n";
            }
        } else if (type == markdown::BlockType::heading) {
            if (enter) line_start(out);
            out += enter ? "<h" : "</h";
            out += static_cast<char>('0' + event.data);
            out += enter ? ">" : ">\n";
        } else if (type == markdown::BlockType::code_block) {
            if (enter) {
                line_start(out);
                out += "<pre><code";
                if (event.detail != 0xffffffff) {
                    const stxlib::View<char> language = document.attribute(event.detail + 1);
                    if (!language.empty()) {
                        out += " class=\"language-";
                        append_escaped(out, language.data(), language.count());
                        out += '"';
                    }
                }
                out += '>';
            } else {
                out += "</code></pre>\n";
            }
        } else if (type == markdown::BlockType::paragraph) {
            if (enter) {
                line_start(out);
                out += "<p>";
            } else {
                out += "</p>\n";
            }
        }
    }
    return out;
}

// ---- the specification -----------------------------------------------------------------

struct Example final {
    int number{0};
    std::string section{};
    std::string markdown{};
    std::string html{};
};

// The specification writes a tab as U+2192.
void restore_tabs(std::string& text) {
    static const char arrow[] = "\xE2\x86\x92";
    usize at = text.find(arrow);
    while (at != std::string::npos) {
        text.replace(at, 3, "\t");
        at = text.find(arrow, at + 1);
    }
}

[[nodiscard]] bool load_examples(const char* const path, std::vector<Example>& examples) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const std::string fence(32, '`');
    std::string section{};
    std::string line{};
    int state = 0; // 0 prose, 1 the example's Markdown, 2 its HTML
    Example current{};
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (state == 0) {
            if (line == fence + " example") {
                state = 1;
                current = Example{};
                current.number = static_cast<int>(examples.size()) + 1;
                current.section = section;
            } else if (line.rfind("#", 0) == 0) {
                const usize text = line.find_first_not_of("# ");
                section = text == std::string::npos ? std::string{} : line.substr(text);
            }
        } else if (state == 1) {
            if (line == ".") {
                state = 2;
            } else {
                current.markdown += line + "\n";
            }
        } else {
            if (line == fence) {
                restore_tabs(current.markdown);
                restore_tabs(current.html);
                examples.push_back(current);
                state = 0;
            } else {
                current.html += line + "\n";
            }
        }
    }
    return true;
}

// Examples outside the two HTML sections whose expected HTML passes raw
// HTML through. Each was read: the difference is that reading and nothing
// else.
const int html_examples[] = {21, 31, 201, 308, 309, 344, 475, 476, 477, 491, 494, 524, 536, 642, 643};

[[nodiscard]] bool reads_html(const Example& example) {
    if (example.section == "HTML blocks" || example.section == "Raw HTML") return true;
    for (const int number : html_examples) {
        if (number == example.number) return true;
    }
    return false;
}

// ---- S2: the tape's form ---------------------------------------------------------------

[[nodiscard]] bool tape_well_formed(const std::string& source, const markdown::Document& document) {
    const usize count = document.events.count();
    if (count < 2) return false;
    const markdown::Event& first = document.events[0];
    const markdown::Event& last = document.events[count - 1];
    if (first.kind != markdown::EventKind::enter_block || first.type != static_cast<u8>(markdown::BlockType::document)) {
        return false;
    }
    if (last.kind != markdown::EventKind::leave_block || last.type != static_cast<u8>(markdown::BlockType::document)) {
        return false;
    }
    // Open blocks and spans: kind and type, innermost last.
    std::vector<u32> open{};
    u32 text_end = 0;
    for (usize index = 0; index < count; ++index) {
        const markdown::Event& event = document.events[index];
        if (event.begin > event.end || event.end > source.size()) return false;
        const u32 key = (event.kind == markdown::EventKind::enter_block || event.kind == markdown::EventKind::leave_block
                             ? 0x100U
                             : 0x200U) |
                        event.type;
        if (event.kind == markdown::EventKind::enter_block || event.kind == markdown::EventKind::enter_span) {
            // A block does not open inside a span.
            if (event.kind == markdown::EventKind::enter_block && !open.empty() && (open.back() & 0x200U) != 0) return false;
            open.push_back(key);
        } else if (event.kind == markdown::EventKind::leave_block || event.kind == markdown::EventKind::leave_span) {
            if (open.empty() || open.back() != key) return false;
            open.pop_back();
        } else {
            if (open.empty()) return false;
            // Text follows the source in order.
            if (event.begin < text_end) return false;
            text_end = event.end;
            if (event.type == static_cast<u8>(markdown::TextType::reference)) {
                if (event.data == 0 || event.detail + event.data > document.text.count()) return false;
            }
        }
        const bool is_link = (event.kind == markdown::EventKind::enter_span || event.kind == markdown::EventKind::leave_span) &&
                             (event.type == static_cast<u8>(markdown::SpanType::link) ||
                              event.type == static_cast<u8>(markdown::SpanType::image));
        if (is_link && event.detail + 1 >= document.attributes.count()) return false;
    }
    const bool closed = open.empty();
    return closed;
}

void specification(const char* const path, const bool list) {
    std::vector<Example> examples{};
    CHECK(load_examples(path, examples));
    CHECK(examples.size() == 652);
    const markdown::Options options{};
    int matched = 0;
    int differing = 0;
    int exempt = 0;
    int malformed = 0;
    markdown::Document document{};
    for (const Example& example : examples) {
        const bool parsed = read(example.markdown, options, document);
        if (!parsed || !tape_well_formed(example.markdown, document)) {
            malformed = malformed + 1;
            std::fprintf(stderr, "example %d: the tape is not well formed\n", example.number);
            continue;
        }
        const std::string html = to_html(example.markdown, document);
        if (html == example.html) {
            matched = matched + 1;
        } else if (reads_html(example)) {
            exempt = exempt + 1;
        } else {
            differing = differing + 1;
            std::fprintf(stderr, "example %d (%s) differs\n", example.number, example.section.c_str());
            if (list) {
                std::fprintf(stderr, "--- markdown\n%s--- expected\n%s--- got\n%s---\n", example.markdown.c_str(),
                             example.html.c_str(), html.c_str());
            }
        }
        // S2 on every prefix: a source cut anywhere is still a source.
        for (usize length = 0; length < example.markdown.size(); ++length) {
            const std::string prefix = example.markdown.substr(0, length);
            if (!read(prefix, options, document) || !tape_well_formed(prefix, document)) {
                malformed = malformed + 1;
                std::fprintf(stderr, "example %d cut at %zu: the tape is not well formed\n", example.number, length);
                break;
            }
        }
    }
    std::printf("specification: %d of %zu examples match, %d read HTML as text, %d differ, %d malformed\n", matched,
                examples.size(), exempt, differing, malformed);
    CHECK(differing == 0);
    CHECK(malformed == 0);
}

// ---- C1 to C3 --------------------------------------------------------------------------

void byte_classes() {
    bool agree = true;
    for (int value = 0; value < 256; ++value) {
        const char byte = static_cast<char>(value);
        if (detail::is_blank(byte) != (value == ' ' || value == '\t')) agree = false;
        if (detail::is_newline(byte) != (value == '\n' || value == '\r')) agree = false;
        if (detail::is_whitespace(byte) != (value == ' ' || value == '\t' || value == '\v' || value == '\f')) agree = false;
        const bool punctuation = value < 128 && std::strchr("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", value) != nullptr && value != 0;
        if (detail::is_ascii_punctuation(byte) != punctuation) agree = false;
        if (detail::is_digit(byte) != (value >= '0' && value <= '9')) agree = false;
        const bool alpha = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
        if (detail::is_alpha(byte) != alpha) agree = false;
        const bool hex = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
        if (detail::is_hex_digit(byte) != hex) agree = false;
        if (detail::is_control(byte) != (value <= 31 || value == 127)) agree = false;
    }
    CHECK(agree);
}

void utf8_decoding() {
    // Every scalar value: encode, decode forward, decode backward.
    bool forward = true;
    bool backward = true;
    for (u32 code_point = 0; code_point <= 0x10ffff; ++code_point) {
        if (code_point >= 0xd800 && code_point <= 0xdfff) continue;
        char bytes[6] = {'x', 0, 0, 0, 0, 'y'};
        const u32 length = detail::encode_utf8(code_point, bytes + 1);
        const detail::Decoded at = detail::decode_at(bytes, 1 + length, 1);
        if (at.code_point != code_point || at.length != length) forward = false;
        const detail::Decoded before = detail::decode_before(bytes, 0, 1 + length);
        if (before.code_point != code_point || before.length != length) backward = false;
        // One byte short is not that character.
        if (length > 1) {
            const detail::Decoded cut = detail::decode_at(bytes, length, 1);
            if (cut.code_point != detail::not_a_character || cut.length != 1) forward = false;
        }
    }
    CHECK(forward);
    CHECK(backward);
    // Ill-formed leads and seconds, from the boundary bytes.
    const unsigned char boundary[] = {0x00, 0x7f, 0x80, 0x8f, 0x90, 0x9f, 0xa0, 0xbf, 0xc0, 0xc1, 0xc2, 0xdf,
                                      0xe0, 0xe1, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1, 0xf3, 0xf4, 0xf5, 0xff};
    const usize boundary_count = sizeof(boundary);
    bool strict = true;
    for (usize a = 0; a < boundary_count; ++a) {
        for (usize b = 0; b < boundary_count; ++b) {
            for (usize c = 0; c < boundary_count; ++c) {
                for (usize d = 0; d < boundary_count; ++d) {
                    const char bytes[4] = {static_cast<char>(boundary[a]), static_cast<char>(boundary[b]),
                                           static_cast<char>(boundary[c]), static_cast<char>(boundary[d])};
                    const detail::Decoded decoded = detail::decode_at(bytes, 4, 0);
                    if (decoded.code_point == detail::not_a_character) continue;
                    // What it accepted must encode back to the same bytes.
                    char again[4] = {0, 0, 0, 0};
                    const u32 length = detail::encode_utf8(decoded.code_point, again);
                    if (length != decoded.length || std::memcmp(again, bytes, length) != 0) strict = false;
                    if (decoded.code_point > 0x10ffff || (decoded.code_point >= 0xd800 && decoded.code_point <= 0xdfff)) {
                        strict = false;
                    }
                }
            }
        }
    }
    CHECK(strict);
}

void newline_scan() {
    bool agree = true;
    char buffer[64];
    // A line ending at each position, alone and with a second one, in
    // text of every length up to 40.
    for (usize size = 0; size <= 40; ++size) {
        for (usize first = 0; first <= size; ++first) {
            for (usize second = first; second <= size; ++second) {
                for (usize index = 0; index < size; ++index) buffer[index] = static_cast<char>('a' + index % 26);
                if (first < size) buffer[first] = '\n';
                if (second < size) buffer[second] = '\r';
                for (usize offset = 0; offset <= size; ++offset) {
                    usize expected = offset;
                    while (expected < size && buffer[expected] != '\n' && buffer[expected] != '\r') expected = expected + 1;
                    const u32 found = detail::scan_to_newline(buffer, static_cast<u32>(size), static_cast<u32>(offset));
                    if (found != expected) agree = false;
                }
            }
        }
    }
    // Bytes that differ from a line ending in one bit must not be taken.
    const char close_bytes[] = {0x0b, 0x0c, 0x0e, 0x08, 0x02, 0x1a, 0x2a, 0x4a, static_cast<char>(0x8a),
                                static_cast<char>(0x8d), 0x1d, 0x2d};
    for (const char byte : close_bytes) {
        for (usize index = 0; index < 16; ++index) buffer[index] = byte;
        if (detail::scan_to_newline(buffer, 16, 0) != 16) agree = false;
    }
    CHECK(agree);
}

// ---- U1, U2: against the Unicode files -------------------------------------------------

[[nodiscard]] std::vector<std::string> split(const std::string& line, const char separator) {
    std::vector<std::string> fields{};
    std::string field{};
    std::istringstream stream(line);
    while (std::getline(stream, field, separator)) fields.push_back(field);
    return fields;
}

void unicode_classes(const char* const path) {
    // The general category's first letter, and whether it is Zs, per code point.
    std::vector<char> major(0x110000, 'C');
    std::vector<bool> space_separator(0x110000, false);
    std::ifstream file(path);
    CHECK(static_cast<bool>(file));
    std::string line{};
    long range_first = -1;
    while (std::getline(file, line)) {
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 3) continue;
        const long code_point = std::strtol(fields[0].c_str(), nullptr, 16);
        long first = code_point;
        if (fields[1].size() > 8 && fields[1].compare(fields[1].size() - 8, 8, ", First>") == 0) {
            range_first = code_point;
            continue;
        }
        if (fields[1].size() > 7 && fields[1].compare(fields[1].size() - 7, 7, ", Last>") == 0) first = range_first;
        for (long inside = first; inside <= code_point; ++inside) {
            major[static_cast<usize>(inside)] = fields[2][0];
            space_separator[static_cast<usize>(inside)] = fields[2] == "Zs";
        }
    }
    bool whitespace = true;
    bool punctuation = true;
    for (u32 code_point = 0; code_point <= 0x10ffff; ++code_point) {
        const bool expected_space = space_separator[code_point] || code_point == '\t' || code_point == '\n' ||
                                    code_point == '\f' || code_point == '\r';
        if (detail::is_unicode_whitespace(code_point) != expected_space) whitespace = false;
        const bool expected_punctuation = major[code_point] == 'P' || major[code_point] == 'S';
        if (detail::is_unicode_punctuation(code_point) != expected_punctuation) punctuation = false;
    }
    CHECK(whitespace);
    CHECK(punctuation);
    CHECK(!detail::is_unicode_whitespace(detail::not_a_character));
    CHECK(!detail::is_unicode_punctuation(detail::not_a_character));
}

void full_folding(const char* const path) {
    std::vector<std::vector<u32>> expected(0x110000);
    std::ifstream file(path);
    CHECK(static_cast<bool>(file));
    std::string line{};
    while (std::getline(file, line)) {
        const usize comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const std::vector<std::string> fields = split(line, ';');
        if (fields.size() < 3) continue;
        const std::string status = fields[1].substr(fields[1].find_first_not_of(' '));
        if (status != "C" && status != "F") continue;
        const u32 code_point = static_cast<u32>(std::strtoul(fields[0].c_str(), nullptr, 16));
        std::istringstream mapped(fields[2]);
        std::string piece{};
        while (mapped >> piece) expected[code_point].push_back(static_cast<u32>(std::strtoul(piece.c_str(), nullptr, 16)));
    }
    bool agree = true;
    for (u32 code_point = 0; code_point <= 0x10ffff; ++code_point) {
        u32 folded[3] = {0, 0, 0};
        const u32 count = detail::fold_case_full(code_point, folded);
        if (expected[code_point].empty()) {
            if (count != 1 || folded[0] != code_point) agree = false;
        } else {
            if (count != expected[code_point].size()) agree = false;
            for (u32 index = 0; index < count && index < expected[code_point].size(); ++index) {
                if (folded[index] != expected[code_point][index]) agree = false;
            }
        }
    }
    CHECK(agree);
}

// ---- E1, L1, D1 ------------------------------------------------------------------------

[[nodiscard]] std::string html_of(const std::string& source, const bool strikethrough) {
    markdown::Document document{};
    const markdown::Options options{.strikethrough = strikethrough};
    if (!read(source, options, document) || !tape_well_formed(source, document)) return "(failed)";
    const std::string html = to_html(source, document);
    return html;
}

void references_and_marks() {
    u32 first = 0;
    u32 second = 0;
    CHECK(detail::find_named_reference("amp", 3, first, second) && first == '&' && second == 0);
    CHECK(detail::find_named_reference("CounterClockwiseContourIntegral", 31, first, second) && first == 0x2233);
    CHECK(detail::find_named_reference("NotEqualTilde", 13, first, second) && first == 0x2242 && second == 0x0338);
    CHECK(!detail::find_named_reference("am", 2, first, second));
    CHECK(!detail::find_named_reference("ampx", 4, first, second));
    CHECK(!detail::find_named_reference("AMp", 3, first, second));
    CHECK(html_of("&amp; &#35; &#x23; &nosuch; &#0; &#xD800;\n", false) ==
          "<p>&amp; # # &amp;nosuch; \xEF\xBF\xBD \xEF\xBF\xBD</p>\n");
    CHECK(html_of("~~gone~~ and ~one~ and ~~~three~~~\n", true) == "<p><del>gone</del> and <del>one</del> and ~~~three~~~</p>\n");
    CHECK(html_of("~~kept~~\n", false) == "<p>~~kept~~</p>\n");
    CHECK(html_of("<b>bold</b>\n\n<div>\nx\n</div>\n", false) == "<p>&lt;b&gt;bold&lt;/b&gt;</p>\n<p>&lt;div&gt;\nx\n&lt;/div&gt;</p>\n");
    CHECK(html_of("<me@example.org> <https://example.org/a?b=c>\n", false) ==
          "<p><a href=\"mailto:me@example.org\">me@example.org</a> <a href=\"https://example.org/a?b=c\">https://example.org/a?b=c</a></p>\n");
    // A definition whose title runs over lines, used after its lines are gone.
    CHECK(html_of("[foo]: /url '\nthe title'\n\n[foo]\n", false) == "<p><a href=\"/url\" title=\"\nthe title\">foo</a></p>\n");
    // Many links to one definition share its decoded text.
    markdown::Document document{};
    const markdown::Options options{};
    CHECK(read("[a] [a] [a] [a]\n\n[a]: /x&amp;y \"t\"\n", options, document));
    CHECK(document.attributes.count() == 2);
}

void limits_and_stopping() {
    const markdown::Options options{};
    markdown::Document document{};
    const std::string source = "# Heading\n\n> - item *one*\n> - item [two](/x)\n\ntext\n";
    const stxlib::View<char> view(source.data(), source.size());
    markdown::Limits small_source{};
    small_source.source_bytes = 10;
    stxlib::Status status = markdown::parse(view, options, small_source, document, nullptr);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && document.events.empty());
    markdown::Limits few_events{};
    few_events.events = 8;
    status = markdown::parse(view, options, few_events, document, nullptr);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && document.events.empty());
    markdown::Limits no_text{};
    no_text.text_bytes = 1;
    status = markdown::parse(view, options, no_text, document, nullptr);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && document.events.empty());
    markdown::Limits shallow{};
    shallow.containers = 1;
    status = markdown::parse(view, options, shallow, document, nullptr);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && document.events.empty());
    const std::string defined = "[a]: /1\n[b]: /2\n\n[a] [b]\n";
    const stxlib::View<char> defined_view(defined.data(), defined.size());
    markdown::Limits one_definition{};
    one_definition.definitions = 1;
    status = markdown::parse(defined_view, options, one_definition, document, nullptr);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::limit_exceeded && document.events.empty());
    stxlib::StopSignal stop{};
    stop.request();
    const markdown::Limits limits{};
    status = markdown::parse(view, options, limits, document, &stop);
    CHECK(status.failed() && (*status.error()).kind() == stxlib::ErrorKind::interrupted && document.events.empty());
    // And with nothing in the way, the same source reads; the empty source too.
    status = markdown::parse(view, options, limits, document, nullptr);
    CHECK(status.succeeded() && tape_well_formed(source, document));
    const stxlib::View<char> nothing{};
    status = markdown::parse(nothing, options, limits, document, nullptr);
    CHECK(status.succeeded() && document.events.count() == 2);
}

void differences_from_md4c() {
    // A fence line after the container of an unclosed fenced block has
    // ended opens a new code block.
    CHECK(html_of("- ```\n  a\n```\nb\n", false) ==
          "<ul>\n<li>\n<pre><code>a\n</code></pre>\n</li>\n</ul>\n<pre><code>b\n</code></pre>\n");
    // An emphasis mark inside a reference link's label closes nothing.
    CHECK(html_of("*a [b][*] c*\n\n[*]: /u\n", false) == "<p><em>a <a href=\"/u\">b</a> c</em></p>\n");
    // An item that begins with a blank line ends at a second one, whatever
    // the offsets of the lines before it.
    for (usize padding = 0; padding < 300; ++padding) {
        const std::string before(padding, 'x');
        const std::string source = before + "\n\n- a\n\n\n  b\n";
        const std::string expected = (padding == 0 ? std::string{} : "<p>" + before + "</p>\n") +
                                     "<ul>\n<li>\n<p>a</p>\n<p>b</p>\n</li>\n</ul>\n";
        if (html_of(source, false) != expected) {
            CHECK(html_of(source, false) == expected);
            break;
        }
    }
    CHECK(html_of("-\n\n  foo\n", false) == "<ul>\n<li></li>\n</ul>\n<p>foo</p>\n");
    // The exact input the comparison with MD4C found: the paragraph's line
    // begins at offset 4, which MD4C reads as "the last block is a list item".
    CHECK(html_of("-\n  f\n\n\n  f\n", false) == "<ul>\n<li>\n<p>f</p>\n<p>f</p>\n</li>\n</ul>\n");
    // A code span whose closing backticks start a line, with a line after
    // it: one line ending, not two.
    CHECK(html_of("``\nfo\n``\no\n", false) == "<p><code>fo</code>\no</p>\n");
    CHECK(html_of("> ```\n```\n", false) == "<blockquote>\n<pre><code></code></pre>\n</blockquote>\n<pre><code></code></pre>\n");
}

// Inputs built to make a careless reader slow or deep: each must read, and
// read into a well-formed tape.
void hostile_shapes() {
    const markdown::Options options{.strikethrough = true};
    markdown::Document document{};
    const usize size = 20000;
    std::string source{};
    for (usize index = 0; index < size; ++index) source += "*a _b ~c ";
    CHECK(read(source, options, document) && tape_well_formed(source, document));
    source.assign(size, '[');
    CHECK(read(source, options, document) && tape_well_formed(source, document));
    source.clear();
    for (usize index = 0; index < size; ++index) source += "[a](";
    CHECK(read(source, options, document) && tape_well_formed(source, document));
    source.clear();
    for (usize index = 1; index < 2000; ++index) source += std::string(index % 40 + 1, '`') + "x ";
    CHECK(read(source, options, document) && tape_well_formed(source, document));
    source.assign(size, '*');
    source += "a";
    source.append(size, '*');
    CHECK(read(source, options, document) && tape_well_formed(source, document));
    // Deeper than the limit: refused, not read.
    source.assign(1000, '>');
    source += " x\n";
    CHECK(!read(source, options, document) && document.events.empty());
    source.assign(200, '>');
    source += " x\n";
    CHECK(read(source, options, document) && tape_well_formed(source, document));
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count < 4) {
        std::fprintf(stderr, "usage: stxlib_markdown_tests <spec.txt> <UnicodeData.txt> <CaseFolding.txt> [--list]\n");
        return 2;
    }
    const bool list = argument_count > 4 && std::strcmp(arguments[4], "--list") == 0;
    byte_classes();
    utf8_decoding();
    newline_scan();
    unicode_classes(arguments[2]);
    full_folding(arguments[3]);
    references_and_marks();
    limits_and_stopping();
    differences_from_md4c();
    hostile_shapes();
    specification(arguments[1], list);
    const int status = stxlib_tests::finish("stxlib_markdown_tests");
    return status;
}
