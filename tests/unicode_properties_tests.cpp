// Tests of include/stxlib/unicode_properties.hpp against UnicodeData.txt
// (Unicode 18.0.0, third_party/unicode-18.0.0), read by the test's own
// parser.
//
//   U1  general_category of every code point 0 to 0x10FFFF is the file's
//       (ranges "<..., First>" to "<..., Last>" included; a code point the
//       file does not list is Cn), and above 0x10FFFF it is Cn.
//   U2  combining_class of every code point is the file's.
//   U3  canonical_decomposition of every code point is the file's canonical
//       mapping (no <tag>) where the file has one, Hangul syllables by
//       their algorithm (LV to L V, LVT to LV T), and false elsewhere.
//   U4  canonical_composition: for every canonical two-code-point mapping
//       c = (a, b) of the file, composition gives c or nothing; it gives
//       nothing when c is a non-starter or a starts with a non-starter
//       (NFC's derivable exclusions); the composites it does not give, of
//       the rest, are counted and listed (they are the script-specific and
//       post-composition-version exclusions, which UnicodeData.txt does not
//       mark: CompositionExclusions.txt is not in the tree). Every Hangul
//       L V and LV T pair composes by the algorithm. A bounded search for
//       pairs composition gives that the file has no mapping for: every
//       first of a mapping, and the Hangul L and LV syllables, against every
//       second of a mapping and every code point of a combining class above
//       0.
#include "stxlib/unicode_properties.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace text = stxlib::text;
using stxlib::u32;
using stxlib::usize;

constexpr u32 code_points = 0x110000;

struct Row final {
    text::GeneralCategory category{text::GeneralCategory::unassigned};
    u32 combining{0};
    u32 first{0};
    u32 second{0};
    bool canonical{false};
};

struct Reference final {
    std::vector<Row> rows{};
    bool read{false};
};

[[nodiscard]] u32 hex_at(const std::string& line, usize& index) {
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

[[nodiscard]] text::GeneralCategory category_of(const std::string& name) {
    const char* const names[30] = {"Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps", "Pe",
                                   "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"};
    for (u32 index = 0; index < 30; ++index) {
        if (name == names[index]) return static_cast<text::GeneralCategory>(index);
    }
    std::fprintf(stderr, "unknown category %s\n", name.c_str());
    return text::GeneralCategory::unassigned;
}

// The fields of a line split at ';'.
[[nodiscard]] std::vector<std::string> fields_of(const std::string& line) {
    std::vector<std::string> fields{};
    std::string field{};
    for (const char letter : line) {
        if (letter == ';') {
            fields.push_back(field);
            field.clear();
        } else if (letter != '\n' && letter != '\r') {
            field.push_back(letter);
        }
    }
    fields.push_back(field);
    return fields;
}

[[nodiscard]] Reference read_reference(const char* const path) {
    Reference reference{};
    reference.rows.resize(code_points);
    std::FILE* const file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", path);
        return reference;
    }
    char buffer[1024];
    u32 range_start = 0;
    bool in_range = false;
    while (std::fgets(buffer, sizeof buffer, file) != nullptr) {
        const std::vector<std::string> fields = fields_of(std::string(buffer));
        if (fields.size() < 6) continue;
        usize index = 0;
        const u32 code_point = hex_at(fields[0], index);
        if (code_point >= code_points) continue;
        Row row{};
        row.category = category_of(fields[2]);
        row.combining = static_cast<u32>(std::stoul(fields[3]));
        const std::string& mapping = fields[5];
        if (!mapping.empty() && mapping[0] != '<') {
            usize place = 0;
            row.first = hex_at(mapping, place);
            if (place < mapping.size() && mapping[place] == ' ') {
                place = place + 1;
                row.second = hex_at(mapping, place);
            }
            if (place != mapping.size()) std::fprintf(stderr, "a canonical mapping of more than two: %s", buffer);
            row.canonical = true;
        }
        const std::string& name = fields[1];
        if (name.size() > 8 && name.compare(name.size() - 8, 8, ", First>") == 0) {
            range_start = code_point;
            in_range = true;
            continue;
        }
        if (in_range && name.size() > 7 && name.compare(name.size() - 7, 7, ", Last>") == 0) {
            for (u32 inside = range_start; inside <= code_point; ++inside) reference.rows[inside] = row;
            in_range = false;
            continue;
        }
        reference.rows[code_point] = row;
    }
    std::fclose(file);
    reference.read = true;
    return reference;
}

constexpr u32 hangul_base = 0xAC00;
constexpr u32 hangul_count = 11172;
constexpr u32 lead_base = 0x1100;
constexpr u32 vowel_base = 0x1161;
constexpr u32 trail_base = 0x11A7;
constexpr u32 trail_count = 28;
constexpr u32 vowel_count = 21;

void categories_and_classes(const Reference& reference) {
    u32 category_wrong = 0;
    u32 class_wrong = 0;
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        const Row& row = reference.rows[code_point];
        if (text::general_category(code_point) != row.category) {
            category_wrong = category_wrong + 1;
            if (category_wrong <= 10) std::fprintf(stderr, "  category differs at U+%04X\n", static_cast<unsigned>(code_point));
        }
        if (text::combining_class(code_point) != row.combining) class_wrong = class_wrong + 1;
    }
    CHECK(category_wrong == 0);
    CHECK(class_wrong == 0);
    const u32 beyond[] = {0x110000U, 0x7FFFFFFFU, 0xFFFFFFFFU};
    for (const u32 code_point : beyond) {
        CHECK(text::general_category(code_point) == text::GeneralCategory::unassigned && text::combining_class(code_point) == 0);
    }
    CHECK(text::general_category(0x200D) == text::GeneralCategory::format && text::general_category('A') == text::GeneralCategory::uppercase_letter);
}

