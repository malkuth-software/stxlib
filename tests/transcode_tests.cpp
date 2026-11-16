// Tests of public transcoding (transcode.hpp, R15).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - every Unicode scalar value: its UTF-8 (an independent encoder, Unicode
//   Table 3-6) converts to the expected UTF-16 and UTF-32 and back, and
//   code_point_at, code_point_count and utf16_count agree;
// - every high-low surrogate pair (1024 x 1024): one supplementary code
//   point, in UTF-8 and in WTF-8, and back;
// - every single 16-bit unit, and every sequence of up to four units from
//   fifteen units at the encoding edges (the ASCII, two- and three-byte
//   limits, both surrogate ranges, a pair): text_of_utf16,
//   utf8_count_of_utf16 and write_utf8_of_utf16 accept exactly the
//   sequences without an unpaired surrogate (an independent model, which
//   also gives the index of the first bad unit), and wtf8_of_utf16 then
//   utf16_of_wtf8 gives every sequence back;
// - is_wtf8 on every byte string of up to two bytes and every three-byte
//   string with a lead E0..EF, against an independent generalized decoder;
//   accepted bytes round-trip through UTF-16;
// - failures leave outputs unchanged: a short output, a refused
//   allocation, a bad unit.

#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/transcode.hpp"

#include "test_support.hpp"

#include <string>
#include <vector>

