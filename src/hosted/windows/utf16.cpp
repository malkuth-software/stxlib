// UTF-8 and UTF-16 conversion (windows/utf16.hpp), ported from stx_auxlib's
// src/unicode.cpp at b6e330a. The UTF-8 decoder trusts its input (a
// TextView, valid by construction), so it decodes without checking; the
// UTF-16 side checks pairing, as stx_auxlib's append_utf8 does.

#include "hosted/windows/utf16.hpp"

namespace stxlib::utf16 {

bool append_wide(const char* const bytes, const usize count, Growable<wchar_t>& output) noexcept {
    usize index = 0;
    while (index < count) {
        const u32 lead = static_cast<unsigned char>(bytes[index]);
        u32 value = 0;
        usize length = 1;
        if (lead < 0x80U) {
            value = lead;
        } else if (lead < 0xe0U) {
            value = lead & 0x1fU;
            length = 2;
        } else if (lead < 0xf0U) {
            value = lead & 0x0fU;
            length = 3;
        } else {
            value = lead & 0x07U;
            length = 4;
        }
        for (usize offset = 1; offset < length && index + offset < count; ++offset) {
            value = (value << 6) | (static_cast<unsigned char>(bytes[index + offset]) & 0x3fU);
        }
        index = index + length;
        if (value < 0x10000U) {
            if (!output.push(static_cast<wchar_t>(value))) return false;
        } else {
            const u32 offset = value - 0x10000U;
            if (!output.push(static_cast<wchar_t>(0xd800U + (offset >> 10)))) return false;
            if (!output.push(static_cast<wchar_t>(0xdc00U + (offset & 0x3ffU)))) return false;
        }
    }
    return true;
}

Narrowed append_narrow(const wchar_t* const units, const usize count, Growable<char>& output) noexcept {
    const usize start = output.count();
    usize index = 0;
    while (index < count) {
        u32 value = static_cast<u16>(units[index]);
        index = index + 1;
        if (value >= 0xd800U && value <= 0xdbffU) {
            if (index == count) {
                output.shrink(start);
                return Narrowed::unpaired_surrogate;
            }
            const u32 low = static_cast<u16>(units[index]);
            if (low < 0xdc00U || low > 0xdfffU) {
                output.shrink(start);
                return Narrowed::unpaired_surrogate;
            }
            index = index + 1;
            value = 0x10000U + ((value - 0xd800U) << 10) + (low - 0xdc00U);
        } else if (value >= 0xdc00U && value <= 0xdfffU) {
            output.shrink(start);
            return Narrowed::unpaired_surrogate;
        }
        char encoded[4]{};
        usize length = 0;
        if (value < 0x80U) {
            encoded[0] = static_cast<char>(value);
            length = 1;
        } else if (value < 0x800U) {
            encoded[0] = static_cast<char>(0xc0U | (value >> 6));
            encoded[1] = static_cast<char>(0x80U | (value & 0x3fU));
            length = 2;
        } else if (value < 0x10000U) {
            encoded[0] = static_cast<char>(0xe0U | (value >> 12));
            encoded[1] = static_cast<char>(0x80U | ((value >> 6) & 0x3fU));
            encoded[2] = static_cast<char>(0x80U | (value & 0x3fU));
            length = 3;
        } else {
            encoded[0] = static_cast<char>(0xf0U | (value >> 18));
            encoded[1] = static_cast<char>(0x80U | ((value >> 12) & 0x3fU));
            encoded[2] = static_cast<char>(0x80U | ((value >> 6) & 0x3fU));
            encoded[3] = static_cast<char>(0x80U | (value & 0x3fU));
            length = 4;
        }
        if (!output.append(encoded, length)) {
            output.shrink(start);
            return Narrowed::out_of_memory;
        }
    }
    return Narrowed::ok;
}

bool terminate(Growable<wchar_t>& output) noexcept {
    const bool result = output.push(L'\0');
    return result;
}

} // namespace stxlib::utf16
