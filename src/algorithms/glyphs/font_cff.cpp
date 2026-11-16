// stxlib::glyphs: CFF outlines (include/stxlib/font_file.hpp). A glyph's
// points in font units from its Type 2 charstring. After ttf-parser 0.25.1
// (tables/cff: index.rs, dict.rs, cff1.rs, charstring.rs: the indexes, the
// dictionaries, the operators) and, for the numbers, Skrifa 0.50.0 with
// read-fonts 0.47.0 (ps/charstring.rs, skrifa's outline/unscaled.rs):
// operands are 16.16, a coordinate goes to whole font units by a
// sixty-fourth and a shift, a line that goes nowhere is left out, and the
// point that would close a contour on its own start is left out.
//
// A charstring is a list of drawing operators. Nothing here is a font
// program in the sense of TrueType's: there is no loop and no jump, a
// subroutine is another list, called at most ten deep, and the whole glyph
// has a budget of steps. The hint operators are counted (their count says
// how long a mask is) and otherwise passed over. Not read: the arithmetic
// and storage operators, and the accented-character form of endchar, which
// the format has retired; a glyph that uses one is refused.

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs::detail {

namespace {

// An index: a count, the width of its offsets, the offsets, the objects.
struct Index final {
    Span offsets{};
    Span objects{};
    usize count{0};
    usize width{0};
};

// Reads the index at `offset` of `data`. `end` is the first byte after it.
[[nodiscard]] bool read_index(const Span& data, const usize offset, Index& index, usize& end) noexcept {
    index = Index{};
    if (!has(data, offset, 2)) return false;
    const usize count = read_u16(data, offset);
    if (count == 0) {
        end = offset + 2;
        return true;
    }
    if (!has(data, offset, 3)) return false;
    const usize width = read_u8(data, offset + 2);
    if (width < 1 || width > 4) return false;
    const usize offsets_at = offset + 3;
    const usize offsets_size = (count + 1) * width;
    if (!has(data, offsets_at, offsets_size)) return false;
    index.offsets = part(data, offsets_at, offsets_size);
    index.count = count;
    index.width = width;
    // The last offset is one more than the objects' length.
    usize last = 0;
    for (usize byte = 0; byte < width; ++byte) last = (last << 8) | read_u8(index.offsets, count * width + byte);
    if (last < 1 || !has(data, offsets_at + offsets_size, last - 1)) return false;
    index.objects = part(data, offsets_at + offsets_size, last - 1);
    end = offsets_at + offsets_size + (last - 1);
    return true;
}

// Object `number` of an index: empty when the index has none such or its
// offsets are not in order.
[[nodiscard]] Span object_of(const Index& index, const usize number) noexcept {
    if (number >= index.count) return Span{};
    usize begin = 0;
    usize end = 0;
    for (usize byte = 0; byte < index.width; ++byte) {
        begin = (begin << 8) | read_u8(index.offsets, number * index.width + byte);
        end = (end << 8) | read_u8(index.offsets, (number + 1) * index.width + byte);
    }
    if (begin < 1 || end < begin || !has(index.objects, begin - 1, end - begin)) return Span{};
    const Span result = part(index.objects, begin - 1, end - begin);
    return result;
}

// A dictionary's whole-number operands for one operator. A real number is
// stepped over and read as 0: no operand this reader uses is one.
inline constexpr usize most_operands = 48;
struct Entry final {
    u32 operation{0};
    usize count{0};
    i64 operands[most_operands]{};
};

// The next entry of the dictionary from `cursor`; false at its end or when
// it is not well formed.
[[nodiscard]] bool next_entry(const Span& dictionary, usize& cursor, Entry& entry) noexcept {
    entry.count = 0;
    while (cursor < dictionary.size) {
        const u32 first = read_u8(dictionary, cursor);
        cursor = cursor + 1;
        i64 value = 0;
        if (first <= 21) {
            entry.operation = first;
            if (first == 12) {
                if (!has(dictionary, cursor, 1)) return false;
                entry.operation = 1200 + read_u8(dictionary, cursor);
                cursor = cursor + 1;
            }
            return true;
        }
        if (first == 28) {
            if (!has(dictionary, cursor, 2)) return false;
            value = read_i16(dictionary, cursor);
            cursor = cursor + 2;
        } else if (first == 29) {
            if (!has(dictionary, cursor, 4)) return false;
            const u32 bits = read_u32(dictionary, cursor);
            value = bits >= 0x80000000U ? static_cast<i64>(bits) - 0x100000000LL : static_cast<i64>(bits);
            cursor = cursor + 4;
        } else if (first == 30) {
            // A real number: pairs of digits until the one that ends it.
            bool ended = false;
            while (!ended) {
                if (!has(dictionary, cursor, 1)) return false;
                const u32 pair = read_u8(dictionary, cursor);
                cursor = cursor + 1;
                ended = (pair >> 4) == 0xF || (pair & 0xF) == 0xF;
            }
        } else if (first >= 32 && first <= 246) {
            value = static_cast<i64>(first) - 139;
        } else if (first >= 247 && first <= 250) {
            if (!has(dictionary, cursor, 1)) return false;
            value = (static_cast<i64>(first) - 247) * 256 + read_u8(dictionary, cursor) + 108;
            cursor = cursor + 1;
        } else if (first >= 251 && first <= 254) {
            if (!has(dictionary, cursor, 1)) return false;
            value = -(static_cast<i64>(first) - 251) * 256 - static_cast<i64>(read_u8(dictionary, cursor)) - 108;
            cursor = cursor + 1;
        } else {
            return false;
        }
        if (entry.count >= most_operands) return false;
        entry.operands[entry.count] = value;
        entry.count = entry.count + 1;
    }
    return false;
}

inline constexpr u32 operation_charstrings = 17;
inline constexpr u32 operation_private = 18;
inline constexpr u32 operation_subroutines = 19;
inline constexpr u32 operation_charstring_type = 1206;
inline constexpr u32 operation_registry = 1230;
inline constexpr u32 operation_dictionaries = 1236;
inline constexpr u32 operation_select = 1237;

// The private dictionary a dictionary names, and the local subroutines it
// names in turn: where that index begins in the table, or 0 for none.
[[nodiscard]] usize subroutines_named_by(const Span& table, const Span& dictionary) noexcept {
    usize cursor = 0;
    Entry entry{};
    usize private_size = 0;
    usize private_at = 0;
    while (next_entry(dictionary, cursor, entry)) {
        if (entry.operation == operation_private && entry.count == 2 && entry.operands[0] >= 0 && entry.operands[1] >= 0) {
            private_size = static_cast<usize>(entry.operands[0]);
            private_at = static_cast<usize>(entry.operands[1]);
        }
    }
    if (private_size == 0 || !has(table, private_at, private_size)) return 0;
    const Span private_dictionary = part(table, private_at, private_size);
    cursor = 0;
    usize result = 0;
    while (next_entry(private_dictionary, cursor, entry)) {
        if (entry.operation == operation_subroutines && entry.count == 1 && entry.operands[0] > 0) {
            // From the private dictionary's own beginning.
            const usize at = private_at + static_cast<usize>(entry.operands[0]);
            result = has(table, at, 2) ? at : 0;
        }
    }
    return result;
}

[[nodiscard]] FontTable table_part(const Font& font, const usize offset, const usize length) noexcept {
    const FontTable result{.offset = static_cast<u32>(font.cff.offset + offset), .length = static_cast<u32>(length)};
    return result;
}

// The index a Font names, read again from its bytes.
[[nodiscard]] bool index_of(const Font& font, const FontTable& where, Index& index) noexcept {
    const Span data = table_of(font, where);
    usize end = 0;
    const bool result = where.length != 0 && read_index(data, 0, index, end);
    return result;
}

// ---- the charstring ------------------------------------------------------------------------

inline constexpr usize most_arguments = 48;
inline constexpr usize deepest_call = 10;
// A sixty-fourth, as 16.16.
inline constexpr i64 one_sixty_fourth = 1024;
inline constexpr i64 farthest = static_cast<i64>(1) << 24;

enum class Pending : u8 {
    none = 0,
    move = 1,
    line = 2,
    curve = 3,
};

// The path as it is drawn, and the points it becomes: a move waits for the
// first thing drawn from it, and a line or curve waits for what follows,
// so that at the end of a contour the point that would repeat its start
// can be left out.
struct Path final {
    GlyphOutline* outline{nullptr};
    const FontLimits* limits{nullptr};
    Pending pending{Pending::none};
    Point waiting[3]{};
    Point start{};
    bool open{false};
    bool too_large{false};
    bool no_memory{false};
};

// A 16.16 coordinate in whole font units: a sixty-fourth of it, rounded,
// and then the fraction dropped.
[[nodiscard]] i32 units_of(const i64 value) noexcept {
    const i64 units = multiply_fixed(value, one_sixty_fourth) >> 10;
    const i64 result = units < -farthest ? -farthest : (units > farthest ? farthest : units);
    return static_cast<i32>(result);
}

void put_point(Path& path, const Point point, const PointTag tag) noexcept {
    GlyphOutline& outline = *path.outline;
    if (outline.points.count() >= (*path.limits).points) {
        path.too_large = true;
        return;
    }
    if (outline.points.push_copy(point).failed() || outline.tags.push_copy(tag).failed()) path.no_memory = true;
}

void flush(Path& path, const bool closing) noexcept {
    const Pending pending = path.pending;
    path.pending = Pending::none;
    if (pending == Pending::move) {
        if (closing) return;
        path.open = true;
        path.start = path.waiting[0];
        put_point(path, path.start, PointTag::on);
        return;
    }
    if (pending == Pending::none) return;
    Point end = path.waiting[0];
    if (pending == Pending::curve) {
        put_point(path, path.waiting[0], PointTag::cubic);
        put_point(path, path.waiting[1], PointTag::cubic);
        end = path.waiting[2];
    }
    const bool repeats_start = end.x == path.start.x && end.y == path.start.y;
    if (!closing || !repeats_start) put_point(path, end, PointTag::on);
}

void path_move(Path& path, const i64 x, const i64 y) noexcept {
    path.pending = Pending::move;
    path.waiting[0] = Point{.x = units_of(x), .y = units_of(y)};
}

void path_line(Path& path, const i64 x, const i64 y) noexcept {
    const Point to{.x = units_of(x), .y = units_of(y)};
    // A line to where the path already is goes nowhere.
    if (path.pending != Pending::none) {
        const Point at = path.pending == Pending::curve ? path.waiting[2] : path.waiting[0];
        if (at.x == to.x && at.y == to.y) return;
    }
    flush(path, false);
    path.pending = Pending::line;
    path.waiting[0] = to;
}

void path_curve(Path& path, const i64 x1, const i64 y1, const i64 x2, const i64 y2, const i64 x3, const i64 y3) noexcept {
    flush(path, false);
    path.pending = Pending::curve;
    path.waiting[0] = Point{.x = units_of(x1), .y = units_of(y1)};
    path.waiting[1] = Point{.x = units_of(x2), .y = units_of(y2)};
    path.waiting[2] = Point{.x = units_of(x3), .y = units_of(y3)};
}

void path_close(Path& path) noexcept {
    flush(path, true);
    if (!path.open) return;
    path.open = false;
    GlyphOutline& outline = *path.outline;
    const usize count = outline.points.count();
    const usize contours = outline.contour_ends.count();
    const usize before = contours == 0 ? 0 : static_cast<usize>(outline.contour_ends[contours - 1]) + 1;
    if (count == before) return;
    if (contours >= (*path.limits).contours) {
        path.too_large = true;
        return;
    }
    if (outline.contour_ends.push_copy(static_cast<u32>(count - 1)).failed()) path.no_memory = true;
}

// One list of operators being run, and where in it.
struct Call final {
    Span code{};
    usize cursor{0};
};

struct Machine final {
    Path path{};
    Index global{};
    Index local{};
    i64 arguments[most_arguments]{};
    usize count{0};
    // Where the path is, 16.16.
    i64 x{0};
    i64 y{0};
    usize stems{0};
    // A contour has been begun.
    bool drawing{false};
    bool ended{false};
};

enum class Ran : u8 {
    done = 0,
    invalid = 1,
    too_large = 2,
};

// Something is drawn: a path with no move before it begins at the origin.
void begin_if_not(Machine& machine) noexcept {
    if (machine.drawing) return;
    machine.drawing = true;
    path_move(machine.path, 0, 0);
}

void move_to(Machine& machine) noexcept {
    if (machine.drawing) path_close(machine.path);
    machine.drawing = true;
    path_move(machine.path, machine.x, machine.y);
}

void line_to(Machine& machine) noexcept {
    path_line(machine.path, machine.x, machine.y);
}

// A curve by its three steps from where the path is.
void curve_by(Machine& machine, const i64 dx1, const i64 dy1, const i64 dx2, const i64 dy2, const i64 dx3, const i64 dy3) noexcept {
    const i64 x1 = machine.x + dx1;
    const i64 y1 = machine.y + dy1;
    const i64 x2 = x1 + dx2;
    const i64 y2 = y1 + dy2;
    machine.x = x2 + dx3;
    machine.y = y2 + dy3;
    path_curve(machine.path, x1, y1, x2, y2, machine.x, machine.y);
}

// The number a subroutine's operand stands for: a bias by the size of its
// index is added.
[[nodiscard]] bool subroutine_number(const i64 operand, const usize count, usize& number) noexcept {
    const i64 bias = count < 1240 ? 107 : (count < 33900 ? 1131 : 32768);
    if ((operand & 0xFFFF) != 0) return false;
    const i64 value = (operand >> 16) + bias;
    if (value < 0 || static_cast<usize>(value) >= count) return false;
    number = static_cast<usize>(value);
    return true;
}

// The path operators. Each takes all the arguments there are; a count that
// does not fit the operator is refused.
[[nodiscard]] bool draw(Machine& machine, const u32 operation) noexcept {
    const i64* const a = machine.arguments;
    const usize n = machine.count;
    if (operation == 5) {
        // rlineto
        if (n < 2 || n % 2 != 0) return false;
        for (usize i = 0; i < n; i = i + 2) {
            machine.x = machine.x + a[i];
            machine.y = machine.y + a[i + 1];
            line_to(machine);
        }
        return true;
    }
    if (operation == 6 || operation == 7) {
        // hlineto, vlineto: the two directions in turn.
        if (n < 1) return false;
        bool across = operation == 6;
        for (usize i = 0; i < n; ++i) {
            if (across) {
                machine.x = machine.x + a[i];
            } else {
                machine.y = machine.y + a[i];
            }
            line_to(machine);
            across = !across;
        }
        return true;
    }
    if (operation == 8) {
        // rrcurveto
        if (n < 6 || n % 6 != 0) return false;
        for (usize i = 0; i < n; i = i + 6) curve_by(machine, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
        return true;
    }
    if (operation == 24) {
        // rcurveline
        if (n < 8 || (n - 2) % 6 != 0) return false;
        for (usize i = 0; i + 2 < n; i = i + 6) curve_by(machine, a[i], a[i + 1], a[i + 2], a[i + 3], a[i + 4], a[i + 5]);
        machine.x = machine.x + a[n - 2];
        machine.y = machine.y + a[n - 1];
        line_to(machine);
        return true;
    }
    if (operation == 25) {
        // rlinecurve
        if (n < 8 || (n - 6) % 2 != 0) return false;
        for (usize i = 0; i + 6 < n; i = i + 2) {
            machine.x = machine.x + a[i];
            machine.y = machine.y + a[i + 1];
            line_to(machine);
        }
        curve_by(machine, a[n - 6], a[n - 5], a[n - 4], a[n - 3], a[n - 2], a[n - 1]);
        return true;
    }
    if (operation == 26 || operation == 27) {
        // vvcurveto, hhcurveto: an odd first argument steps the other way.
        usize i = 0;
        if (n % 2 != 0) {
            if (operation == 26) {
                machine.x = machine.x + a[0];
            } else {
                machine.y = machine.y + a[0];
            }
            i = 1;
        }
        if (n - i < 4 || (n - i) % 4 != 0) return false;
        for (; i < n; i = i + 4) {
            if (operation == 26) {
                curve_by(machine, 0, a[i], a[i + 1], a[i + 2], 0, a[i + 3]);
            } else {
                curve_by(machine, a[i], 0, a[i + 1], a[i + 2], a[i + 3], 0);
            }
        }
        return true;
    }
    if (operation == 30 || operation == 31) {
        // vhcurveto, hvcurveto: curves that begin along one axis and end
        // along the other, in turn; a last odd argument finishes the last.
        if (n < 4) return false;
        bool across = operation == 31;
        usize i = 0;
        while (i < n) {
            if (n - i < 4) return false;
            const bool last = n - i == 5;
            const i64 extra = last ? a[i + 4] : 0;
            if (across) {
                curve_by(machine, a[i], 0, a[i + 1], a[i + 2], extra, a[i + 3]);
            } else {
                curve_by(machine, 0, a[i], a[i + 1], a[i + 2], a[i + 3], extra);
            }
            i = i + (last ? 5 : 4);
            across = !across;
        }
        return true;
    }
    return false;
}

// The flex operators: two curves.
[[nodiscard]] bool flex(Machine& machine, const u32 operation) noexcept {
    const i64* const a = machine.arguments;
    const usize n = machine.count;
    const i64 start_x = machine.x;
    const i64 start_y = machine.y;
    if (operation == 35) {
        if (n != 13) return false;
        curve_by(machine, a[0], a[1], a[2], a[3], a[4], a[5]);
        curve_by(machine, a[6], a[7], a[8], a[9], a[10], a[11]);
        return true;
    }
    if (operation == 34) {
        if (n != 7) return false;
        curve_by(machine, a[0], 0, a[1], a[2], a[3], 0);
        curve_by(machine, a[4], 0, a[5], -a[2], a[6], 0);
        return true;
    }
    if (operation == 36) {
        if (n != 9) return false;
        curve_by(machine, a[0], a[1], a[2], a[3], a[4], 0);
        curve_by(machine, a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7]));
        return true;
    }
    if (operation == 37) {
        if (n != 11) return false;
        curve_by(machine, a[0], a[1], a[2], a[3], a[4], a[5]);
        // The last step is along whichever axis the flex has gone further.
        const i64 dx = a[0] + a[2] + a[4] + a[6] + a[8];
        const i64 dy = a[1] + a[3] + a[5] + a[7] + a[9];
        const bool across = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy);
        const i64 x1 = machine.x + a[6];
        const i64 y1 = machine.y + a[7];
        const i64 x2 = x1 + a[8];
        const i64 y2 = y1 + a[9];
        machine.x = across ? x2 + a[10] : start_x;
        machine.y = across ? start_y : y2 + a[10];
        path_curve(machine.path, x1, y1, x2, y2, machine.x, machine.y);
        return true;
    }
    return false;
}

