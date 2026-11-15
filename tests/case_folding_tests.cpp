// Tests of include/stxlib/case_folding.hpp (M2 C7).
//
// The reference is CaseFolding.txt itself (the path is the first argument;
// CMake passes third_party/unicode-18.0.0/CaseFolding.txt), read here by a
// parser of its own, independent of tools/gen_case_folding.py, which built
// the library's table from the same file.
//
// Properties:
//   F1  every code point 0 to 0x10FFFF folds to the file's C or S mapping,
//       or to itself when it has none (surrogates included); values above
//       0x10FFFF are unchanged; the file holds 1,501 C and 32 S mappings;
//   F2  folding is idempotent at every code point;
//   F3  every scalar value as one-scalar text: fold_case writes the UTF-8
//       form of its folding, folded_bytes agrees, and the size is within
//       maximum_folded_bytes;
//   F4  too small storage: nothing written, the size needed reported; the
//       growth bound is reached exactly (U+023A, two bytes to three);
//   F5  compare_folded on every pair of one-scalar texts drawn from the
//       table's sources and targets and the ASCII letters agrees with the
//       order of the reference foldings; equal_folded agrees with it;
//   F6  texts of several scalars: known cases (final sigma, the Kelvin sign,
//       sharp s not matching "ss", prefixes, empty text).
// F1 to F3 are exhaustive over their domains (a proof by enumeration once
// the test passes); F5 is exhaustive over its stated set only.

#include "stxlib/case_folding.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace text = stxlib::text;
using stxlib::TextView;
using stxlib::u32;
using stxlib::usize;

constexpr u32 code_points = 0x110000;

std::uint64_t mismatches = 0;
std::uint64_t comparisons = 0;

void expect(const bool condition, const char* const property, const u32 code_point) {
    comparisons = comparisons + 1;
    if (condition) return;
    mismatches = mismatches + 1;
    if (mismatches <= 20) std::fprintf(stderr, "%s mismatch at U+%04X\n", property, static_cast<unsigned>(code_point));
}

struct Reference final {
    std::vector<u32> folding;
    std::vector<u32> listed; // sources, in file order
    usize common{0};
    usize simple{0};
    bool read{false};
};

// The hexadecimal number at text[index], advancing index past it.
u32 hex_at(const std::string& line, usize& index) {
    u32 value = 0;
    while (index < line.size()) {
        const char digit = line[index];
        u32 nibble = 0;
        if (digit >= '0' && digit <= '9') {
            nibble = static_cast<u32>(digit - '0');
        } else if (digit >= 'A' && digit <= 'F') {
            nibble = static_cast<u32>(digit - 'A' + 10);
        } else {
            break;
        }
        value = value * 16 + nibble;
        index = index + 1;
    }
    return value;
}

Reference read_reference(const char* const path) {
    Reference reference;
    reference.folding.resize(code_points);
    for (u32 code_point = 0; code_point < code_points; ++code_point) reference.folding[code_point] = code_point;
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return reference;
    }
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        const std::string line(buffer);
        if (line.empty() || line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        // <code>; <status>; <mapping>; # <name>
        usize index = 0;
        const u32 source = hex_at(line, index);
        if (line.compare(index, 2, "; ") != 0) continue;
        const char status = line[index + 2];
        if (status != 'C' && status != 'S') continue;
        index = index + 5; // past "; X; "
        const u32 target = hex_at(line, index);
        if (index >= line.size() || line[index] != ';') {
            std::fprintf(stderr, "a C or S mapping of more than one code point: %s", buffer);
            continue;
        }
        if (source >= code_points) continue;
        reference.folding[source] = target;
        reference.listed.push_back(source);
        if (status == 'C') {
            reference.common = reference.common + 1;
        } else {
            reference.simple = reference.simple + 1;
        }
    }
    std::fclose(file);
    reference.read = true;
    return reference;
}

bool scalar_value(const u32 code_point) { return code_point < code_points && (code_point < 0xd800 || code_point > 0xdfff); }

