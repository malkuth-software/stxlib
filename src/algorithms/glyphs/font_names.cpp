// stxlib::glyphs: a font's names (include/stxlib/font_file.hpp). The name
// table's records are checked as FreeType 2.14.2 checks them
// (src/sfnt/ttload.c, tt_face_load_name) and one is chosen as it chooses
// (src/sfnt/sfobjs.c, tt_face_get_name, and sfnt_load_face for the family
// and style); the text is decoded in full, not cut to ASCII.

#include "stxlib/font_file.hpp"

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs {

namespace {

using detail::has;
using detail::read_u16;
using detail::read_u8;
using detail::Span;
using detail::table_of;

inline constexpr u32 platform_unicode = 0;
inline constexpr u32 platform_mac = 1;
inline constexpr u32 platform_iso = 2;
inline constexpr u32 platform_windows = 3;
inline constexpr u32 mac_roman = 0;
inline constexpr u32 mac_english = 0;
inline constexpr u32 replacement = 0xFFFD;
inline constexpr usize none = ~static_cast<usize>(0);

// Mac Roman's characters from 0x80 to 0xFF (Apple's table, the euro sign at
// 0xDB).
inline constexpr u16 mac_roman_high[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7,
    0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5,
    0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9,
    0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248,
    0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D,
    0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7,
    0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD,
    0x02DB, 0x02C7,
};

// Where a record's text is, from the start of the table.
struct NameText final {
    usize offset{0};
    usize length{0};
};

// The table's layout: its records, where the text may be, and format 1's
// language tags.
struct NameLayout final {
    usize count{0};
    usize storage{0};
    usize text_start{0};
    usize tag_count{0};
};

// The layout, as FreeType checks it: the text lies after the records (and
// format 1's tags) and inside the table. False: the table is not read.
[[nodiscard]] bool layout_of(const Span& table, NameLayout& layout) noexcept {
    if (!has(table, 0, 6)) return false;
    layout.count = read_u16(table, 2);
    layout.storage = read_u16(table, 4);
    layout.text_start = 6 + layout.count * 12;
    if (layout.text_start > table.size) return false;
    if (read_u16(table, 0) == 1) {
        if (!has(table, layout.text_start, 2)) return false;
        layout.tag_count = read_u16(table, layout.text_start);
        layout.text_start = layout.text_start + 2 + layout.tag_count * 4;
        if (layout.text_start > table.size) return false;
    }
    return true;
}

// Whether a piece of text from the storage is inside where text may be.
[[nodiscard]] bool text_inside(const Span& table, const NameLayout& layout, const usize offset, const usize length) noexcept {
    const usize begin = layout.storage + offset;
    const bool result = begin >= layout.text_start && has(table, begin, length);
    return result;
}

// Whether record `index` is one FreeType keeps: text not empty and inside,
// and, in format 1, a language tag that exists and is not empty.
[[nodiscard]] bool record_kept(const Span& table, const NameLayout& layout, const usize index, NameText& text) noexcept {
    const usize record = 6 + index * 12;
    const usize length = read_u16(table, record + 8);
    const usize offset = read_u16(table, record + 10);
    if (length == 0 || !text_inside(table, layout, offset, length)) return false;
    const u32 language = read_u16(table, record + 4);
    if (read_u16(table, 0) == 1 && language >= 0x8000U) {
        const usize tag = language - 0x8000U;
        if (tag >= layout.tag_count) return false;
        const usize tag_record = 6 + layout.count * 12 + 2 + tag * 4;
        const usize tag_length = read_u16(table, tag_record);
        const usize tag_offset = read_u16(table, tag_record + 2);
        if (tag_length == 0 || !text_inside(table, layout, tag_offset, tag_length)) return false;
    }
    text = NameText{.offset = layout.storage + offset, .length = length};
    return true;
}

[[nodiscard]] bool push_byte(Vector<char>& text, const u32 byte) noexcept {
    const bool pushed = text.push_copy(static_cast<char>(static_cast<u8>(byte))).succeeded();
    return pushed;
}

// One character as UTF-8. False: out of memory.
[[nodiscard]] bool put_character(Vector<char>& text, const u32 character) noexcept {
    bool pushed = true;
    if (character < 0x80U) {
        pushed = push_byte(text, character);
    } else if (character < 0x800U) {
        pushed = push_byte(text, 0xC0U | (character >> 6)) && push_byte(text, 0x80U | (character & 0x3FU));
    } else if (character < 0x10000U) {
        pushed = push_byte(text, 0xE0U | (character >> 12)) && push_byte(text, 0x80U | ((character >> 6) & 0x3FU)) &&
                 push_byte(text, 0x80U | (character & 0x3FU));
    } else {
        pushed = push_byte(text, 0xF0U | (character >> 18)) && push_byte(text, 0x80U | ((character >> 12) & 0x3FU)) &&
                 push_byte(text, 0x80U | ((character >> 6) & 0x3FU)) && push_byte(text, 0x80U | (character & 0x3FU));
    }
    return pushed;
}

// UTF-16 (big-endian) to UTF-8, to the first U+0000; an odd last byte is
// not read; a surrogate without its other half is U+FFFD.
[[nodiscard]] bool decode_utf16(const Span& table, const NameText& place, Vector<char>& text) noexcept {
    const usize units = place.length / 2;
    usize index = 0;
    while (index < units) {
        const u32 unit = read_u16(table, place.offset + index * 2);
        index = index + 1;
        if (unit == 0) return true;
        u32 character = unit;
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            character = replacement;
            if (index < units) {
                const u32 low = read_u16(table, place.offset + index * 2);
                if (low >= 0xDC00U && low <= 0xDFFFU) {
                    character = 0x10000U + ((unit - 0xD800U) << 10) + (low - 0xDC00U);
                    index = index + 1;
                }
            }
        } else if (unit >= 0xDC00U && unit <= 0xDFFFU) {
            character = replacement;
        }
        if (!put_character(text, character)) return false;
    }
    return true;
}

// Mac Roman to UTF-8, to the first 0 byte.
[[nodiscard]] bool decode_mac_roman(const Span& table, const NameText& place, Vector<char>& text) noexcept {
    for (usize index = 0; index < place.length; ++index) {
        const u32 byte = read_u8(table, place.offset + index);
        if (byte == 0) return true;
        const u32 character = byte < 0x80U ? byte : mac_roman_high[byte - 0x80U];
        if (!put_character(text, character)) return false;
    }
    return true;
}

// FreeType's choice of record for a name: see font_file.hpp.
struct NameChoice final {
    usize windows{none};
    bool windows_english{false};
    usize mac_english_record{none};
    usize mac_roman_record{none};
    usize unicode{none};
};

void consider(const Span& table, const usize index, NameChoice& choice) noexcept {
    const usize record = 6 + index * 12;
    const u32 platform = read_u16(table, record);
    const u32 encoding = read_u16(table, record + 2);
    const u32 language = read_u16(table, record + 4);
    if (platform == platform_unicode || platform == platform_iso) {
        choice.unicode = index;
    } else if (platform == platform_mac) {
        if (language == mac_english) {
            choice.mac_english_record = index;
        } else if (encoding == mac_roman) {
            choice.mac_roman_record = index;
        }
    } else if (platform == platform_windows) {
        const bool english = (language & 0x3FFU) == 0x009U;
        const bool readable = encoding == 0 || encoding == 1 || encoding == 10;
        if ((choice.windows == none || english) && readable) {
            choice.windows = index;
            choice.windows_english = english;
        }
    }
}

} // namespace