[[nodiscard]] Ran run(Machine& machine, const Span& charstring, const u32 step_limit) noexcept {
    Call calls[deepest_call + 1] = {};
    usize depth = 1;
    calls[0] = Call{.code = charstring, .cursor = 0};
    u32 steps = 0;
    while (depth > 0 && !machine.ended) {
        Call& call = calls[depth - 1];
        if (call.cursor >= call.code.size) {
            // The list ran out with no return: the caller goes on.
            depth = depth - 1;
            continue;
        }
        steps = steps + 1;
        if (steps > step_limit) return Ran::too_large;
        const Span& code = call.code;
        const u32 first = read_u8(code, call.cursor);
        call.cursor = call.cursor + 1;
        // Numbers.
        if (first == 28 || first >= 32) {
            i64 value = 0;
            if (first == 28) {
                if (!has(code, call.cursor, 2)) return Ran::invalid;
                value = static_cast<i64>(read_i16(code, call.cursor)) * 65536;
                call.cursor = call.cursor + 2;
            } else if (first <= 246) {
                value = (static_cast<i64>(first) - 139) * 65536;
            } else if (first <= 250) {
                if (!has(code, call.cursor, 1)) return Ran::invalid;
                value = ((static_cast<i64>(first) - 247) * 256 + read_u8(code, call.cursor) + 108) * 65536;
                call.cursor = call.cursor + 1;
            } else if (first <= 254) {
                if (!has(code, call.cursor, 1)) return Ran::invalid;
                value = (-(static_cast<i64>(first) - 251) * 256 - static_cast<i64>(read_u8(code, call.cursor)) - 108) * 65536;
                call.cursor = call.cursor + 1;
            } else {
                if (!has(code, call.cursor, 4)) return Ran::invalid;
                const u32 bits = read_u32(code, call.cursor);
                value = bits >= 0x80000000U ? static_cast<i64>(bits) - 0x100000000LL : static_cast<i64>(bits);
                call.cursor = call.cursor + 4;
            }
            if (machine.count >= most_arguments) return Ran::invalid;
            machine.arguments[machine.count] = value;
            machine.count = machine.count + 1;
            continue;
        }
        const usize n = machine.count;
        const i64* const a = machine.arguments;
        if (first == 1 || first == 3 || first == 18 || first == 23) {
            // Stems: pairs, after a width if the count is odd.
            machine.stems = machine.stems + n / 2;
            machine.count = 0;
        } else if (first == 19 || first == 20) {
            // A mask: a bit for every stem; pairs left on the stack are
            // stems too.
            machine.stems = machine.stems + n / 2;
            machine.count = 0;
            const usize bytes = (machine.stems + 7) / 8;
            if (!has(code, call.cursor, bytes)) return Ran::invalid;
            call.cursor = call.cursor + bytes;
        } else if (first == 21 || first == 22 || first == 4) {
            // rmoveto, hmoveto, vmoveto: the last arguments, a width before
            // them or not.
            const usize needed = first == 21 ? 2 : 1;
            if (n < needed || n > needed + 1) return Ran::invalid;
            if (first == 21) {
                machine.x = machine.x + a[n - 2];
                machine.y = machine.y + a[n - 1];
            } else if (first == 22) {
                machine.x = machine.x + a[n - 1];
            } else {
                machine.y = machine.y + a[n - 1];
            }
            move_to(machine);
            machine.count = 0;
        } else if (first == 10 || first == 29) {
            // A subroutine, local or global.
            const Index& index = first == 10 ? machine.local : machine.global;
            usize number = 0;
            if (n < 1 || !subroutine_number(a[n - 1], index.count, number)) return Ran::invalid;
            machine.count = n - 1;
            if (depth > deepest_call) return Ran::too_large;
            calls[depth] = Call{.code = object_of(index, number), .cursor = 0};
            depth = depth + 1;
        } else if (first == 11) {
            depth = depth - 1;
        } else if (first == 14) {
            // endchar. With four arguments it once named two other glyphs
            // to put together; that form is not read.
            if (n >= 4) return Ran::invalid;
            if (machine.drawing) path_close(machine.path);
            machine.drawing = false;
            machine.ended = true;
        } else if (first == 12) {
            if (!has(code, call.cursor, 1)) return Ran::invalid;
            const u32 second = read_u8(code, call.cursor);
            call.cursor = call.cursor + 1;
            begin_if_not(machine);
            if (!flex(machine, second)) return Ran::invalid;
            machine.count = 0;
        } else {
            begin_if_not(machine);
            if (!draw(machine, first)) return Ran::invalid;
            machine.count = 0;
        }
        if (machine.path.too_large) return Ran::too_large;
    }
    // A glyph that ends without endchar is closed all the same.
    if (machine.drawing) path_close(machine.path);
    return Ran::done;
}

