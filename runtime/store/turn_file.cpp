// turn_file.cpp: turn files (turn_file.h).

#include "turn_file.h"

namespace stxstore {

namespace {

constexpr char turn_magic[] = "STXTURN 1 ";
constexpr std::size_t turn_magic_length = sizeof(turn_magic) - 1u;
constexpr char hex_digits[] = "0123456789abcdef";

// Appends value in decimal; returns the new size.
[[nodiscard]] std::size_t put_decimal(std::span<char> out, std::size_t at, std::uint64_t value) noexcept {
    char digits[20];
    std::size_t count = 0;
    std::uint64_t remaining = value;
    do {
        digits[count] = static_cast<char>('0' + static_cast<int>(remaining % 10u));
        remaining /= 10u;
        count += 1;
    } while (remaining != 0u);
    for (std::size_t index = 0; index < count; index += 1) {
        out[at + index] = digits[count - 1u - index];
    }
    return at + count;
}

// Reads a decimal number without leading zeros ending at a space, from at;
// advances at past the space. Nothing when malformed or above maximum.
[[nodiscard]] std::optional<std::uint64_t> get_decimal(std::span<const char> bytes, std::size_t& at,
                                                       std::uint64_t maximum) noexcept {
    const std::size_t begin = at;
    std::uint64_t value = 0;
    while (at < bytes.size() && bytes[at] >= '0' && bytes[at] <= '9') {
        // At most 20 digits are examined before the value passes maximum.
        const std::uint64_t digit = static_cast<std::uint64_t>(bytes[at] - '0');
        if (digit > maximum || value > (maximum - digit) / 10u) {
            return std::nullopt;
        }
        value = value * 10u + digit;
        at += 1;
    }
    const std::size_t length = at - begin;
    if (length == 0u || (length > 1u && bytes[begin] == '0') || at >= bytes.size() || bytes[at] != ' ') {
        return std::nullopt;
    }
    at += 1;
    const std::optional<std::uint64_t> found = value;
    return found;
}

}  // namespace

std::uint32_t crc32_of(std::span<const char> bytes) noexcept {
    std::uint32_t crc = 0xffffffffu;
    for (const char byte : bytes) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
        for (int bit = 0; bit < 8; bit += 1) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^ (0xedb88320u & mask);
        }
    }
    return crc ^ 0xffffffffu;
}

std::size_t format_turn_header(std::uint32_t sequence, std::span<const char> body,
                               std::span<char, turn_header_bytes_max> out) noexcept {
    const std::span<char> text(out.data(), out.size());
    std::size_t size = 0;
    for (std::size_t index = 0; index < turn_magic_length; index += 1) {
        text[size] = turn_magic[index];
        size += 1;
    }
    size = put_decimal(text, size, sequence);
    text[size] = ' ';
    size += 1;
    size = put_decimal(text, size, body.size());
    text[size] = ' ';
    size += 1;
    const std::uint32_t crc = crc32_of(body);
    for (int shift = 28; shift >= 0; shift -= 4) {
        text[size] = hex_digits[(crc >> static_cast<std::uint32_t>(shift)) & 0xfu];
        size += 1;
    }
    text[size] = '\n';
    size += 1;
    return size;
}

std::optional<TurnView> parse_turn_file(std::span<const char> file, std::size_t body_max) noexcept {
    if (file.size() < turn_magic_length) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < turn_magic_length; index += 1) {
        if (file[index] != turn_magic[index]) {
            return std::nullopt;
        }
    }
    std::size_t at = turn_magic_length;
    const std::optional<std::uint64_t> sequence = get_decimal(file, at, turn_sequence_max);
    if (!sequence.has_value() || *sequence == 0u) {
        return std::nullopt;
    }
    const std::optional<std::uint64_t> length = get_decimal(file, at, body_max);
    if (!length.has_value()) {
        return std::nullopt;
    }
    if (file.size() - at < 9u) {
        return std::nullopt;
    }
    std::uint32_t crc = 0;
    for (std::size_t index = 0; index < 8u; index += 1) {
        const char digit = file[at + index];
        std::uint32_t value = 0;
        if (digit >= '0' && digit <= '9') {
            value = static_cast<std::uint32_t>(digit - '0');
        } else if (digit >= 'a' && digit <= 'f') {
            value = static_cast<std::uint32_t>(digit - 'a' + 10);
        } else {
            return std::nullopt;
        }
        crc = (crc << 4u) | value;
    }
    at += 8u;
    if (file[at] != '\n') {
        return std::nullopt;
    }
    at += 1u;
    if (file.size() - at != *length) {
        return std::nullopt;
    }
    const std::span<const char> body = file.subspan(at);
    if (crc32_of(body) != crc) {
        return std::nullopt;
    }
    TurnView view{};
    view.sequence = static_cast<std::uint32_t>(*sequence);
    view.body = body;
    const std::optional<TurnView> found = view;
    return found;
}

TurnChoice choose_turns(const std::optional<TurnView>& file_0, const std::optional<TurnView>& file_1) noexcept {
    TurnChoice choice{};
    choice.newest = std::nullopt;
    choice.older = std::nullopt;
    choice.write_index = 0;
    choice.next_sequence = 1u;
    std::optional<std::uint32_t> newest_sequence;
    if (file_0.has_value() && file_1.has_value()) {
        const bool first_newer = (*file_0).sequence >= (*file_1).sequence;
        choice.newest = first_newer ? 0u : 1u;
        choice.older = first_newer ? 1u : 0u;
        newest_sequence = first_newer ? (*file_0).sequence : (*file_1).sequence;
    } else if (file_0.has_value()) {
        choice.newest = 0u;
        newest_sequence = (*file_0).sequence;
    } else if (file_1.has_value()) {
        choice.newest = 1u;
        newest_sequence = (*file_1).sequence;
    }
    if (choice.newest.has_value()) {
        choice.write_index = *choice.newest == 0u ? 1u : 0u;
    }
    if (newest_sequence.has_value()) {
        if (*newest_sequence == turn_sequence_max) {
            choice.next_sequence = std::nullopt;
        } else {
            choice.next_sequence = *newest_sequence + 1u;
        }
    }
    return choice;
}

}  // namespace stxstore
