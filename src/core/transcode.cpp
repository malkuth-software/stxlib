// Public transcoding (include/stxlib/transcode.hpp; roadmap M2, R15).
//
// Every conversion has two passes: a check-and-count pass that writes
// nothing, then a write pass into storage of exactly the counted size. So a
// refused allocation, a short output or a bad unit leaves every output as it
// was. Byte counts cannot overflow: an input object is at most PTRDIFF_MAX
// bytes, and no conversion here more than doubles the byte size (UTF-16 to
// UTF-8 is at most 3 bytes for a 2-byte unit; UTF-32 to UTF-8 at most 4 for
// 4; UTF-8 to UTF-16 or UTF-32 at most one unit per byte).

#include "stxlib/transcode.hpp"

#include "core/wtf8.hpp"
#include "stxlib/detail/utf8.hpp"
#include "stxlib/require.hpp"

namespace stxlib {

namespace {

constexpr u32 high_first = 0xd800U;
constexpr u32 high_last = 0xdbffU;
constexpr u32 low_first = 0xdc00U;
constexpr u32 low_last = 0xdfffU;

u32 saturated_index(const usize index) noexcept {
    const u32 result = index > static_cast<usize>(u32_max) ? u32_max : static_cast<u32>(index);
    return result;
}

ErrorRecord bad_unit(const Operation operation, const usize index, const TextView message) noexcept {
    const ErrorRecord result =
        ErrorRecord::make(ErrorKind::invalid_argument, operation,
                          ErrorDetail{.code = saturated_index(index), .message = message});
    return result;
}

ErrorRecord output_too_short(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                                                 ErrorDetail{.message = "the output is shorter than the conversion"});
    return result;
}

ErrorRecord text_too_long(const Operation operation) noexcept {
    const ErrorRecord result = ErrorRecord::make(ErrorKind::limit_exceeded, operation,
                                                 ErrorDetail{.message = "the text length is not representable"});
    return result;
}

bool is_high(const u32 unit) noexcept { return unit >= high_first && unit <= high_last; }
bool is_low(const u32 unit) noexcept { return unit >= low_first && unit <= low_last; }

u32 combined(const u32 high, const u32 low) noexcept {
    const u32 result = 0x10000U + ((high - high_first) << 10) + (low - low_first);
    return result;
}

usize utf8_length_of(const u32 code_point) noexcept {
    if (code_point < 0x80U) return 1;
    if (code_point < 0x800U) return 2;
    if (code_point < 0x10000U) return 3;
    return 4;
}

// Walks well-formed UTF-8 text one code point at a time.
struct TextCursor final {
    const char* bytes;
    usize count;
    usize index;

    [[nodiscard]] bool more() const noexcept { return index < count; }
    [[nodiscard]] u32 next() noexcept {
        const usize length = detail::utf8_sequence_length(bytes, count, index);
        STXLIB_REQUIRE(length != 0, "transcode: a TextView that is not well-formed UTF-8");
        const u32 result = wtf8::decode(bytes, index, length);
        index = index + length;
        return result;
    }
};

// Reads 16-bit units one code point at a time; an unpaired surrogate is
// returned as itself, for the caller to accept (WTF-8) or refuse (UTF-8).
struct UnitCursor final {
    const u16* units;
    usize count;
    usize index;

