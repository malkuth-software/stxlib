#pragma once

// WTF-8 and the host-name escapes behind Path's lossless host names (owner,
// D13 option a, notes decisions/2026-11-13-d13-files-paths.md: "Carry
// losslessly").
//
// WTF-8 (Simon Sapin, "The WTF-8 encoding", simonsapin.github.io/wtf-8, read
// from memory of the published text, not re-read this session) is UTF-8
// generalized to every code point U+0000..U+10FFFF including the surrogates
// U+D800..U+DFFF, each encoded as UTF-8's 3-byte form would (ED A0..BF xx),
// with one restriction: a high surrogate (D800..DBFF) is never immediately
// followed by a low surrogate (DC00..DFFF), since that pair is encoded as the
// 4-byte form of its supplementary code point. It represents every sequence
// of 16-bit units losslessly, including Windows names with unpaired
// surrogates (NTFS stores a name as 16-bit units without checking pairing;
// the WTF-8 text names this as its motivation). Valid UTF-8 is WTF-8.
//
// POSIX names are bytes: any byte but '/' and NUL (POSIX.1-2017, Base
// Definitions 3.170 "Filename"; from memory, not re-read). Path carries a
// byte that is not part of a well-formed UTF-8 sequence as the surrogate
// U+DC00 + byte, U+DC80..U+DCFF (the scheme of Python's surrogateescape,
// PEP 383), and also ':' and '\' (U+DC3A, U+DC5C), the two characters Path
// refuses in its own syntax, so that a POSIX name holding them is reachable
// and a name like "c:" is not read as a drive. Valid UTF-8 never encodes a
// surrogate, so an escape cannot collide with a real character.
//
// Losslessness, by construction:
// - escape_posix is a function of its input (a greedy left-to-right decode),
//   and unescape_posix inverts it: copied sequences are copied back and each
//   escape yields its byte. So unescape(escape(s)) = s for every s.
// - unescape_posix accepts only text t with escape(unescape(t)) = t (it
//   re-escapes and compares), so it is injective on what it accepts: two
//   Paths never name the same POSIX file.
// - from_utf16 and to_utf16 are the WTF-8 text's bijection between 16-bit
//   unit sequences and well-formed WTF-8.
// tests/path_names_exhaustive.cpp checks the round trips over every POSIX
// name up to three bytes and the Windows unit sequences it lists.

#include "core/growable.hpp"
#include "stxlib/detail/utf8.hpp"
#include "stxlib/types.hpp"

namespace stxlib::wtf8 {

[[nodiscard]] inline bool is_surrogate(const u32 code_point) noexcept {
    const bool result = code_point >= 0xd800U && code_point <= 0xdfffU;
    return result;
}

// The length of the generalized UTF-8 sequence at bytes[index] (UTF-8 plus
// the surrogates' ED A0..BF 80..BF), or 0. Requirement: index < count.
[[nodiscard]] inline usize sequence_length(const char* const bytes, const usize count, const usize index) noexcept {
    const u8 first = static_cast<u8>(bytes[index]);
    if (first == 0xed) {
        if (count - index < 3) return 0;
        const u8 second = static_cast<u8>(bytes[index + 1]);
        const u8 third = static_cast<u8>(bytes[index + 2]);
        if (second < 0x80 || second > 0xbf || third < 0x80 || third > 0xbf) return 0;
        return 3;
    }
    const usize result = detail::utf8_sequence_length(bytes, count, index);
    return result;
}

// The code point of a sequence sequence_length accepted.
[[nodiscard]] inline u32 decode(const char* const bytes, const usize index, const usize length) noexcept {
    const u32 first = static_cast<u8>(bytes[index]);
    if (length == 1) return first;
    u32 value = length == 2 ? (first & 0x1fU) : (length == 3 ? (first & 0x0fU) : (first & 0x07U));
    for (usize offset = 1; offset < length; ++offset) {
        value = (value << 6) | (static_cast<u8>(bytes[index + offset]) & 0x3fU);
    }
    return value;
}

// Writes code_point (at most U+10FFFF; surrogates allowed) to output[0, 4)
// and returns the byte count.
[[nodiscard]] inline usize encode(const u32 code_point, char* const output) noexcept {
    if (code_point < 0x80U) {
        output[0] = static_cast<char>(code_point);
        return 1;
    }
    if (code_point < 0x800U) {
        output[0] = static_cast<char>(0xc0U | (code_point >> 6));
        output[1] = static_cast<char>(0x80U | (code_point & 0x3fU));
        return 2;
    }
    if (code_point < 0x10000U) {
        output[0] = static_cast<char>(0xe0U | (code_point >> 12));
        output[1] = static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU));
        output[2] = static_cast<char>(0x80U | (code_point & 0x3fU));
        return 3;
    }
    output[0] = static_cast<char>(0xf0U | (code_point >> 18));
    output[1] = static_cast<char>(0x80U | ((code_point >> 12) & 0x3fU));
    output[2] = static_cast<char>(0x80U | ((code_point >> 6) & 0x3fU));
    output[3] = static_cast<char>(0x80U | (code_point & 0x3fU));
    return 4;
}

