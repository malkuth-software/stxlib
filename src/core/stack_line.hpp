#pragma once

// One line of a written stack trace (stack_trace.hpp), formatted into a
// fixed buffer. Pure: no host call, no allocation, every write inside the
// output view. In proof scope (tests/platform_rest_exhaustive.cpp;
// proofs/esbmc/platform_rest.cpp): for every capacity the line takes at
// most that many bytes, ends with '\n' when the capacity is not zero, and
// holds no control byte but that newline.

#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::stack_line {

struct Frame final {
    usize index{0};
    uptr address{0};
    // The image's name (any bytes; control bytes are written as '?'), and
    // whether the image is known at all ("?" when not).
    View<char> image{};
    bool image_known{false};
    uptr image_offset{0};
    // The symbol's name; none when empty.
    View<char> symbol{};
    uptr symbol_offset{0};
};

namespace detail {

// Appends while there is room, keeping the last byte for the newline.
class LineWriter final {
public:
    explicit LineWriter(const MutableView<char> output) noexcept : output_(output) {}

    void put(const char byte) noexcept {
        if (output_.count() == 0 || count_ >= output_.count() - 1) return;
        output_[count_] = byte;
        count_ = count_ + 1;
    }
    void put_text(const char* const text) noexcept {
        for (usize index = 0; text[index] != '\0'; ++index) put(text[index]);
    }
    void put_name(const View<char> name) noexcept {
        for (usize index = 0; index < name.count(); ++index) {
            const unsigned char byte = static_cast<unsigned char>(name.data()[index]);
            put(byte < 0x20 || byte == 0x7f ? '?' : static_cast<char>(byte));
        }
    }
    void put_decimal(const usize value) noexcept {
        char digits[20]{};
        usize count = 0;
        usize rest = value;
        do {
            digits[count] = static_cast<char>('0' + static_cast<int>(rest % 10));
            count = count + 1;
            rest = rest / 10;
        } while (rest != 0);
        while (count > 0) {
            count = count - 1;
            put(digits[count]);
        }
    }
    // `width` 0: the fewest digits (at least one).
    void put_hex(const uptr value, const usize width) noexcept {
        constexpr char hex[] = "0123456789abcdef";
        char digits[16]{};
        usize count = 0;
        uptr rest = value;
        do {
            digits[count] = hex[rest & 0xf];
            count = count + 1;
            rest = rest >> 4;
        } while (rest != 0 && count < 16);
        while (count < width && count < 16) {
            digits[count] = '0';
            count = count + 1;
        }
        while (count > 0) {
            count = count - 1;
            put(digits[count]);
        }
    }
    // Ends the line; returns its length.
    [[nodiscard]] usize finish() noexcept {
        if (output_.count() == 0) return 0;
        output_[count_] = '\n';
        count_ = count_ + 1;
        return count_;
    }

private:
    MutableView<char> output_;
    usize count_{0};
};

} // namespace detail

// Writes "  #<index> 0x<address> <image>+0x<offset>[ <symbol>+0x<offset>]\n"
// into `output`, cut to its capacity with the newline kept; returns the
// bytes written (0 only for an empty output).
[[nodiscard]] inline usize format(const Frame& frame, const MutableView<char> output) noexcept {
    detail::LineWriter writer(output);
    writer.put_text("  #");
    writer.put_decimal(frame.index);
    writer.put_text(" 0x");
    writer.put_hex(frame.address, 16);
    writer.put(' ');
    if (frame.image_known) {
        writer.put_name(frame.image);
        writer.put_text("+0x");
        writer.put_hex(frame.image_offset, 0);
    } else {
        writer.put('?');
    }
    if (!frame.symbol.empty()) {
        writer.put(' ');
        writer.put_name(frame.symbol);
        writer.put_text("+0x");
        writer.put_hex(frame.symbol_offset, 0);
    }
    const usize result = writer.finish();
    return result;
}

} // namespace stxlib::stack_line
