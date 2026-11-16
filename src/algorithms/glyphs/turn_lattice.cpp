// stxlib::glyphs: a glyph's mask as a smooth surface, and that surface
// turned (src/algorithms/glyphs/turn_lattice.hpp has the rules).

#include "algorithms/glyphs/turn_lattice.hpp"

#include "algorithms/glyphs/glyph_checks.hpp"

namespace stxlib::glyphs::detail {

namespace {

// n / d rounded down, d above 0.
[[nodiscard]] constexpr i64 floored(const i64 n, const i64 d) noexcept {
    const i64 result = n >= 0 ? n / d : -((-n + d - 1) / d);
    return result;
}

// Of the columns [first, last), those k with 0 <= start + k * step < limit.
void within(const i64 start, const i64 step, const i64 limit, i64& first, i64& last) noexcept {
    i64 low = first;
    i64 high = last;
    if (step == 0) {
        if (start < 0 || start >= limit) high = low;
    } else if (step > 0) {
        low = floored(-start + step - 1, step);
        high = floored(limit - start + step - 1, step);
    } else {
        low = floored(start - limit, -step) + 1;
        high = floored(start, -step) + 1;
    }
    first = low > first ? low : first;
    last = high < last ? high : last;
}

// A column's result as a kept control: rule 2's rounding.
[[nodiscard]] constexpr i16 kept_control(const i64 value) noexcept {
    const i64 size = value < 0 ? -value : value;
    const i64 rounded = (size + control_divisor / 2) / control_divisor;
    const i64 control = value < 0 ? -rounded : rounded;
    const i16 result = static_cast<i16>(control < -32768 ? -32768 : (control > 32767 ? 32767 : control));
    return result;
}

} // namespace

bool build_lattice(const u8* const pixels, const u32 width, const u32 height, Vector<i64>& rows, Vector<i16>& controls) noexcept {
    STXLIB_GLYPHS_CHECK(width >= 1 && height >= 1 && width <= lattice_side && height <= lattice_side, "build_lattice: a mask out of range");
    const usize stride = lattice_stride(width);
    const usize across = static_cast<usize>(width) + 1;
    const usize down = static_cast<usize>(height) + 1;
    // The rows' results, with three rows of nothing above and below so
    // that a column is read without asking where it ends; and one row of
    // the mask with three pixels of nothing either side, for the same.
    const usize padded_rows = static_cast<usize>(height) + 6;
    const usize line_length = static_cast<usize>(width) + 6;
    if (rows.resize(padded_rows * stride + line_length).failed() || controls.resize(stride * lattice_stride(height)).failed()) return false;
    i64* const results = rows.mutable_view().data();
    i64* const line = results + padded_rows * stride;
    for (usize index = 0; index < 3 * stride; ++index) {
        results[index] = 0;
        results[(padded_rows - 3) * stride + index] = 0;
    }
    for (usize index = 0; index < line_length; ++index) line[index] = 0;
    for (usize row = 0; row < height; ++row) {
        const u8* const from = pixels + row * width;
        for (usize column = 0; column < width; ++column) line[column + 3] = from[column];
        i64* const to = results + (row + 3) * stride;
        // The cell k lies between the pixels k - 1 and k: line[k + 2] and
        // line[k + 3].
        for (usize cell = 0; cell < across; ++cell) {
            lattice_cell(line[cell], line[cell + 1], line[cell + 2], line[cell + 3], line[cell + 4], line[cell + 5], to + 5 * cell);
        }
        to[stride - 1] = 0;
    }
    i16* const kept = controls.mutable_view().data();
    for (usize column = 0; column < stride; ++column) {
        const i64* const from = results + column;
        for (usize cell = 0; cell < down; ++cell) {
            i64 made[5] = {0, 0, 0, 0, 0};
            lattice_cell(from[cell * stride], from[(cell + 1) * stride], from[(cell + 2) * stride], from[(cell + 3) * stride], from[(cell + 4) * stride],
                         from[(cell + 5) * stride], made);
            for (usize step = 0; step < 5; ++step) kept[(5 * cell + step) * stride + column] = kept_control(made[step]);
        }
        kept[(5 * down) * stride + column] = 0;
    }
    return true;
}

bool is_turn(const i32 xx, const i32 xy, const i32 yx, const i32 yy) noexcept {
    const i64 one = static_cast<i64>(1) << 32;
    const i64 length = static_cast<i64>(xx) * xx + static_cast<i64>(yx) * yx;
    const i64 off = length > one ? length - one : one - length;
    const bool result = xx == yy && static_cast<i64>(xy) == -static_cast<i64>(yx) && off <= (static_cast<i64>(1) << 18);
    return result;
}

bool turned_bounds(const Bounds& upright, const i32 cosine, const i32 sine, const Point shift, const Limits& limits, Bounds& turned) noexcept {
    turned = Bounds{};
    STXLIB_GLYPHS_CHECK(upright.width >= 1 && upright.height >= 1 && is_turn(cosine, -sine, sine, cosine), "turned_bounds: not a mask and a turn");
    // The rectangle's corners in 64ths, and through the turn in 64ths of
    // 65536ths.
    const i64 xs[2] = {static_cast<i64>(upright.left) * 64 - 32, (static_cast<i64>(upright.left) + upright.width) * 64 + 32};
    const i64 ys[2] = {static_cast<i64>(upright.top) * 64 + 32, (static_cast<i64>(upright.top) - upright.height) * 64 - 32};
    const i64 c = cosine;
    const i64 s = sine;
    i64 least_x = 0;
    i64 most_x = 0;
    i64 least_y = 0;
    i64 most_y = 0;
    for (usize corner = 0; corner < 4; ++corner) {
        const i64 x = xs[corner & 1];
        const i64 y = ys[corner >> 1];
        const i64 turned_x = x * c - y * s;
        const i64 turned_y = x * s + y * c;
        least_x = (corner == 0 || turned_x < least_x) ? turned_x : least_x;
        most_x = (corner == 0 || turned_x > most_x) ? turned_x : most_x;
        least_y = (corner == 0 || turned_y < least_y) ? turned_y : least_y;
        most_y = (corner == 0 || turned_y > most_y) ? turned_y : most_y;
    }
    // In 4194304ths of a pixel, the shift added; then the pixels whose
    // centres are inside.
    const i64 pixel = static_cast<i64>(1) << 22;
    const i64 half = pixel / 2;
    const i64 shift_x = static_cast<i64>(shift.x) * 65536;
    const i64 shift_y = static_cast<i64>(shift.y) * 65536;
    const i64 left = floored(least_x + shift_x + half, pixel);
    const i64 right = -floored(half - (most_x + shift_x), pixel);
    const i64 bottom = floored(least_y + shift_y + half, pixel);
    const i64 top = -floored(half - (most_y + shift_y), pixel);
    const i64 far = static_cast<i64>(1) << 30;
    if (right <= left || top <= bottom || right - left > limits.width || top - bottom > limits.height || left < -far || right > far || bottom < -far || top > far) {
        return false;
    }
    turned = Bounds{.left = static_cast<i32>(left), .top = static_cast<i32>(top), .width = static_cast<u32>(right - left), .height = static_cast<u32>(top - bottom)};
    return true;
}

void turn_lattice(const TurnLattice& lattice, const i32 cosine, const i32 sine, const Point shift, const Bounds& turned, u8* const pixels) noexcept {
    STXLIB_GLYPHS_CHECK(lattice.controls != nullptr && is_turn(cosine, -sine, sine, cosine), "turn_lattice: not a lattice and a turn");
    const i64 c = cosine;
    const i64 s = sine;
    const usize stride = lattice_stride(lattice.upright.width);
    const i64 limit_u = (static_cast<i64>(lattice.upright.width) + 1) << 16;
    const i64 limit_v = (static_cast<i64>(lattice.upright.height) + 1) << 16;
    const i64 origin_u = static_cast<i64>(lattice.upright.left) * 65536 - 32768;
    const i64 origin_v = static_cast<i64>(lattice.upright.top) * 65536 + 32768;
    const i64 x6 = static_cast<i64>(turned.left) * 64 + 32 - shift.x;
    const i16* const controls = lattice.controls;
    const usize width = turned.width;
    for (usize row = 0; row < turned.height; ++row) {
        u8* const line = pixels + row * width;
        const i64 y6 = (static_cast<i64>(turned.top) - static_cast<i64>(row)) * 64 - 32 - shift.y;
        const i64 u0 = ((x6 * c + y6 * s) >> 6) - origin_u + 128;
        const i64 v0 = origin_v - ((y6 * c - x6 * s) >> 6) + 128;
        // Which of the row's pixels are on the lattice is settled here,
        // and the loop over them asks nothing.
        i64 first = 0;
        i64 last = static_cast<i64>(width);
        within(u0, c, limit_u, first, last);
        within(v0, s, limit_v, first, last);
        if (last < 0) last = 0;
        if (first > last) first = last;
        for (i64 column = 0; column < first; ++column) line[column] = 0;
        for (i64 column = last; column < static_cast<i64>(width); ++column) line[column] = 0;
        i64 u = u0 + first * c;
        i64 v = v0 + first * s;
        for (i64 column = first; column < last; ++column) {
            const i16* const along = place_weights.of[(u >> 8) & 255];
            const i16* const across = place_weights.of[(v >> 8) & 255];
            const i16* const corner = controls + 5 * static_cast<usize>(v >> 16) * stride + 5 * static_cast<usize>(u >> 16);
            i32 total = 0;
            for (usize a = 0; a < 6; ++a) {
                const i16* const from = corner + a * stride;
                const i32 sum = along[0] * from[0] + along[1] * from[1] + along[2] * from[2] + along[3] * from[3] + along[4] * from[4] + along[5] * from[5];
                total = total + across[a] * ((sum + 8192) >> 14);
            }
            const i32 level = (total + 262144) >> 19;
            line[column] = static_cast<u8>(level < 0 ? 0 : (level > 255 ? 255 : level));
            u = u + c;
            v = v + s;
        }
    }
}

} // namespace stxlib::glyphs::detail