    [[nodiscard]] bool more() const noexcept { return index < count; }
    [[nodiscard]] u32 next() noexcept {
        u32 result = units[index];
        index = index + 1;
        if (is_high(result) && index < count && is_low(units[index])) {
            result = combined(result, units[index]);
            index = index + 1;
        }
        return result;
    }
};

// The UTF-8 byte count of 16-bit units, or the index of the first unpaired
// surrogate when `refuse_surrogates`.
struct UnitsMeasure final {
    bool well_formed;
    usize bytes;
    usize bad_index;
};

UnitsMeasure measure_units(const View<u16> units, const bool refuse_surrogates) noexcept {
    UnitCursor cursor{units.data(), units.count(), 0};
    usize bytes = 0;
    while (cursor.more()) {
        const usize at = cursor.index;
        const u32 code_point = cursor.next();
        if (refuse_surrogates && wtf8::is_surrogate(code_point)) {
            const UnitsMeasure refused{false, 0, at};
            return refused;
        }
        bytes = bytes + utf8_length_of(code_point);
    }
    const UnitsMeasure result{true, bytes, 0};
    return result;
}

// Writes the code points of checked units as UTF-8 (or WTF-8) to output,
// which has room for exactly what measure_units counted.
void write_units_as_utf8(const View<u16> units, char* const output) noexcept {
    UnitCursor cursor{units.data(), units.count(), 0};
    usize written = 0;
    while (cursor.more()) {
        const u32 code_point = cursor.next();
        written = written + wtf8::encode(code_point, output + written);
    }
}

bool is_scalar(const u32 code_point) noexcept {
    const bool result = code_point <= 0x10ffffU && !wtf8::is_surrogate(code_point);
    return result;
}

// Appends checked, counted UTF-8 bytes produced by `produce` to `text`
// through a local buffer, in pieces that end on code point boundaries.
// `text` has already been reserved for them, so no append allocates.
template <typename Producer>
OperationStatus append_produced(Utf8String& text, Producer& produce) noexcept {
    const usize start = text.count();
    char piece[256];
    usize filled = 0;
    while (produce.more()) {
        if (filled + 4 > sizeof(piece)) {
            const OperationStatus appended = text.append(detail::TextViewAccess::adopt_validated(piece, filled));
            if (appended.failed()) {
                const OperationStatus restored = text.truncate(start);
                STXLIB_REQUIRE(restored.succeeded(), "transcode: restoring the text failed");
                return appended;
            }
            filled = 0;
        }
        filled = filled + wtf8::encode(produce.next(), piece + filled);
    }
    if (filled != 0) {
        const OperationStatus appended = text.append(detail::TextViewAccess::adopt_validated(piece, filled));
        if (appended.failed()) {
            const OperationStatus restored = text.truncate(start);
            STXLIB_REQUIRE(restored.succeeded(), "transcode: restoring the text failed");
            return appended;
        }
    }
    return OperationStatus::success();
}

struct ScalarCursor final {
    const u32* values;
    usize count;
    usize index;

    [[nodiscard]] bool more() const noexcept { return index < count; }
    [[nodiscard]] u32 next() noexcept {
        const u32 result = values[index];
        index = index + 1;
        return result;
    }
};

OperationStatus reserve_more(Utf8String& text, const usize bytes, const Operation operation) noexcept {
    if (bytes > usize_max - 1 - text.count()) return OperationStatus::failure(text_too_long(operation));
    const OperationStatus result = text.reserve(text.count() + bytes);
    return result;
}

// Well-formed WTF-8 up to the returned offset; the offset equals the count
// exactly when the whole input is well-formed. Also counts the 16-bit units.
struct Wtf8Measure final {
    usize stopped;
    usize units;
};

Wtf8Measure measure_wtf8(const char* const bytes, const usize count) noexcept {
    usize index = 0;
    usize units = 0;
    bool after_high = false;
    while (index < count) {
        const usize length = wtf8::sequence_length(bytes, count, index);
        if (length == 0) break;
        const u32 code_point = wtf8::decode(bytes, index, length);
        if (after_high && is_low(code_point)) break;
        after_high = is_high(code_point);
        const usize needed = code_point >= 0x10000U ? usize{2} : usize{1};
        units = units + needed;
        index = index + length;
    }
    const Wtf8Measure result{index, units};
    return result;
}

} // namespace

// ---- code points ------------------------------------------------------------

usize code_point_count(const TextView text) noexcept {
    usize result = 0;
    const char* const bytes = text.data();
    for (usize index = 0; index < text.count(); ++index) {
        if ((static_cast<u8>(bytes[index]) & 0xc0U) != 0x80U) result = result + 1;
    }
    return result;
}

OperationResult<DecodedCodePoint> code_point_at(const TextView text, const usize offset) noexcept {
    if (offset >= text.count()) {
        const ErrorRecord refused = ErrorRecord::make(ErrorKind::out_of_range, "stxlib.transcode.code_point_at",
                                                      ErrorDetail{.message = "the offset is not inside the text"});
        return OperationResult<DecodedCodePoint>::failure(refused);
    }
    const char* const bytes = text.data();
    if ((static_cast<u8>(bytes[offset]) & 0xc0U) == 0x80U) {
        const ErrorRecord refused = bad_unit("stxlib.transcode.code_point_at", offset,
                                             "the offset is not on a code point boundary");
        return OperationResult<DecodedCodePoint>::failure(refused);
    }
    const usize length = detail::utf8_sequence_length(bytes, text.count(), offset);
    STXLIB_REQUIRE(length != 0, "code_point_at: a TextView that is not well-formed UTF-8");
    const DecodedCodePoint decoded{wtf8::decode(bytes, offset, length), static_cast<u32>(length)};
    return OperationResult<DecodedCodePoint>::success(decoded);
}

