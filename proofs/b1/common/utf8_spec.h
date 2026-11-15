/* Reference functions for the UTF-8 and UTF-16 rows (03, 04, 05), in C so
 * a C++ harness and any later C harness share one specification.
 *
 * Written from RFC 3629 section 4 (the UTF8-octets grammar) and RFC 2781
 * section 2 (UTF-16), not from any implementation under test. Each
 * function states its contract; all are total on their stated inputs.
 */
#ifndef STX_B1_UTF8_SPEC_H
#define STX_B1_UTF8_SPEC_H

#if defined(__cplusplus)
#include <cstddef>
#include <cstdint>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif

/* One scalar value at bytes[index], index < length: its value and width in
 * bytes, or false when the bytes there do not start an RFC 3629 UTF8-char.
 * RFC 3629 section 4:
 *   UTF8-1 = %x00-7F
 *   UTF8-2 = %xC2-DF UTF8-tail
 *   UTF8-3 = %xE0 %xA0-BF UTF8-tail / %xE1-EC 2( UTF8-tail ) /
 *            %xED %x80-9F UTF8-tail / %xEE-EF 2( UTF8-tail )
 *   UTF8-4 = %xF0 %x90-BF 2( UTF8-tail ) / %xF1-F3 3( UTF8-tail ) /
 *            %xF4 %x80-8F 2( UTF8-tail )
 *   UTF8-tail = %x80-BF */
static inline bool stx_spec_scalar_at(const unsigned char *bytes, size_t length, size_t index,
                                      uint32_t *value, size_t *width)
{
    const unsigned char lead = bytes[index];
    size_t needed = 0;
    unsigned char second_low = 0x80;
    unsigned char second_high = 0xbf;
    uint32_t scalar = 0;
    if (lead <= 0x7f) {
        *value = lead;
        *width = 1;
        return true;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        needed = 2;
        scalar = lead & 0x1fu;
    } else if (lead == 0xe0) {
        needed = 3;
        second_low = 0xa0;
        scalar = lead & 0x0fu;
    } else if ((lead >= 0xe1 && lead <= 0xec) || lead == 0xee || lead == 0xef) {
        needed = 3;
        scalar = lead & 0x0fu;
    } else if (lead == 0xed) {
        needed = 3;
        second_high = 0x9f;
        scalar = lead & 0x0fu;
    } else if (lead == 0xf0) {
        needed = 4;
        second_low = 0x90;
        scalar = lead & 0x07u;
    } else if (lead >= 0xf1 && lead <= 0xf3) {
        needed = 4;
        scalar = lead & 0x07u;
    } else if (lead == 0xf4) {
        needed = 4;
        second_high = 0x8f;
        scalar = lead & 0x07u;
    } else {
        return false;
    }
    if (length - index < needed) return false;
    if (bytes[index + 1] < second_low || bytes[index + 1] > second_high) return false;
    for (size_t offset = 1; offset < needed; ++offset) {
        const unsigned char tail = bytes[index + offset];
        if (tail < 0x80 || tail > 0xbf) return false;
        scalar = (scalar << 6) | (uint32_t)(tail & 0x3fu);
    }
    *value = scalar;
    *width = needed;
    return true;
}

/* True when bytes[0, length) is a sequence of RFC 3629 UTF8-chars. */
static inline bool stx_spec_valid_utf8(const unsigned char *bytes, size_t length)
{
    size_t index = 0;
    while (index < length) {
        uint32_t value = 0;
        size_t width = 0;
        if (!stx_spec_scalar_at(bytes, length, index, &value, &width)) return false;
        index += width;
    }
    return true;
}

/* UTF-8 to UTF-16. `units` holds at least `length` code units (UTF-16 never
 * needs more units than UTF-8 has bytes). False, with *count unspecified,
 * when the input is not valid UTF-8. */
static inline bool stx_spec_utf8_to_utf16(const unsigned char *bytes, size_t length, uint16_t *units,
                                          size_t *count)
{
    size_t index = 0;
    size_t written = 0;
    while (index < length) {
        uint32_t value = 0;
        size_t width = 0;
        if (!stx_spec_scalar_at(bytes, length, index, &value, &width)) return false;
        index += width;
        if (value < 0x10000u) {
            units[written] = (uint16_t)value;
            written += 1;
        } else {
            const uint32_t offset = value - 0x10000u;
            units[written] = (uint16_t)(0xd800u + (offset >> 10));
            units[written + 1] = (uint16_t)(0xdc00u + (offset & 0x3ffu));
            written += 2;
        }
    }
    *count = written;
    return true;
}

/* UTF-16 to UTF-8. `bytes` holds at least 3 * count bytes (a unit gives at
 * most 3 bytes; a surrogate pair gives 4 for 2 units). False on an
 * unpaired surrogate. */
static inline bool stx_spec_utf16_to_utf8(const uint16_t *units, size_t count, unsigned char *bytes,
                                          size_t *length)
{
    size_t index = 0;
    size_t written = 0;
    while (index < count) {
        uint32_t value = units[index];
        index += 1;
        if (value >= 0xd800u && value <= 0xdbffu) {
            if (index == count) return false;
            if (units[index] < 0xdc00u || units[index] > 0xdfffu) return false;
            value = 0x10000u + ((value - 0xd800u) << 10) + ((uint32_t)units[index] - 0xdc00u);
            index += 1;
        } else if (value >= 0xdc00u && value <= 0xdfffu) {
            return false;
        }
        if (value < 0x80u) {
            bytes[written] = (unsigned char)value;
            written += 1;
        } else if (value < 0x800u) {
            bytes[written] = (unsigned char)(0xc0u | (value >> 6));
            bytes[written + 1] = (unsigned char)(0x80u | (value & 0x3fu));
            written += 2;
        } else if (value < 0x10000u) {
            bytes[written] = (unsigned char)(0xe0u | (value >> 12));
            bytes[written + 1] = (unsigned char)(0x80u | ((value >> 6) & 0x3fu));
            bytes[written + 2] = (unsigned char)(0x80u | (value & 0x3fu));
            written += 3;
        } else {
            bytes[written] = (unsigned char)(0xf0u | (value >> 18));
            bytes[written + 1] = (unsigned char)(0x80u | ((value >> 12) & 0x3fu));
            bytes[written + 2] = (unsigned char)(0x80u | ((value >> 6) & 0x3fu));
            bytes[written + 3] = (unsigned char)(0x80u | (value & 0x3fu));
            written += 4;
        }
    }
    *length = written;
    return true;
}

#endif /* STX_B1_UTF8_SPEC_H */