// Which font dictionary a glyph uses, by the select table: one number a
// glyph, or ranges of glyphs.
[[nodiscard]] bool dictionary_of(const Span& select, const u32 glyph, usize& dictionary) noexcept {
    if (!has(select, 0, 1)) return false;
    const u32 format = read_u8(select, 0);
    if (format == 0) {
        if (!has(select, 1 + static_cast<usize>(glyph), 1)) return false;
        dictionary = read_u8(select, 1 + static_cast<usize>(glyph));
        return true;
    }
    if (format != 3 || !has(select, 1, 2)) return false;
    const usize ranges = read_u16(select, 1);
    if (ranges == 0 || !has(select, 3, ranges * 3 + 2)) return false;
    // The last range that begins at or before the glyph.
    usize low = 0;
    usize high = ranges;
    while (high - low > 1) {
        const usize middle = low + (high - low) / 2;
        if (read_u16(select, 3 + middle * 3) <= glyph) {
            low = middle;
        } else {
            high = middle;
        }
    }
    if (read_u16(select, 3 + low * 3) > glyph || read_u16(select, 3 + ranges * 3) <= glyph) return false;
    dictionary = read_u8(select, 3 + low * 3 + 2);
    return true;
}

} // namespace

Status open_cff(Font& font) noexcept {
    const Operation operation("stxlib.glyphs.open_font");
    const Span table = table_of(font, font.cff);
    if (!has(table, 0, 4) || read_u8(table, 0) != 1) return refuse_font(ErrorKind::invalid_argument, operation);
    // The header, then four indexes: names, the top dictionaries, strings,
    // the global subroutines.
    usize cursor = read_u8(table, 2);
    Index names{};
    Index tops{};
    Index strings{};
    Index global{};
    if (!read_index(table, cursor, names, cursor) || !read_index(table, cursor, tops, cursor) || !read_index(table, cursor, strings, cursor)) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    const usize global_at = cursor;
    if (!read_index(table, cursor, global, cursor)) return refuse_font(ErrorKind::invalid_argument, operation);
    font.global_subroutines = table_part(font, global_at, cursor - global_at);
    const Span top = object_of(tops, 0);
    usize charstrings_at = 0;
    usize dictionaries_at = 0;
    usize select_at = 0;
    bool by_dictionary = false;
    usize dictionary_cursor = 0;
    Entry entry{};
    while (next_entry(top, dictionary_cursor, entry)) {
        const bool one = entry.count == 1 && entry.operands[0] > 0;
        if (entry.operation == operation_charstrings && one) charstrings_at = static_cast<usize>(entry.operands[0]);
        if (entry.operation == operation_dictionaries && one) dictionaries_at = static_cast<usize>(entry.operands[0]);
        if (entry.operation == operation_select && one) select_at = static_cast<usize>(entry.operands[0]);
        if (entry.operation == operation_registry) by_dictionary = true;
        if (entry.operation == operation_charstring_type && (entry.count != 1 || entry.operands[0] != 2)) {
            return refuse_font(ErrorKind::invalid_argument, operation);
        }
    }
    Index charstrings{};
    usize end = 0;
    if (charstrings_at == 0 || !read_index(table, charstrings_at, charstrings, end) || charstrings.count == 0) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    font.charstrings = table_part(font, charstrings_at, end - charstrings_at);
    if (by_dictionary) {
        Index dictionaries{};
        if (dictionaries_at == 0 || !read_index(table, dictionaries_at, dictionaries, end) || !has(table, select_at, 1) || select_at == 0) {
            return refuse_font(ErrorKind::invalid_argument, operation);
        }
        font.font_dictionaries = table_part(font, dictionaries_at, end - dictionaries_at);
        font.dictionary_select = table_part(font, select_at, table.size - select_at);
    } else {
        const usize local_at = subroutines_named_by(table, top);
        Index local{};
        if (local_at != 0 && read_index(table, local_at, local, end)) font.local_subroutines = table_part(font, local_at, end - local_at);
    }
    return Status::success();
}