// ---- UTF-16 -----------------------------------------------------------------

usize utf16_count(const TextView text) noexcept {
    usize result = 0;
    const char* const bytes = text.data();
    for (usize index = 0; index < text.count(); ++index) {
        const u8 byte = static_cast<u8>(bytes[index]);
        if ((byte & 0xc0U) == 0x80U) continue;
        // A 4-byte sequence (lead F0..F4) is a supplementary code point.
        const usize needed = byte >= 0xf0U ? usize{2} : usize{1};
        result = result + needed;
    }
    return result;
}

OperationResult<usize> utf8_count_of_utf16(const View<u16> units) noexcept {
    const UnitsMeasure measured = measure_units(units, true);
    if (!measured.well_formed) {
        return OperationResult<usize>::failure(
            bad_unit("stxlib.transcode.utf8_count_of_utf16", measured.bad_index, "an unpaired surrogate"));
    }
    return OperationResult<usize>::success(measured.bytes);
}

namespace {

void write_text_as_utf16(const TextView text, u16* const output) noexcept {
    TextCursor cursor{text.data(), text.count(), 0};
    usize written = 0;
    while (cursor.more()) {
        const u32 code_point = cursor.next();
        if (code_point < 0x10000U) {
            output[written] = static_cast<u16>(code_point);
            written = written + 1;
            continue;
        }
        const u32 offset = code_point - 0x10000U;
        output[written] = static_cast<u16>(high_first + (offset >> 10));
        output[written + 1] = static_cast<u16>(low_first + (offset & 0x3ffU));
        written = written + 2;
    }
}

} // namespace

OperationResult<usize> write_utf16(const TextView text, const MutableView<u16> output) noexcept {
    const usize needed = utf16_count(text);
    if (needed > output.count()) return OperationResult<usize>::failure(output_too_short("stxlib.transcode.write_utf16"));
    write_text_as_utf16(text, output.data());
    return OperationResult<usize>::success(needed);
}

OperationResult<usize> write_utf8_of_utf16(const View<u16> units, const MutableView<char> output) noexcept {
    const UnitsMeasure measured = measure_units(units, true);
    if (!measured.well_formed) {
        return OperationResult<usize>::failure(
            bad_unit("stxlib.transcode.write_utf8_of_utf16", measured.bad_index, "an unpaired surrogate"));
    }
    if (measured.bytes > output.count()) {
        return OperationResult<usize>::failure(output_too_short("stxlib.transcode.write_utf8_of_utf16"));
    }
    write_units_as_utf8(units, output.data());
    return OperationResult<usize>::success(measured.bytes);
}

OperationResult<Vector<u16>> utf16_of(const TextView text) noexcept {
    const usize needed = utf16_count(text);
    OperationResult<Vector<u16>> made = Vector<u16>::with_capacity(needed);
    if (made.failed()) return made;
    Vector<u16> result = static_cast<OperationResult<Vector<u16>>&&>(made).take_value();
    const OperationStatus sized = result.resize(needed);
    STXLIB_REQUIRE(sized.succeeded(), "utf16_of: resizing within the reserved capacity failed");
    if (needed != 0) write_text_as_utf16(text, result.mutable_view().data());
    return OperationResult<Vector<u16>>::success(static_cast<Vector<u16>&&>(result));
}

OperationStatus append_utf16(Utf8String& text, const View<u16> units) noexcept {
    const UnitsMeasure measured = measure_units(units, true);
    if (!measured.well_formed) {
        return OperationStatus::failure(bad_unit("stxlib.transcode.append_utf16", measured.bad_index, "an unpaired surrogate"));
    }
    const OperationStatus reserved = reserve_more(text, measured.bytes, "stxlib.transcode.append_utf16");
    if (reserved.failed()) return reserved;
    UnitCursor cursor{units.data(), units.count(), 0};
    const OperationStatus result = append_produced(text, cursor);
    return result;
}

OperationResult<Utf8String> text_of_utf16(const View<u16> units) noexcept {
    Utf8String result;
    const OperationStatus appended = append_utf16(result, units);
    if (appended.failed()) return OperationResult<Utf8String>::failure(*appended.error());
    return OperationResult<Utf8String>::success(static_cast<Utf8String&&>(result));
}

// ---- UTF-32 -----------------------------------------------------------------