// An encoder of its own (the test's, not the library's).
std::string utf8_of(const u32 scalar) {
    std::string bytes;
    if (scalar < 0x80) {
        bytes.push_back(static_cast<char>(scalar));
    } else if (scalar < 0x800) {
        bytes.push_back(static_cast<char>(0xc0 + (scalar / 64)));
        bytes.push_back(static_cast<char>(0x80 + (scalar % 64)));
    } else if (scalar < 0x10000) {
        bytes.push_back(static_cast<char>(0xe0 + (scalar / 4096)));
        bytes.push_back(static_cast<char>(0x80 + ((scalar / 64) % 64)));
        bytes.push_back(static_cast<char>(0x80 + (scalar % 64)));
    } else {
        bytes.push_back(static_cast<char>(0xf0 + (scalar / 262144)));
        bytes.push_back(static_cast<char>(0x80 + ((scalar / 4096) % 64)));
        bytes.push_back(static_cast<char>(0x80 + ((scalar / 64) % 64)));
        bytes.push_back(static_cast<char>(0x80 + (scalar % 64)));
    }
    return bytes;
}

// A view of a test string. Requirement: the string is UTF-8 and outlives the view.
TextView view_of(const std::string& value) {
    stxlib::Result<TextView> validated = stxlib::validate_utf8(value.data(), value.size());
    CHECK(validated.succeeded());
    if (!validated.succeeded()) return TextView();
    const TextView result = static_cast<stxlib::Result<TextView>&&>(validated).take_value();
    return result;
}

char storage[4096];

std::string folded(const std::string& value) {
    const text::Formatted formatted = text::fold_case(stxlib::MutableView<char>(storage), view_of(value));
    CHECK(formatted.succeeded());
    if (!formatted.succeeded()) return std::string("<too small>");
    const TextView* const written = formatted.text();
    const std::string result((*written).data(), (*written).count());
    return result;
}

void every_code_point(const Reference& reference) {
    CHECK(reference.read);
    CHECK(reference.common == 1501);
    CHECK(reference.simple == 32);
    CHECK(reference.listed.size() == 1533);
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        const u32 got = text::fold_case_simple(code_point);
        expect(got == reference.folding[code_point], "F1", code_point);
        expect(text::fold_case_simple(got) == got, "F2", code_point);
    }
    const u32 beyond[5] = {0x110000, 0x110041, 0x7fffffff, 0xfffffffe, 0xffffffff};
    for (const u32 code_point : beyond) expect(text::fold_case_simple(code_point) == code_point, "F1 beyond", code_point);
    CHECK(text::case_folding_unicode_major == 18 && text::case_folding_unicode_minor == 0 &&
          text::case_folding_unicode_update == 0);
}

void every_scalar_as_text(const Reference& reference) {
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        if (!scalar_value(code_point)) continue;
        const std::string source = utf8_of(code_point);
        const std::string expected = utf8_of(reference.folding[code_point]);
        const TextView view = view_of(source);
        expect(text::folded_bytes(view) == expected.size(), "F3 size", code_point);
        expect(expected.size() <= text::maximum_folded_bytes(source.size()), "F3 bound", code_point);
        const text::Formatted formatted = text::fold_case(stxlib::MutableView<char>(storage), view);
        bool same = formatted.succeeded();
        if (same) {
            const TextView* const written = formatted.text();
            same = (*written).count() == expected.size() &&
                   std::memcmp((*written).data(), expected.data(), expected.size()) == 0;
        }
        expect(same, "F3 text", code_point);
    }
}

void storage_rules() {
    // "ÀBC" folds to "àbc" (4 bytes): 3 bytes of room is too small.
    const std::string source = "\xc3\x80" "BC";
    char small[3] = {'#', '#', '#'};
    const text::Formatted refused = text::fold_case(stxlib::MutableView<char>(small), view_of(source));
    CHECK(!refused.succeeded());
    CHECK(refused.text() == nullptr);
    CHECK(refused.required_bytes() == 4);
    CHECK(small[0] == '#' && small[1] == '#' && small[2] == '#');
    char exact[4] = {};
    const text::Formatted fitted = text::fold_case(stxlib::MutableView<char>(exact), view_of(source));
    CHECK(fitted.succeeded() && fitted.required_bytes() == 4);
    CHECK(std::memcmp(exact, "\xc3\xa0" "bc", 4) == 0);
    // The growth bound, reached: 100 x U+023A (2 bytes) fold to U+2C65 (3).
    std::string strokes;
    std::string expected;
    for (int index = 0; index < 100; ++index) {
        strokes += "\xc8\xba";
        expected += "\xe2\xb1\xa5";
    }
    CHECK(text::folded_bytes(view_of(strokes)) == text::maximum_folded_bytes(strokes.size()));
    CHECK(folded(strokes) == expected);
    // Empty text, and empty storage for it.
    const text::Formatted empty = text::fold_case(stxlib::MutableView<char>(), TextView());
    CHECK(empty.succeeded() && empty.required_bytes() == 0);
}

