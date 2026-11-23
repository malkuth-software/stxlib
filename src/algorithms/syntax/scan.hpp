#pragma once

// What stxlib::syntax's engines share (engine.cpp for the C-like family, css.cpp, markup.cpp):
// the scan over one line, the LineState packing and the small byte helpers.
//
// A LineState's low four bits are a mode and the next four a rule; the 56 bits above are a
// payload whose meaning belongs to the engine and the mode. Zero is always "nothing open".
//
// Each engine provides one function that lexes the next token at the scan's position. It
// returns false when the line has no more; a token of length zero is not written (a byte
// with no kind). The function must leave the scan at a token boundary with `state` being
// the state there, so that lexing can stop after any token and resume.

#include "grammar.hpp"

namespace stxlib::syntax::detail {

struct Scan final {
    const Grammar& grammar;
    const char* text;
    u32 count;
    u32 position;
    u64 state;
    // What the last word asks of the next token (the C-like engine).
    u8 expect;
    // The line has had a token.
    bool started;
    // The last token can end a value (a name, a literal, a closing bracket).
    bool value_before;
};

[[nodiscard]] constexpr u64 pack_state(const u8 mode, const u8 rule, const u64 payload) noexcept {
    return static_cast<u64>(mode & 15u) | (static_cast<u64>(rule & 15u) << 4) | (payload << 8);
}
[[nodiscard]] constexpr u8 state_mode(const u64 state) noexcept { return static_cast<u8>(state & 15u); }
[[nodiscard]] constexpr u8 state_rule(const u64 state) noexcept { return static_cast<u8>((state >> 4) & 15u); }
[[nodiscard]] constexpr u64 state_payload(const u64 state) noexcept { return state >> 8; }

[[nodiscard]] inline ByteClass class_at(const Scan& scan, const u32 index) noexcept {
    return scan.grammar.classes[static_cast<u8>(scan.text[index])];
}

[[nodiscard]] inline bool starts_with(const Scan& scan, const u32 index, const TextView text) noexcept {
    const usize length = text.count();
    if (length == 0 || scan.count - index < length) return false;
    for (usize k = 0; k < length; ++k) {
        if (scan.text[index + k] != text.data()[k]) return false;
    }
    return true;
}

[[nodiscard]] constexpr char folded(const char byte) noexcept {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : byte;
}

// Whether the bytes [index, index + text.count()) are `text` without regard to ASCII case
// (`text` in lower case).
[[nodiscard]] inline bool starts_with_folded(const Scan& scan, const u32 index, const TextView text) noexcept {
    const usize length = text.count();
    if (length == 0 || scan.count - index < length) return false;
    for (usize k = 0; k < length; ++k) {
        if (folded(scan.text[index + k]) != text.data()[k]) return false;
    }
    return true;
}

[[nodiscard]] inline u32 skip_spaces(const Scan& scan, u32 index) noexcept {
    while (index < scan.count && class_at(scan, index) == ByteClass::space) ++index;
    return index;
}

[[nodiscard]] constexpr bool is_digit(const char byte) noexcept { return byte >= '0' && byte <= '9'; }
[[nodiscard]] constexpr bool is_hex_digit(const char byte) noexcept {
    return is_digit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F');
}

inline void emit(LineToken& token, const u32 start, const u32 end, const TokenKind kind) noexcept {
    token.column = start;
    token.length = end - start;
    token.kind = kind;
}

// Emits [start, end) and moves the scan to `end`.
inline void take(Scan& scan, LineToken& token, const u32 start, const u32 end, const TokenKind kind) noexcept {
    emit(token, start, end, kind);
    scan.position = end;
}

[[nodiscard]] inline const WordSlot* find_word(const Grammar& grammar, const char* const text, const usize length, const u32 hash) noexcept {
    if (grammar.slots == nullptr || length > 255) return nullptr;
    usize slot = hash & grammar.slot_mask;
    for (;;) {
        const WordSlot& candidate = grammar.slots[slot];
        if (candidate.length == 0) return nullptr;
        if (candidate.length == length) {
            bool same = true;
            for (usize k = 0; k < length && same; ++k) {
                const char byte = grammar.spec.fold_case ? folded(text[k]) : text[k];
                same = byte == candidate.text[k];
            }
            if (same) return &candidate;
        }
        slot = (slot + 1) & grammar.slot_mask;
    }
}

// The word of the grammar's table that is the bytes [start, end), or null.
[[nodiscard]] inline const WordSlot* find_word_at(const Scan& scan, const u32 start, const u32 end) noexcept {
    u32 hash = word_hash_seed;
    for (u32 k = start; k < end; ++k) {
        hash = word_hash_step(hash, static_cast<u8>(scan.grammar.spec.fold_case ? folded(scan.text[k]) : scan.text[k]));
    }
    return find_word(scan.grammar, scan.text + start, end - start, hash);
}

// The engines' token functions.
[[nodiscard]] bool c_like_token(Scan& scan, LineToken& token) noexcept;
[[nodiscard]] bool css_token(Scan& scan, LineToken& token) noexcept;
[[nodiscard]] bool markup_token(Scan& scan, LineToken& token) noexcept;

} // namespace stxlib::syntax::detail
