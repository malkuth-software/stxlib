// Proof harness for Path's lossless host names (include/stxlib/path.hpp;
// owner decision D13 a): exhaustive enumeration of whole input domains
// against an independent reference (FORWARD_PROGRAMME section 10).
//
// Properties:
//   N1  For every POSIX name s of 1 to 3 bytes over the bytes 1..255 except
//       '/' (16,451,834 names): from_posix_name(s) succeeds, its text is the
//       reference escape of s, and posix_name gives s back followed by NUL.
//   N2  For every text t of one or two tokens from T (below; 392 tokens,
//       153,664 pairs): posix_name(parse_wtf8(t)) succeeds
//       exactly when the reference says t is the escape of some bytes, and
//       then from_posix_name of those bytes gives t back. With N1 this is
//       injectivity on the domain: no two Paths name one POSIX name.
//   N3  For every single 16-bit unit u except 0, '/', '\' and ':' (65,532
//       units), and every pair of units from W (below; 4,227,136 pairs):
//       from_windows_name succeeds, its text is the reference WTF-8 of the
//       units, and windows_name gives the units back followed by NUL.
//   N4  For the same pairs of units, parse_wtf8 of the reference WTF-8 gives
//       the same Path (the stored text reads back).
//
// T: every byte 1..127 except '/', ':' and '\' (which Path's syntax gives
// other meanings) as a character, every escape U+DC00..
// U+DCFF, the other surrogate ends U+D800, U+DBFF, U+DD00, U+DFFF, and the
// scalar boundaries U+0080, U+07FF, U+0800, U+FFFD, U+FFFF, U+10000,
// U+10FFFF and U+00E9. W: every surrogate U+D800..U+DFFF and 'a', '.',
// U+007F, U+0080, U+07FF, U+0800, U+FFFD, U+FFFF.
//
// Out of scope: POSIX names longer than 3 bytes, texts of three or more
// tokens, and Windows sequences of three or more units. The conversions are
// left-to-right with at most one unit or one sequence of lookahead, which is
// why these lengths exercise every transition, but that argument is not
// itself checked here: longer inputs are tested, not proved.
//
// The reference decodes UTF-8 by the Unicode Standard's section 3.9 (D92)
// arithmetic, as tests/utf8_exhaustive.cpp does, and encodes WTF-8 by the
// WTF-8 text's definition, independently of src/core/wtf8.hpp.

#include "stxlib/path.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using stxlib::Path;
using stxlib::Result;
using stxlib::u16;
using stxlib::usize;

std::uint64_t mismatches = 0;
std::uint64_t comparisons = 0;

void expect(const bool condition, const char* const property) {
    comparisons = comparisons + 1;
    if (condition) return;
    mismatches = mismatches + 1;
    if (mismatches <= 10) std::fprintf(stderr, "%s mismatch\n", property);
}

// The length of a well-formed UTF-8 sequence at index (D92), or 0.
usize reference_sequence(const unsigned char* const bytes, const usize count, const usize index) {
    const unsigned first = bytes[index];
    usize length = 0;
    std::uint32_t scalar = 0;
    if ((first & 0x80u) == 0x00u) {
        length = 1;
        scalar = first;
    } else if ((first & 0xe0u) == 0xc0u) {
        length = 2;
        scalar = first & 0x1fu;
    } else if ((first & 0xf0u) == 0xe0u) {
        length = 3;
        scalar = first & 0x0fu;
    } else if ((first & 0xf8u) == 0xf0u) {
        length = 4;
        scalar = first & 0x07u;
    } else {
        return 0;
    }
    if (length > count - index) return 0;
    for (usize offset = 1; offset < length; ++offset) {
        const unsigned next = bytes[index + offset];
        if ((next & 0xc0u) != 0x80u) return 0;
        scalar = (scalar << 6) | (next & 0x3fu);
    }
    const std::uint32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (scalar < minimum[length]) return 0;
    if (scalar > 0x10ffffu) return 0;
    if (scalar >= 0xd800u && scalar <= 0xdfffu) return 0;
    return length;
}

// The generalized UTF-8 encoding of a code point (surrogates included).
void encode(const std::uint32_t code_point, std::string& output) {
    if (code_point < 0x80u) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800u) {
        output.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else if (code_point < 0x10000u) {
        output.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
        output.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
        output.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
    }
}