Status load_cff_glyph(const Font& font, const u32 glyph, const FontLimits& limits, GlyphOutline& outline) noexcept {
    const Operation operation("stxlib.glyphs.load_glyph");
    Index charstrings{};
    if (!index_of(font, font.charstrings, charstrings)) return refuse_font(ErrorKind::invalid_argument, operation);
    // A glyph past the charstrings has no outline.
    if (glyph >= charstrings.count) return Status::success();
    Machine machine{};
    machine.path.outline = &outline;
    machine.path.limits = &limits;
    static_cast<void>(index_of(font, font.global_subroutines, machine.global));
    if (font.font_dictionaries.length != 0) {
        // The glyph's own font dictionary names its local subroutines.
        const Span table = table_of(font, font.cff);
        Index dictionaries{};
        usize dictionary = 0;
        if (index_of(font, font.font_dictionaries, dictionaries) && dictionary_of(table_of(font, font.dictionary_select), glyph, dictionary)) {
            const usize local_at = subroutines_named_by(table, object_of(dictionaries, dictionary));
            usize end = 0;
            if (local_at != 0 && !read_index(table, local_at, machine.local, end)) machine.local = Index{};
        }
    } else {
        static_cast<void>(index_of(font, font.local_subroutines, machine.local));
    }
    const Ran ran = run(machine, object_of(charstrings, glyph), limits.charstring_steps);
    if (machine.path.no_memory) return refuse_font(ErrorKind::out_of_memory, operation);
    if (ran == Ran::too_large || machine.path.too_large) return refuse_font(ErrorKind::limit_exceeded, operation);
    if (ran == Ran::invalid) return refuse_font(ErrorKind::invalid_argument, operation);
    return Status::success();
}

} // namespace stxlib::glyphs::detail