// Well-formed WTF-8: generalized UTF-8 throughout, and no high surrogate
// immediately followed by a low one.
[[nodiscard]] inline bool well_formed(const char* const bytes, const usize count) noexcept {
    usize index = 0;
    bool after_high = false;
    while (index < count) {
        const usize length = sequence_length(bytes, count, index);
        if (length == 0) return false;
        const u32 code_point = decode(bytes, index, length);
        if (after_high && code_point >= 0xdc00U && code_point <= 0xdfffU) return false;
        after_high = code_point >= 0xd800U && code_point <= 0xdbffU;
        index = index + length;
    }
    return true;
}

// For well-formed WTF-8: whether it holds a surrogate (it is then not UTF-8).
[[nodiscard]] inline bool has_surrogate(const char* const bytes, const usize count) noexcept {
    for (usize index = 0; index + 1 < count; ++index) {
        // In well-formed WTF-8, ED followed by A0..BF only starts a surrogate.
        if (static_cast<u8>(bytes[index]) == 0xed && static_cast<u8>(bytes[index + 1]) >= 0xa0) return true;
    }
    return false;
}

// Appends the POSIX host bytes[0, count) as WTF-8 with escapes. False when
// memory runs out.
[[nodiscard]] inline bool escape_posix(const char* const bytes, const usize count, Growable<char>& output) noexcept {
    usize index = 0;
    while (index < count) {
        const usize length = detail::utf8_sequence_length(bytes, count, index);
        const char first = bytes[index];
        if (length == 0 || (length == 1 && (first == ':' || first == '\\'))) {
            char escaped[4]{};
            const usize written = encode(0xdc00U + static_cast<u8>(first), escaped);
            if (!output.append(escaped, written)) return false;
            index = index + 1;
            continue;
        }
        if (!output.append(bytes + index, length)) return false;
        index = index + length;
    }
    return true;
}

enum class Converted : u8 {
    ok = 0,
    not_representable = 1,
    out_of_memory = 2,
};

// Appends the POSIX host bytes that well-formed WTF-8 text[0, count) names:
// each escape U+DC00..U+DCFF becomes its byte. not_representable for any
// other surrogate, or for text that is not exactly what escape_posix makes of
// those bytes (an escape of a byte that needs none, or escaped bytes that
// together form UTF-8). On failure `output` is unchanged.
[[nodiscard]] inline Converted unescape_posix(const char* const text, const usize count, Growable<char>& output) noexcept {
    const usize start = output.count();
    usize index = 0;
    while (index < count) {
        const usize length = sequence_length(text, count, index);
        if (length == 0) {
            output.shrink(start);
            return Converted::not_representable;
        }
        const u32 code_point = decode(text, index, length);
        if (is_surrogate(code_point)) {
            if (code_point < 0xdc00U || code_point > 0xdcffU) {
                output.shrink(start);
                return Converted::not_representable;
            }
            if (!output.push(static_cast<char>(code_point - 0xdc00U))) {
                output.shrink(start);
                return Converted::out_of_memory;
            }
        } else if (!output.append(text + index, length)) {
            output.shrink(start);
            return Converted::out_of_memory;
        }
        index = index + length;
    }
    // The canonical check: escaping the bytes again must give the text back.
    Growable<char> again;
    if (!escape_posix(output.data() + start, output.count() - start, again)) {
        output.shrink(start);
        return Converted::out_of_memory;
    }
    bool same = again.count() == count;
    for (usize position = 0; same && position < count; ++position) same = again[position] == text[position];
    if (!same) {
        output.shrink(start);
        return Converted::not_representable;
    }
    return Converted::ok;
}

// Appends 16-bit units (u16, or Windows' wchar_t) as WTF-8: a high
// surrogate followed by a low one is one supplementary code point; any other
// surrogate is encoded alone. False when memory runs out.
template <typename Unit>
[[nodiscard]] bool from_utf16(const Unit* const units, const usize count, Growable<char>& output) noexcept {
    static_assert(sizeof(Unit) == 2, "UTF-16 units are 16 bits");
    usize index = 0;
    while (index < count) {
        u32 code_point = static_cast<u16>(units[index]);
        index = index + 1;
        if (code_point >= 0xd800U && code_point <= 0xdbffU && index < count) {
            const u32 low = static_cast<u16>(units[index]);
            if (low >= 0xdc00U && low <= 0xdfffU) {
                code_point = 0x10000U + ((code_point - 0xd800U) << 10) + (low - 0xdc00U);
                index = index + 1;
            }
        }
        char encoded[4]{};
        const usize written = encode(code_point, encoded);
        if (!output.append(encoded, written)) return false;
    }
    return true;
}

// Appends well-formed WTF-8 text as 16-bit units. False when memory runs out
// or the text is not well-formed (a caller holding a Path never sees that).
template <typename Unit>
[[nodiscard]] bool to_utf16(const char* const text, const usize count, Growable<Unit>& output) noexcept {
    static_assert(sizeof(Unit) == 2, "UTF-16 units are 16 bits");
    usize index = 0;
    while (index < count) {
        const usize length = sequence_length(text, count, index);
        if (length == 0) return false;
        const u32 code_point = decode(text, index, length);
        index = index + length;
        if (code_point < 0x10000U) {
            if (!output.push(static_cast<Unit>(code_point))) return false;
            continue;
        }
        const u32 offset = code_point - 0x10000U;
        if (!output.push(static_cast<Unit>(0xd800U + (offset >> 10)))) return false;
        if (!output.push(static_cast<Unit>(0xdc00U + (offset & 0x3ffU)))) return false;
    }
    return true;
}

} // namespace stxlib::wtf8