// The reference POSIX escape: greedy well-formed sequences are copied, except
// ':' and '\'; every other byte becomes U+DC00 + byte.
std::string reference_escape(const std::string& bytes) {
    std::string result;
    const unsigned char* const data = reinterpret_cast<const unsigned char*>(bytes.data());
    usize index = 0;
    while (index < bytes.size()) {
        const usize length = reference_sequence(data, bytes.size(), index);
        if (length == 0 || data[index] == ':' || data[index] == '\\') {
            encode(0xdc00u + data[index], result);
            index = index + 1;
            continue;
        }
        result.append(bytes, index, length);
        index = index + length;
    }
    return result;
}

// The reference WTF-8 of 16-bit units: a high surrogate directly followed by
// a low one is one supplementary code point; every other unit is its own.
std::string reference_wtf8(const std::vector<u16>& units) {
    std::string result;
    usize index = 0;
    while (index < units.size()) {
        const std::uint32_t unit = units[index];
        if (unit >= 0xd800u && unit <= 0xdbffu && index + 1 < units.size() && units[index + 1] >= 0xdc00u &&
            units[index + 1] <= 0xdfffu) {
            encode(0x10000u + ((unit - 0xd800u) << 10) + (units[index + 1] - 0xdc00u), result);
            index = index + 2;
            continue;
        }
        encode(unit, result);
        index = index + 1;
    }
    return result;
}

std::string text_of(const Path& path) {
    const stxlib::PathText text = path.text();
    return std::string(text.data(), text.count());
}

// ---- N1 ----------------------------------------------------------------------

std::uint64_t posix_names = 0;

void check_posix_name(const std::string& bytes) {
    posix_names = posix_names + 1;
    Result<Path> made = Path::from_posix_name(bytes.data(), bytes.size());
    expect(made.succeeded(), "N1 from_posix_name");
    if (made.failed()) return;
    const Path& path = *made.value();
    expect(text_of(path) == reference_escape(bytes), "N1 escape");
    Result<stxlib::Buffer<char>> name = path.posix_name();
    expect(name.succeeded(), "N1 posix_name");
    if (name.failed()) return;
    const stxlib::View<char> spelled = (*name.value()).view();
    const bool same = spelled.count() == bytes.size() + 1 && spelled[bytes.size()] == '\0' &&
                      std::memcmp(spelled.data(), bytes.data(), bytes.size()) == 0;
    expect(same, "N1 round trip");
}

void check_posix_names() {
    std::vector<unsigned char> alphabet;
    for (unsigned byte = 1; byte <= 255; ++byte) {
        if (byte != '/') alphabet.push_back(static_cast<unsigned char>(byte));
    }
    std::string bytes;
    for (const unsigned char first : alphabet) {
        bytes.assign(1, static_cast<char>(first));
        check_posix_name(bytes);
        for (const unsigned char second : alphabet) {
            bytes.assign(1, static_cast<char>(first));
            bytes.push_back(static_cast<char>(second));
            check_posix_name(bytes);
            for (const unsigned char third : alphabet) {
                bytes.resize(2);
                bytes.push_back(static_cast<char>(third));
                check_posix_name(bytes);
            }
        }
    }
}

// ---- N2 ----------------------------------------------------------------------

// The bytes a text names if it is an escape image: each U+DC00..U+DCFF its
// byte, other surrogates refused; then the image must escape back to it.
bool reference_unescape(const std::vector<std::uint32_t>& code_points, std::string& bytes) {
    bytes.clear();
    std::string text;
    for (const std::uint32_t code_point : code_points) {
        encode(code_point, text);
        if (code_point >= 0xd800u && code_point <= 0xdfffu) {
            if (code_point < 0xdc00u || code_point > 0xdcffu) return false;
            bytes.push_back(static_cast<char>(code_point - 0xdc00u));
        } else {
            encode(code_point, bytes);
        }
    }
    return reference_escape(bytes) == text;
}

std::uint64_t texts = 0;