int sign_of(const u32 left, const u32 right) {
    if (left < right) return -1;
    if (left > right) return 1;
    return 0;
}

int order_value(const text::TextOrder order) { return static_cast<int>(order); }

void pairs(const Reference& reference) {
    std::vector<u32> set;
    for (const u32 source : reference.listed) {
        set.push_back(source);
        set.push_back(reference.folding[source]);
    }
    for (u32 letter = 0x41; letter <= 0x7a; ++letter) set.push_back(letter);
    std::vector<std::string> texts;
    texts.reserve(set.size());
    for (const u32 code_point : set) texts.push_back(utf8_of(code_point));
    std::vector<TextView> views;
    views.reserve(texts.size());
    for (const std::string& value : texts) views.push_back(view_of(value));
    for (usize left = 0; left < set.size(); ++left) {
        for (usize right = 0; right < set.size(); ++right) {
            const int wanted = sign_of(reference.folding[set[left]], reference.folding[set[right]]);
            const text::TextOrder order = text::compare_folded(views[left], views[right]);
            const bool equal = text::equal_folded(views[left], views[right]);
            expect(order_value(order) == wanted && equal == (wanted == 0), "F5", set[left]);
        }
    }
}

void known_texts() {
    // Greek: capital sigma and final sigma both fold to small sigma.
    CHECK(folded("\xce\xa3\xce\x91\xce\xa3") == "\xcf\x83\xce\xb1\xcf\x83");
    CHECK(text::equal_folded(view_of("\xce\xbf\xcf\x82"), view_of("\xce\x9f\xce\xa3")));
    // The Kelvin sign folds to ASCII k (three bytes to one).
    CHECK(folded("\xe2\x84\xaa") == "k");
    CHECK(text::equal_folded(view_of("\xe2\x84\xaa"), view_of("K")));
    // Long s folds to s.
    CHECK(text::equal_folded(view_of("\xc5\xbf"), view_of("S")));
    // Simple folding only: sharp s does not match "ss"; capital sharp s
    // (U+1E9E, status S) folds to sharp s.
    CHECK(!text::equal_folded(view_of("Stra\xc3\x9f" "e"), view_of("STRASSE")));
    CHECK(text::equal_folded(view_of("STRA\xe1\xba\x9e" "E"), view_of("stra\xc3\x9f" "e")));
    // No Turkic mapping: dotted capital I (U+0130) has only F and T
    // mappings, so it stays; capital I folds to i.
    CHECK(folded("\xc4\xb0") == "\xc4\xb0");
    CHECK(folded("I") == "i");
    // Order: a proper prefix first; case does not order.
    CHECK(text::compare_folded(view_of("abc"), view_of("ABCD")) == text::TextOrder::before);
    CHECK(text::compare_folded(view_of("ABCD"), view_of("abc")) == text::TextOrder::after);
    CHECK(text::compare_folded(view_of("Hello"), view_of("hELLO")) == text::TextOrder::same);
    CHECK(text::compare_folded(TextView(), TextView()) == text::TextOrder::same);
    CHECK(text::compare_folded(TextView(), view_of("a")) == text::TextOrder::before);
    // Deseret (four bytes each).
    CHECK(folded("\xf0\x90\x90\x80") == "\xf0\x90\x90\xa8");
    // Mixed: ASCII, two-, three- and four-byte scalars in one text.
    CHECK(folded("A\xc3\x80\xe2\x84\xaa\xf0\x90\x90\x80z") == "a\xc3\xa0k\xf0\x90\x90\xa8z");
}

} // namespace

int main(const int argc, char** const argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: stxlib_case_folding_tests <CaseFolding.txt>\n");
        return 2;
    }
    const Reference reference = read_reference(argv[1]);
    every_code_point(reference);
    every_scalar_as_text(reference);
    storage_rules();
    pairs(reference);
    known_texts();
    std::printf("stxlib_case_folding_tests: %llu comparisons, %llu mismatches\n",
                static_cast<unsigned long long>(comparisons), static_cast<unsigned long long>(mismatches));
    const int harness = mismatches == 0 ? 0 : 1;
    const int checks = stxlib_tests::finish("stxlib_case_folding_tests");
    return harness | checks;
}