void decompositions(const Reference& reference) {
    u32 wrong = 0;
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        const Row& row = reference.rows[code_point];
        u32 first = 1;
        u32 second = 1;
        const bool found = text::canonical_decomposition(code_point, first, second);
        bool same = false;
        if (code_point >= hangul_base && code_point < hangul_base + hangul_count) {
            const u32 index = code_point - hangul_base;
            const u32 trail = index % trail_count;
            if (trail == 0) {
                same = found && first == lead_base + index / (vowel_count * trail_count) &&
                       second == vowel_base + (index % (vowel_count * trail_count)) / trail_count;
            } else {
                same = found && first == code_point - trail && second == trail_base + trail;
            }
        } else if (row.canonical) {
            same = found && first == row.first && second == row.second;
        } else {
            same = !found && first == 0 && second == 0;
        }
        if (!same) {
            wrong = wrong + 1;
            if (wrong <= 10) std::fprintf(stderr, "  decomposition differs at U+%04X\n", static_cast<unsigned>(code_point));
        }
    }
    CHECK(wrong == 0);
}

void compositions(const Reference& reference) {
    std::vector<u32> firsts{};
    std::vector<bool> is_first(code_points, false);
    std::vector<bool> is_second(code_points, false);
    u32 wrong = 0;
    u32 given = 0;
    u32 derivable_excluded = 0;
    std::vector<u32> not_given{};
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        const Row& row = reference.rows[code_point];
        if (!row.canonical || row.second == 0) continue;
        if (!is_first[row.first]) firsts.push_back(row.first);
        is_first[row.first] = true;
        is_second[row.second] = true;
        u32 made = 1;
        const bool composed = text::canonical_composition(row.first, row.second, made);
        const bool non_starter = row.combining != 0 || reference.rows[row.first].combining != 0;
        if (composed && made != code_point) wrong = wrong + 1;
        if (!composed && made != 0) wrong = wrong + 1;
        if (non_starter) {
            derivable_excluded = derivable_excluded + 1;
            if (composed) wrong = wrong + 1;
        } else if (composed) {
            given = given + 1;
        } else {
            not_given.push_back(code_point);
        }
    }
    CHECK(wrong == 0);
    std::printf("composition: %u composites given, %u non-starter mappings excluded, %zu others not given (exclusions):",
                given, derivable_excluded, not_given.size());
    for (const u32 code_point : not_given) std::printf(" %04X", static_cast<unsigned>(code_point));
    std::printf("\n");
    // Hangul by the algorithm.
    u32 hangul_wrong = 0;
    for (u32 index = 0; index < hangul_count; ++index) {
        const u32 syllable = hangul_base + index;
        u32 made = 0;
        if (index % trail_count == 0) {
            const u32 lead = lead_base + index / (vowel_count * trail_count);
            const u32 vowel = vowel_base + (index % (vowel_count * trail_count)) / trail_count;
            if (!text::canonical_composition(lead, vowel, made) || made != syllable) hangul_wrong = hangul_wrong + 1;
        } else {
            const u32 trail = trail_base + index % trail_count;
            if (!text::canonical_composition(syllable - index % trail_count, trail, made) || made != syllable) hangul_wrong = hangul_wrong + 1;
        }
    }
    CHECK(hangul_wrong == 0);
    // The bounded search for composites the file does not have.
    std::vector<u32> seconds{};
    for (u32 code_point = 0; code_point < code_points; ++code_point) {
        if (is_second[code_point] || reference.rows[code_point].combining != 0) seconds.push_back(code_point);
    }
    for (u32 lead = 0; lead < 19; ++lead) firsts.push_back(lead_base + lead);
    for (u32 index = 0; index < hangul_count; index = index + trail_count) firsts.push_back(hangul_base + index);
    std::uint64_t searched = 0;
    u32 unexpected = 0;
    for (const u32 first : firsts) {
        for (const u32 second : seconds) {
            searched = searched + 1;
            u32 made = 0;
            if (!text::canonical_composition(first, second, made)) continue;
            u32 back_first = 0;
            u32 back_second = 0;
            const bool decomposes = text::canonical_decomposition(made, back_first, back_second);
            if (!decomposes || back_first != first || back_second != second) {
                unexpected = unexpected + 1;
                if (unexpected <= 10) std::fprintf(stderr, "  U+%04X U+%04X compose to U+%04X\n", first, second, made);
            }
        }
    }
    std::printf("composition search: %zu firsts by %zu seconds, %llu pairs, %u not the inverse of a decomposition\n", firsts.size(),
                seconds.size(), static_cast<unsigned long long>(searched), unexpected);
    CHECK(unexpected == 0);
    u32 made = 7;
    CHECK(!text::canonical_composition(0xFFFFFFFFU, 0x0301, made) && made == 0);
    CHECK(text::canonical_composition('e', 0x0301, made) && made == 0x00E9);
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    if (argument_count < 2) {
        std::fprintf(stderr, "usage: stxlib_unicode_properties_tests <UnicodeData.txt>\n");
        return 2;
    }
    const Reference reference = read_reference(arguments[1]);
    CHECK(reference.read);
    if (reference.read) {
        categories_and_classes(reference);
        decompositions(reference);
        compositions(reference);
    }
    const int status = stxlib_tests::finish("stxlib_unicode_properties_tests");
    return status;
}