Status font_name(const Font& font, const u32 name_id, Vector<char>& text) noexcept {
    const Operation operation("stxlib.glyphs.font_name");
    text.clear();
    const Span table = table_of(font, font.names);
    NameLayout layout{};
    if (!layout_of(table, layout)) return Status::success();
    NameChoice choice{};
    for (usize index = 0; index < layout.count; ++index) {
        NameText ignored{};
        if (read_u16(table, 6 + index * 12 + 6) != name_id || !record_kept(table, layout, index, ignored)) continue;
        consider(table, index, choice);
    }
    const usize mac = choice.mac_english_record != none ? choice.mac_english_record : choice.mac_roman_record;
    usize chosen = none;
    bool utf16 = true;
    if (choice.windows != none && !(mac != none && !choice.windows_english)) {
        chosen = choice.windows;
    } else if (mac != none) {
        chosen = mac;
        utf16 = false;
    } else {
        chosen = choice.unicode;
    }
    if (chosen == none) return Status::success();
    NameText place{};
    const bool kept = record_kept(table, layout, chosen, place);
    bool decoded = kept;
    if (kept) decoded = utf16 ? decode_utf16(table, place, text) : decode_mac_roman(table, place, text);
    if (kept && !decoded) {
        text.clear();
        const Status failed = detail::refuse_font(ErrorKind::out_of_memory, operation);
        return failed;
    }
    return Status::success();
}

namespace {

// The first of up to three names the font has.
[[nodiscard]] Status first_name(const Font& font, const u32* const ids, const usize count, Vector<char>& text) noexcept {
    for (usize index = 0; index < count; ++index) {
        const Status found = font_name(font, ids[index], text);
        if (found.failed() || !text.empty()) return found;
    }
    return Status::success();
}

[[nodiscard]] bool wws_consistent(const Font& font) noexcept {
    const bool result = font.os2_version != 0xFFFFU && (font.selection & 0x100U) != 0;
    return result;
}

} // namespace

Status family_name(const Font& font, Vector<char>& text) noexcept {
    const u32 consistent[2] = {name_typographic_family, name_family};
    const u32 otherwise[3] = {name_wws_family, name_typographic_family, name_family};
    const bool wws = wws_consistent(font);
    const Status found = wws ? first_name(font, consistent, 2, text) : first_name(font, otherwise, 3, text);
    return found;
}

Status style_name(const Font& font, Vector<char>& text) noexcept {
    const u32 consistent[2] = {name_typographic_style, name_style};
    const u32 otherwise[3] = {name_wws_style, name_typographic_style, name_style};
    const bool wws = wws_consistent(font);
    const Status found = wws ? first_name(font, consistent, 2, text) : first_name(font, otherwise, 3, text);
    return found;
}

} // namespace stxlib::glyphs