namespace {

using stxlib::u16;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

std::string encode_utf8(const u32 value) {
    std::string result;
    if (value < 0x80u) {
        result.push_back(static_cast<char>(value));
    } else if (value < 0x800u) {
        result.push_back(static_cast<char>(0xc0u | (value >> 6)));
        result.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else if (value < 0x10000u) {
        result.push_back(static_cast<char>(0xe0u | (value >> 12)));
        result.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    } else {
        result.push_back(static_cast<char>(0xf0u | (value >> 18)));
        result.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
        result.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    }
    return result;
}

stxlib::TextView view_of(const std::string& text) {
    const stxlib::OperationResult<stxlib::TextView> validated = stxlib::validate_utf8(text.data(), text.size());
    if (validated.failed()) return stxlib::TextView();
    const stxlib::TextView result = *validated.value();
    return result;
}

bool same_units(const stxlib::Vector<u16>& got, const std::vector<u16>& expected) {
    if (got.count() != expected.size()) return false;
    for (usize index = 0; index < expected.size(); ++index) {
        if (got[index] != expected[index]) return false;
    }
    return true;
}

bool same_bytes(const stxlib::Vector<char>& got, const std::string& expected) {
    if (got.count() != expected.size()) return false;
    for (usize index = 0; index < expected.size(); ++index) {
        if (got[index] != expected[index]) return false;
    }
    return true;
}

stxlib::View<u16> units_view(const std::vector<u16>& units) {
    const stxlib::View<u16> result(units.data(), units.size());
    return result;
}

// ---- scalar values ------------------------------------------------------------

void scalars_exhaustive() {
    bool same = true;
    for (u32 value = 0; value <= 0x10ffffu; ++value) {
        if (value >= 0xd800u && value <= 0xdfffu) continue;
        const std::string bytes = encode_utf8(value);
        const stxlib::TextView text = view_of(bytes);
        std::vector<u16> expected;
        if (value < 0x10000u) {
            expected.push_back(static_cast<u16>(value));
        } else {
            expected.push_back(static_cast<u16>(0xd800u + ((value - 0x10000u) >> 10)));
            expected.push_back(static_cast<u16>(0xdc00u + ((value - 0x10000u) & 0x3ffu)));
        }
        same = same && text.count() == bytes.size();
        same = same && stxlib::code_point_count(text) == 1 && stxlib::utf16_count(text) == expected.size();
        const stxlib::OperationResult<stxlib::DecodedCodePoint> decoded = stxlib::code_point_at(text, 0);
        same = same && decoded.succeeded() && (*decoded.value()).value == value &&
               (*decoded.value()).length == bytes.size();

        stxlib::OperationResult<stxlib::Vector<u16>> units = stxlib::utf16_of(text);
        same = same && units.succeeded() && same_units(*units.value(), expected);
        const stxlib::OperationResult<stxlib::Utf8String> back = stxlib::text_of_utf16(units_view(expected));
        same = same && back.succeeded() && (*back.value()).equals(text);

        stxlib::OperationResult<stxlib::Vector<u32>> scalars = stxlib::utf32_of(text);
        same = same && scalars.succeeded() && (*scalars.value()).count() == 1 && (*scalars.value())[0] == value;
        const u32 single[] = {value};
        const stxlib::OperationResult<stxlib::Utf8String> again = stxlib::text_of_utf32(stxlib::View<u32>(single));
        same = same && again.succeeded() && (*again.value()).equals(text);
    }
    CHECK(same);

    // Not scalar values: refused with the index.
    const u32 bad[] = {0x41u, 0xd800u, 0x42u};
    const stxlib::OperationResult<stxlib::Utf8String> surrogate = stxlib::text_of_utf32(stxlib::View<u32>(bad));
    CHECK(surrogate.failed() && (*surrogate.error()).kind() == stxlib::ErrorKind::invalid_argument &&
          (*surrogate.error()).code() == 1);
    const u32 beyond[] = {0x110000u};
    CHECK(stxlib::text_of_utf32(stxlib::View<u32>(beyond)).failed());
}

void surrogate_pairs_exhaustive() {
    bool same = true;
    for (u32 high = 0xd800u; high <= 0xdbffu; ++high) {
        for (u32 low = 0xdc00u; low <= 0xdfffu; ++low) {
            const u32 value = 0x10000u + ((high - 0xd800u) << 10) + (low - 0xdc00u);
            const u16 pair[] = {static_cast<u16>(high), static_cast<u16>(low)};
            const stxlib::View<u16> units(pair);
            const std::string expected = encode_utf8(value);
            const stxlib::OperationResult<stxlib::usize> counted = stxlib::utf8_count_of_utf16(units);
            same = same && counted.succeeded() && *counted.value() == 4;
            char output[4]{};
            const stxlib::OperationResult<stxlib::usize> written =
                stxlib::write_utf8_of_utf16(units, stxlib::MutableView<char>(output));
            same = same && written.succeeded() && *written.value() == 4 && std::string(output, 4) == expected;
            stxlib::OperationResult<stxlib::Vector<char>> wtf8 = stxlib::wtf8_of_utf16(units);
            same = same && wtf8.succeeded() && same_bytes(*wtf8.value(), expected);
        }
    }
    CHECK(same);
}

// ---- 16-bit unit sequences ----------------------------------------------------

// The model: the index of the first unpaired surrogate, or -1; and the code
// points read, pairing a high surrogate with a following low one.
struct UnitModel final {
    long bad_index;
    std::string utf8;   // when well-formed
    std::string wtf8;   // always
};

UnitModel model_units(const std::vector<u16>& units) {
    UnitModel result{-1, std::string(), std::string()};
    usize index = 0;
    while (index < units.size()) {
        u32 value = units[index];
        const usize at = index;
        index = index + 1;
        const bool high = value >= 0xd800u && value <= 0xdbffu;
        if (high && index < units.size() && units[index] >= 0xdc00u && units[index] <= 0xdfffu) {
            value = 0x10000u + ((value - 0xd800u) << 10) + (units[index] - 0xdc00u);
            index = index + 1;
        }
        const bool unpaired = value >= 0xd800u && value <= 0xdfffu;
        if (unpaired && result.bad_index < 0) result.bad_index = static_cast<long>(at);
        const std::string bytes = encode_utf8(value);  // the 3-byte form for a surrogate
        result.wtf8 += bytes;
        if (!unpaired) result.utf8 += bytes;
    }
    return result;
}

void check_units(const std::vector<u16>& units) {
    const UnitModel model = model_units(units);
    const stxlib::View<u16> view = units_view(units);
    const bool well_formed = model.bad_index < 0;

    const stxlib::OperationResult<stxlib::usize> counted = stxlib::utf8_count_of_utf16(view);
    CHECK(counted.succeeded() == well_formed);
    if (well_formed) {
        CHECK(*counted.value() == model.utf8.size());
    } else {
        CHECK((*counted.error()).kind() == stxlib::ErrorKind::invalid_argument &&
              static_cast<long>((*counted.error()).code()) == model.bad_index);
    }

    stxlib::OperationResult<stxlib::Utf8String> text = stxlib::text_of_utf16(view);
    CHECK(text.succeeded() == well_formed);
    if (well_formed) CHECK((*text.value()).equals(view_of(model.utf8)));

    // Into caller storage: exact room, and one byte short (unchanged).
    char output[16];
    for (char& byte : output) byte = '#';
    const usize room = well_formed ? model.utf8.size() : 16;
    const stxlib::OperationResult<stxlib::usize> written =
        stxlib::write_utf8_of_utf16(view, stxlib::MutableView<char>(output, room));
    CHECK(written.succeeded() == well_formed);
    if (well_formed) {
        CHECK(std::string(output, model.utf8.size()) == model.utf8);
        if (!model.utf8.empty()) {
            char short_output[16];
            for (char& byte : short_output) byte = '#';
            const stxlib::OperationResult<stxlib::usize> refused = stxlib::write_utf8_of_utf16(
                view, stxlib::MutableView<char>(short_output, model.utf8.size() - 1));
            CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::limit_exceeded);
            bool untouched = true;
            for (const char byte : short_output) untouched = untouched && byte == '#';
            CHECK(untouched);
        }
    } else {
        bool untouched = true;
        for (const char byte : output) untouched = untouched && byte == '#';
        CHECK(untouched);
    }

    // WTF-8 carries every sequence, and gives it back.
    stxlib::OperationResult<stxlib::Vector<char>> wtf8 = stxlib::wtf8_of_utf16(view);
    CHECK(wtf8.succeeded() && same_bytes(*wtf8.value(), model.wtf8));
    if (wtf8.succeeded()) {
        const stxlib::View<char> bytes = (*wtf8.value()).view();
        CHECK(stxlib::is_wtf8(bytes));
        stxlib::OperationResult<stxlib::Vector<u16>> back = stxlib::utf16_of_wtf8(bytes);
        CHECK(back.succeeded() && same_units(*back.value(), units));
    }
}

void units_exhaustive() {
    for (u32 unit = 0; unit <= 0xffffu; ++unit) check_units({static_cast<u16>(unit)});
    const u16 edges[] = {0x0000, 0x0041, 0x007f, 0x0080, 0x07ff, 0x0800, 0xd7ff, 0xd800,
                         0xdbff, 0xdc00, 0xdfff, 0xe000, 0xffff, 0xd834, 0xdd1e};
    std::vector<std::vector<u16>> level{std::vector<u16>()};
    check_units(std::vector<u16>());
    for (usize length = 1; length <= 4; ++length) {
        std::vector<std::vector<u16>> next;
        for (const std::vector<u16>& prefix : level) {
            for (const u16 unit : edges) {
                std::vector<u16> extended = prefix;
                extended.push_back(unit);
                check_units(extended);
                next.push_back(extended);
            }
        }
        level = next;
    }
}

// ---- WTF-8 acceptance -----------------------------------------------------------

// An independent generalized decoder: UTF-8 with the surrogates' ED A0..BF
// allowed, and no high surrogate followed by a low one.
bool model_wtf8(const std::string& bytes) {
    usize index = 0;
    bool after_high = false;
    while (index < bytes.size()) {
        const u32 lead = static_cast<u8>(bytes[index]);
        usize length = 0;
        u32 second_low = 0x80u;
        u32 second_high = 0xbfu;
        u32 value = 0;
        if (lead < 0x80u) {
            length = 1;
            value = lead;
        } else if (lead >= 0xc2u && lead <= 0xdfu) {
            length = 2;
            value = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            length = 3;
            value = lead & 0x0fu;
            if (lead == 0xe0u) second_low = 0xa0u;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            length = 4;
            value = lead & 0x07u;
            if (lead == 0xf0u) second_low = 0x90u;
            if (lead == 0xf4u) second_high = 0x8fu;
        } else {
            return false;
        }
        if (index + length > bytes.size()) return false;
        for (usize offset = 1; offset < length; ++offset) {
            const u32 next = static_cast<u8>(bytes[index + offset]);
            const u32 low = offset == 1 ? second_low : 0x80u;
            const u32 high = offset == 1 ? second_high : 0xbfu;
            if (next < low || next > high) return false;
            value = (value << 6) | (next & 0x3fu);
        }
        if (after_high && value >= 0xdc00u && value <= 0xdfffu) return false;
        after_high = value >= 0xd800u && value <= 0xdbffu;
        index = index + length;
    }
    return true;
}

void check_wtf8_bytes(const std::string& bytes) {
    const stxlib::View<char> view(bytes.data(), bytes.size());
    const bool accepted = stxlib::is_wtf8(view);
    CHECK(accepted == model_wtf8(bytes));
    stxlib::OperationResult<stxlib::Vector<u16>> units = stxlib::utf16_of_wtf8(view);
    CHECK(units.succeeded() == accepted);
    if (accepted && units.succeeded()) {
        stxlib::OperationResult<stxlib::Vector<char>> back = stxlib::wtf8_of_utf16((*units.value()).view());
        CHECK(back.succeeded() && same_bytes(*back.value(), bytes));
    }
}

void wtf8_exhaustive() {
    check_wtf8_bytes(std::string());
    for (int first = 0; first < 256; ++first) {
        check_wtf8_bytes(std::string(1, static_cast<char>(first)));
        for (int second = 0; second < 256; ++second) {
            const char pair[] = {static_cast<char>(first), static_cast<char>(second)};
            check_wtf8_bytes(std::string(pair, 2));
        }
    }
    for (int first = 0xe0; first <= 0xef; ++first) {
        for (int second = 0; second < 256; ++second) {
            for (int third = 0; third < 256; ++third) {
                const char triple[] = {static_cast<char>(first), static_cast<char>(second), static_cast<char>(third)};
                check_wtf8_bytes(std::string(triple, 3));
            }
        }
    }
    // A high surrogate then a low one is the pair's 4-byte form, never two
    // 3-byte forms; a low then a high is two unpaired surrogates.
    CHECK(!stxlib::is_wtf8(stxlib::View<char>("\xed\xa0\x80\xed\xb0\x80", 6)));
    CHECK(stxlib::is_wtf8(stxlib::View<char>("\xed\xb0\x80\xed\xa0\x80", 6)));
}

// ---- failures leave outputs unchanged --------------------------------------------

void failures_unchanged() {
    u16 short_units[1] = {0x1234};
    const stxlib::OperationResult<stxlib::usize> too_short =
        stxlib::write_utf16("ab", stxlib::MutableView<u16>(short_units));
    CHECK(too_short.failed() && (*too_short.error()).kind() == stxlib::ErrorKind::limit_exceeded);
    CHECK(short_units[0] == 0x1234);
    u16 room[4] = {0, 0, 0, 0};
    const stxlib::OperationResult<stxlib::usize> fits = stxlib::write_utf16("a\xf0\x9d\x84\x9e",
                                                                            stxlib::MutableView<u16>(room));
    CHECK(fits.succeeded() && *fits.value() == 3 && room[0] == 0x61 && room[1] == 0xd834 && room[2] == 0xdd1e);

    stxlib::OperationResult<stxlib::Utf8String> made = stxlib::Utf8String::copy_of("kept");
    CHECK(made.succeeded());
    stxlib::Utf8String text = static_cast<stxlib::OperationResult<stxlib::Utf8String>&&>(made).take_value();
    std::vector<u16> many(64, static_cast<u16>(0x00e9));
    stxlib::detail::arm_vocabulary_refusals(0);
    const stxlib::OperationStatus refused = stxlib::append_utf16(text, units_view(many));
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);
    CHECK(text.equals("kept"));
    const u32 bad[] = {0x61u, 0x62u, 0xdfffu};
    const stxlib::OperationStatus rejected = stxlib::append_utf32(text, stxlib::View<u32>(bad));
    CHECK(rejected.failed() && (*rejected.error()).code() == 2 && text.equals("kept"));
    // A long append crosses the 256-byte pieces.
    std::vector<u16> long_units(1000, static_cast<u16>(0x20ac));
    CHECK(stxlib::append_utf16(text, units_view(long_units)).succeeded());
    CHECK(text.count() == 4 + 3000 && stxlib::code_point_count(text.view()) == 1004);

    const stxlib::OperationResult<stxlib::DecodedCodePoint> inside = stxlib::code_point_at("\xc3\xa9", 1);
    CHECK(inside.failed() && (*inside.error()).kind() == stxlib::ErrorKind::invalid_argument);
    const stxlib::OperationResult<stxlib::DecodedCodePoint> outside = stxlib::code_point_at("a", 1);
    CHECK(outside.failed() && (*outside.error()).kind() == stxlib::ErrorKind::out_of_range);
    const stxlib::OperationResult<stxlib::Vector<u16>> not_wtf8 = stxlib::utf16_of_wtf8(stxlib::View<char>("a\xff", 2));
    CHECK(not_wtf8.failed() && (*not_wtf8.error()).code() == 1);
}

} // namespace

int main() {
    scalars_exhaustive();
    surrogate_pairs_exhaustive();
    units_exhaustive();
    wtf8_exhaustive();
    failures_unchanged();
    return stxlib_tests::finish("transcode");
}