void check_text(const std::vector<std::uint32_t>& code_points) {
    texts = texts + 1;
    std::string text;
    for (const std::uint32_t code_point : code_points) encode(code_point, text);
    // A high surrogate directly before a low one is not WTF-8; T's only high
    // surrogates are U+D800 and U+DBFF.
    for (usize index = 0; index + 1 < code_points.size(); ++index) {
        const bool high = code_points[index] >= 0xd800u && code_points[index] <= 0xdbffu;
        const bool low = code_points[index + 1] >= 0xdc00u && code_points[index + 1] <= 0xdfffu;
        if (high && low) {
            expect(Path::parse_wtf8(text.data(), text.size()).failed(), "N2 paired surrogates refused");
            return;
        }
    }
    Result<Path> parsed = Path::parse_wtf8(text.data(), text.size());
    expect(parsed.succeeded(), "N2 parse_wtf8");
    if (parsed.failed()) return;
    std::string expected_bytes;
    const bool representable = reference_unescape(code_points, expected_bytes);
    Result<stxlib::Buffer<char>> name = (*parsed.value()).posix_name();
    expect(name.succeeded() == representable, "N2 representable");
    if (name.failed() || !representable) return;
    const stxlib::View<char> spelled = (*name.value()).view();
    expect(spelled.count() == expected_bytes.size() + 1 &&
               std::memcmp(spelled.data(), expected_bytes.data(), expected_bytes.size()) == 0,
           "N2 bytes");
    Result<Path> back = Path::from_posix_name(spelled.data(), spelled.count() - 1);
    expect(back.succeeded() && *back.value() == *parsed.value(), "N2 injective");
}

void check_texts() {
    std::vector<std::uint32_t> tokens;
    for (std::uint32_t character = 1; character < 0x80u; ++character) {
        // '.' alone and ".." normalize to themselves; '/' separates; ':' and
        // '\' are Path syntax, refused by parse_wtf8 (path_tests.cpp).
        if (character != '/' && character != ':' && character != '\\') tokens.push_back(character);
    }
    for (std::uint32_t escape = 0xdc00u; escape <= 0xdcffu; ++escape) tokens.push_back(escape);
    const std::uint32_t others[] = {0xd800u, 0xdbffu, 0xdd00u, 0xdfffu, 0x80u,    0x7ffu,
                                    0x800u,  0xfffdu, 0xffffu, 0x10000u, 0x10ffffu, 0xe9u};
    for (const std::uint32_t other : others) tokens.push_back(other);
    std::vector<std::uint32_t> code_points;
    for (const std::uint32_t first : tokens) {
        code_points.assign(1, first);
        check_text(code_points);
        for (const std::uint32_t second : tokens) {
            code_points.assign(1, first);
            code_points.push_back(second);
            check_text(code_points);
        }
    }
}

// ---- N3, N4 ------------------------------------------------------------------

std::uint64_t unit_sequences = 0;

void check_units(const std::vector<u16>& units) {
    unit_sequences = unit_sequences + 1;
    Result<Path> made = Path::from_windows_name(units.data(), units.size());
    expect(made.succeeded(), "N3 from_windows_name");
    if (made.failed()) return;
    const Path& path = *made.value();
    const std::string expected = reference_wtf8(units);
    expect(text_of(path) == expected, "N3 wtf8");
    Result<stxlib::Buffer<u16>> name = path.windows_name();
    expect(name.succeeded(), "N3 windows_name");
    if (name.succeeded()) {
        const stxlib::View<u16> spelled = (*name.value()).view();
        bool same = spelled.count() == units.size() + 1 && spelled[units.size()] == 0;
        for (usize index = 0; same && index < units.size(); ++index) same = spelled[index] == units[index];
        expect(same, "N3 round trip");
    }
    Result<Path> stored = Path::parse_wtf8(expected.data(), expected.size());
    expect(stored.succeeded() && *stored.value() == path, "N4 parse_wtf8");
}

void check_windows_names() {
    std::vector<u16> units(1);
    for (std::uint32_t unit = 1; unit <= 0xffffu; ++unit) {
        if (unit == '/' || unit == '\\' || unit == ':') continue;
        units[0] = static_cast<u16>(unit);
        check_units(units);
    }
    std::vector<u16> alphabet;
    for (std::uint32_t unit = 0xd800u; unit <= 0xdfffu; ++unit) alphabet.push_back(static_cast<u16>(unit));
    const u16 others[] = {'a', '.', 0x7f, 0x80, 0x7ff, 0x800, 0xfffd, 0xffff};
    for (const u16 other : others) alphabet.push_back(other);
    units.resize(2);
    for (const u16 first : alphabet) {
        for (const u16 second : alphabet) {
            units[0] = first;
            units[1] = second;
            check_units(units);
        }
    }
}

} // namespace

int main() {
    check_posix_names();
    check_texts();
    check_windows_names();
    std::printf("path names exhaustive: %llu POSIX names, %llu texts, %llu unit sequences, %llu comparisons, "
                "%llu mismatches\n",
                static_cast<unsigned long long>(posix_names), static_cast<unsigned long long>(texts),
                static_cast<unsigned long long>(unit_sequences), static_cast<unsigned long long>(comparisons),
                static_cast<unsigned long long>(mismatches));
    const int status = mismatches == 0 ? 0 : 1;
    return status;
}