OperationResult<Vector<u32>> utf32_of(const TextView text) noexcept {
    const usize needed = code_point_count(text);
    OperationResult<Vector<u32>> made = Vector<u32>::with_capacity(needed);
    if (made.failed()) return made;
    Vector<u32> result = static_cast<OperationResult<Vector<u32>>&&>(made).take_value();
    TextCursor cursor{text.data(), text.count(), 0};
    while (cursor.more()) {
        const OperationStatus pushed = result.push_copy(cursor.next());
        STXLIB_REQUIRE(pushed.succeeded(), "utf32_of: a push within the reserved capacity failed");
    }
    return OperationResult<Vector<u32>>::success(static_cast<Vector<u32>&&>(result));
}

OperationStatus append_utf32(Utf8String& text, const View<u32> code_points) noexcept {
    usize bytes = 0;
    for (usize index = 0; index < code_points.count(); ++index) {
        const u32 code_point = code_points[index];
        if (!is_scalar(code_point)) {
            return OperationStatus::failure(
                bad_unit("stxlib.transcode.append_utf32", index, "a value that is not a Unicode scalar value"));
        }
        bytes = bytes + utf8_length_of(code_point);
    }
    const OperationStatus reserved = reserve_more(text, bytes, "stxlib.transcode.append_utf32");
    if (reserved.failed()) return reserved;
    ScalarCursor cursor{code_points.data(), code_points.count(), 0};
    const OperationStatus result = append_produced(text, cursor);
    return result;
}

OperationResult<Utf8String> text_of_utf32(const View<u32> code_points) noexcept {
    Utf8String result;
    const OperationStatus appended = append_utf32(result, code_points);
    if (appended.failed()) return OperationResult<Utf8String>::failure(*appended.error());
    return OperationResult<Utf8String>::success(static_cast<Utf8String&&>(result));
}

// ---- WTF-8 ------------------------------------------------------------------

bool is_wtf8(const View<char> bytes) noexcept {
    const Wtf8Measure measured = measure_wtf8(bytes.data(), bytes.count());
    const bool result = measured.stopped == bytes.count();
    return result;
}

OperationResult<Vector<char>> wtf8_of_utf16(const View<u16> units) noexcept {
    const UnitsMeasure measured = measure_units(units, false);
    OperationResult<Vector<char>> made = Vector<char>::with_capacity(measured.bytes);
    if (made.failed()) return made;
    Vector<char> result = static_cast<OperationResult<Vector<char>>&&>(made).take_value();
    const OperationStatus sized = result.resize(measured.bytes);
    STXLIB_REQUIRE(sized.succeeded(), "wtf8_of_utf16: resizing within the reserved capacity failed");
    if (measured.bytes != 0) write_units_as_utf8(units, result.mutable_view().data());
    return OperationResult<Vector<char>>::success(static_cast<Vector<char>&&>(result));
}

OperationResult<Vector<u16>> utf16_of_wtf8(const View<char> bytes) noexcept {
    const Wtf8Measure measured = measure_wtf8(bytes.data(), bytes.count());
    if (measured.stopped != bytes.count()) {
        return OperationResult<Vector<u16>>::failure(
            bad_unit("stxlib.transcode.utf16_of_wtf8", measured.stopped, "the bytes are not well-formed WTF-8"));
    }
    OperationResult<Vector<u16>> made = Vector<u16>::with_capacity(measured.units);
    if (made.failed()) return made;
    Vector<u16> result = static_cast<OperationResult<Vector<u16>>&&>(made).take_value();
    usize index = 0;
    while (index < bytes.count()) {
        const usize length = wtf8::sequence_length(bytes.data(), bytes.count(), index);
        const u32 code_point = wtf8::decode(bytes.data(), index, length);
        index = index + length;
        if (code_point < 0x10000U) {
            const OperationStatus pushed = result.push_copy(static_cast<u16>(code_point));
            STXLIB_REQUIRE(pushed.succeeded(), "utf16_of_wtf8: a push within the reserved capacity failed");
            continue;
        }
        const u32 offset = code_point - 0x10000U;
        const OperationStatus high = result.push_copy(static_cast<u16>(high_first + (offset >> 10)));
        const OperationStatus low = result.push_copy(static_cast<u16>(low_first + (offset & 0x3ffU)));
        STXLIB_REQUIRE(high.succeeded() && low.succeeded(), "utf16_of_wtf8: a push within the reserved capacity failed");
    }
    return OperationResult<Vector<u16>>::success(static_cast<Vector<u16>&&>(result));
}

} // namespace stxlib
