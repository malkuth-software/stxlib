// stxlib::glyphs: faces to draw text from (include/stxlib/glyph_face.hpp).

#include "stxlib/glyph_face.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/glyph_checks.hpp"
#include "algorithms/glyphs/turn_lattice.hpp"
#include "core/checked.hpp"
#include "stxlib/glyph_hinter.hpp"
#include "stxlib/threads.hpp"

namespace stxlib::glyphs {

namespace detail {

struct FittedRecord final {
    u32 glyph{0};
    u32 size{0};
    u32 fit{0};
    u32 first_kind{0};
    u32 kind_count{0};
    u32 first_point{0};
    u32 point_count{0};
    i32 advance{0};
    i32 left{0};
    i32 bottom{0};
    i32 right{0};
    i32 top{0};
};

// One half of the fitted outlines: outlines end to end, their records, and
// a table from (glyph, size, fit) to a record's number plus one.
struct FittedHalf final {
    Vector<Segment> kinds{};
    Vector<Point> points{};
    Vector<FittedRecord> records{};
    Vector<u32> table{};
};

// A glyph's lattice among those held: whose it is, where its controls
// begin, and its upright mask's box.
struct LatticeRecord final {
    u32 glyph{0};
    u32 size{0};
    u32 thicken{0};
    u32 first{0};
    Bounds upright{};
};

// One half of the lattices, as a half of the fitted outlines: lattices end
// to end, their records, and a table from (glyph, size, thickening) to a
// record's number plus one.
struct LatticeHalf final {
    Vector<i16> controls{};
    Vector<LatticeRecord> records{};
    Vector<u32> table{};
};

// A glyph of a run as placed: its pixel, the part of a pixel left over,
// and its mask's bounds; and, for a run turned by its glyphs' surfaces,
// whether this glyph is (one too large for a lattice goes by its outline).
struct PlacedGlyph final {
    i32 pixel_x{0};
    i32 pixel_y{0};
    Point shift{};
    Bounds bounds{};
    bool shown{false};
    bool by_surface{false};
};

// The program's: everything here but the font is reached under the lock.
struct TypefaceState final {
    Font font{};
    TypefaceLimits limits{};
    Mutex lock{};
    GlyphStore store{};
    Hinter hinter{};
    bool prepared{false};
    usize peak{0};
    // Where a glyph is read to for its ink box.
    GlyphOutline measuring{};
};

// One thread's.
struct FaceState final {
    Shared<Typeface> typeface{};
    Font font{};
    FaceLimits limits{};
    Rasterizer rasterizer{};
    FittedHalf halves[2]{};
    // What each half holds, in bytes (FaceLimits' measure).
    usize held[2]{};
    u32 newer{0};
    usize fittings{0};
    usize peak{0};
    // Working storage: the outline being fitted, its segments, a turned
    // and a thickened outline's points, a glyph's mask, and a run's glyphs
    // as placed.
    GlyphOutline fitting{};
    Vector<Segment> work_kinds{};
    Vector<Point> work_points{};
    Vector<Point> turned{};
    Vector<Point> thick{};
    Vector<u8> glyph_pixels{};
    // A quarter-turned glyph's mask, turned from glyph_pixels.
    Vector<u8> turned_pixels{};
    Vector<PlacedGlyph> placed{};
    // The lattices of glyphs turned by their surfaces (turn_lattice.hpp),
    // in two halves as the fitted outlines are; and the working storage a
    // lattice is made in.
    LatticeHalf lattices[2]{};
    usize lattices_held[2]{};
    u32 lattices_newer{0};
    usize lattices_made{0};
    Vector<i64> lattice_rows{};
    Vector<i16> lattice_work{};
};

} // namespace detail

namespace {

using detail::FaceState;
using detail::TypefaceState;
using detail::FittedHalf;
using detail::FittedRecord;
using detail::LatticeHalf;
using detail::LatticeRecord;
using detail::PlacedGlyph;
using detail::TurnLattice;
using detail::refuse_font;

inline constexpr usize not_found = ~static_cast<usize>(0);
inline constexpr i64 largest_size = static_cast<i64>(1) << 24;
inline constexpr i32 largest_thickening = 1024;
// Coordinates beyond this (26.6) are not thickened: no mask is that large.
inline constexpr i64 thickening_reach = static_cast<i64>(1) << 22;

// What an outline takes of a half's storage.
[[nodiscard]] constexpr usize cost_of(const usize points) noexcept {
    const usize result = points * 9 + 96;
    return result;
}

// The whole number square root, rounded down.
[[nodiscard]] constexpr i64 root_of(const i64 value) noexcept {
    i64 root = 0;
    i64 bit = static_cast<i64>(1) << 62;
    i64 rest = value;
    while (bit > rest) bit = bit >> 2;
    while (bit != 0) {
        if (rest >= root + bit) {
            rest = rest - (root + bit);
            root = (root >> 1) + bit;
        } else {
            root = root >> 1;
        }
        bit = bit >> 2;
    }
    return root;
}

// The outward normal of the step (dx, dy), 16.16, for an outline whose
// contours' sum of cross products has the sign `sign`.
[[nodiscard]] Point normal_of(const i64 dx, const i64 dy, const i64 sign) noexcept {
    const i64 length = root_of(dx * dx + dy * dy);
    Point normal{};
    if (length > 0) normal = Point{.x = static_cast<i32>(sign * dy * 65536 / length), .y = static_cast<i32>(-sign * dx * 65536 / length)};
    return normal;
}

[[nodiscard]] bool same_point(const Point a, const Point b) noexcept {
    const bool result = a.x == b.x && a.y == b.y;
    return result;
}

// One point moved out by half the thickening, along the mean of the
// normals of the steps from the nearest point before it and to the nearest
// point after it that are not at it.
[[nodiscard]] Point moved_point(const Point before, const Point point, const Point after, const i64 half, const i64 sign) noexcept {
    const Point first_normal = normal_of(static_cast<i64>(point.x) - before.x, static_cast<i64>(point.y) - before.y, sign);
    const Point second_normal = normal_of(static_cast<i64>(after.x) - point.x, static_cast<i64>(after.y) - point.y, sign);
    const i64 sum_x = static_cast<i64>(first_normal.x) + second_normal.x;
    const i64 sum_y = static_cast<i64>(first_normal.y) + second_normal.y;
    const i64 product = static_cast<i64>(first_normal.x) * second_normal.x + static_cast<i64>(first_normal.y) * second_normal.y;
    // Rounded down to a 65536th.
    const i64 cosine = product >= 0 ? product / 65536 : -((-product + 65535) / 65536);
    const i64 divisor = 65536 + cosine < 16384 ? 16384 : 65536 + cosine;
    const i64 move_x = half * sum_x / divisor;
    const i64 move_y = half * sum_y / divisor;
    const Point result{.x = static_cast<i32>(point.x + move_x + half), .y = static_cast<i32>(point.y + move_y + half)};
    return result;
}

// The points of one contour, `first` to `last`, moved. Points at one place
// one after another (a run, which may go round from the last point to the
// first) have the same nearest points not at them: the point before the
// run and the point after it. The contour is gone round once from a point
// that begins a run, so the cost is linear in the contour. A contour all
// of whose points are at one place has none to move by: each point is
// moved by half the thickening alone.
void move_contour(const Outline& source, const usize first, const usize last, const i64 half, const i64 sign, Vector<Point>& target) noexcept {
    const usize length = last - first + 1;
    // A point that is not at the place of the one before it, going round.
    usize start = first;
    bool found = false;
    for (usize index = first; index <= last && !found; ++index) {
        const usize previous = index == first ? last : index - 1;
        found = !same_point(source.points[index], source.points[previous]);
        if (found) start = index;
    }
    if (!found) {
        for (usize index = first; index <= last; ++index) {
            const Point point = source.points[index];
            target[index] = Point{.x = static_cast<i32>(point.x + half), .y = static_cast<i32>(point.y + half)};
        }
        return;
    }
    usize walked = 0;
    usize run_start = start;
    while (walked < length) {
        // The run from run_start: as far as the points stay at its place.
        usize run_length = 1;
        usize next = run_start == last ? first : run_start + 1;
        while (walked + run_length < length && same_point(source.points[next], source.points[run_start])) {
            run_length = run_length + 1;
            next = next == last ? first : next + 1;
        }
        const usize before_index = run_start == first ? last : run_start - 1;
        const Point before = source.points[before_index];
        const Point after = source.points[next];
        usize index = run_start;
        for (usize step = 0; step < run_length; ++step) {
            target[index] = moved_point(before, source.points[index], after, half, sign);
            index = index == last ? first : index + 1;
        }
        walked = walked + run_length;
        run_start = next;
    }
}

// The rule for thicker strokes (glyph_face.hpp): `source` thickened by
// `thickening` into `target`. False: out of memory.
[[nodiscard]] bool thicken(const Outline& source, const i32 thickening, Vector<Point>& target) noexcept {
    const usize count = source.points.count();
    if (target.resize(count).failed()) return false;
    const i64 half = thickening / 2;
    bool in_reach = true;
    for (usize index = 0; index < count; ++index) {
        const Point point = source.points[index];
        target[index] = point;
        in_reach = in_reach && point.x > -thickening_reach && point.x < thickening_reach && point.y > -thickening_reach && point.y < thickening_reach;
    }
    if (!in_reach || half == 0 || count == 0) return true;
    // The contours: a move begins one, and it runs to the next move.
    const usize kinds = source.segments.count();
    i64 area = 0;
    for (usize pass = 0; pass < 2; ++pass) {
        const i64 sign = area > 0 ? 1 : -1;
        if (pass == 1 && area == 0) break;
        usize first = 0;
        usize next_point = 0;
        for (usize kind = 0; kind <= kinds; ++kind) {
            const bool ends = kind == kinds || source.segments[kind] == Segment::move;
            if (ends && next_point > first) {
                const usize last = next_point - 1;
                if (pass == 0) {
                    for (usize index = first; index <= last; ++index) {
                        const Point point = source.points[index];
                        const Point after = source.points[index == last ? first : index + 1];
                        area = area + static_cast<i64>(point.x) * after.y - static_cast<i64>(after.x) * point.y;
                    }
                } else {
                    move_contour(source, first, last, half, sign, target);
                }
            }
            if (ends) first = next_point;
            if (kind < kinds) {
                const Segment segment = source.segments[kind];
                const usize takes = segment == Segment::quadratic ? 2 : (segment == Segment::cubic ? 3 : 1);
                next_point = next_point + takes < count ? next_point + takes : count;
            }
        }
    }
    return true;
}

// a * b / 65536, rounded to the nearest, halves away from zero, and held
// to 2^30 either way (the reader's bound on a coordinate). For |a| below
// 2^31 and b no more than 2^36 (scale_of's most), the product is inside
// 64 bits.
[[nodiscard]] constexpr i32 times(const i64 a, const i64 b) noexcept {
    const i64 product = a * b;
    const i64 size = product < 0 ? -product : product;
    const i64 rounded = (size + 0x8000) >> 16;
    const i64 held = rounded > detail::farthest_point ? detail::farthest_point : rounded;
    const i32 result = static_cast<i32>(product < 0 ? -held : held);
    return result;
}

// The whole pixel a 26.6 coordinate is in.
[[nodiscard]] constexpr i32 pixel_of(const i32 value) noexcept {
    const i32 result = value >> 6;
    return result;
}

// Where in a table of `slots` (a power of two) a glyph at a size and fit
// begins to be looked for: a multiplicative hash, arithmetic modulo 2^64.
[[nodiscard]] STXLIB_MODULAR constexpr usize slot_of(const u32 glyph, const u32 size, const u32 fit, const usize slots) noexcept {
    u64 hash = glyph;
    hash = (hash * 0x9E3779B97F4A7C15ULL) ^ size;
    hash = (hash * 0x9E3779B97F4A7C15ULL) ^ fit;
    hash = hash * 0x9E3779B97F4A7C15ULL;
    const usize result = static_cast<usize>(hash >> 32) & (slots - 1);
    return result;
}

// The record's number in the half, or not_found.
[[nodiscard]] usize find(const FittedHalf& half, const u32 glyph, const u32 size, const u32 fit) noexcept {
    const usize slots = half.table.count();
    usize found = not_found;
    if (slots == 0) return found;
    usize slot = slot_of(glyph, size, fit, slots);
    // The table is never more than half full, so an empty slot ends this.
    for (usize probe = 0; probe < slots; ++probe) {
        const u32 entry = half.table[slot];
        if (entry == 0) break;
        const FittedRecord& record = half.records[entry - 1];
        if (record.glyph == glyph && record.size == size && record.fit == fit) {
            found = entry - 1;
            break;
        }
        slot = (slot + 1) & (slots - 1);
    }
    return found;
}

void empty(FittedHalf& half) noexcept {
    half.kinds.clear();
    half.points.clear();
    half.records.clear();
    for (usize index = 0; index < half.table.count(); ++index) half.table[index] = 0;
}

// The table's size for a half that holds at most `records`: a power of two
// at least twice that.
[[nodiscard]] constexpr usize slots_for(const usize records) noexcept {
    usize slots = 16;
    while (slots < records * 2 && slots < (static_cast<usize>(1) << 30)) slots = slots * 2;
    return slots;
}

void give(const FittedHalf& half, const FittedRecord& record, FittedGlyph& glyph_out) noexcept {
    glyph_out.outline.segments = View<Segment>(half.kinds.view().data() + record.first_kind, record.kind_count);
    glyph_out.outline.points = View<Point>(half.points.view().data() + record.first_point, record.point_count);
    glyph_out.advance = record.advance;
    glyph_out.left = record.left;
    glyph_out.bottom = record.bottom;
    glyph_out.right = record.right;
    glyph_out.top = record.top;
}

// The outline in work_kinds and work_points goes to the end of the half.
// False: out of memory, and the half is as it was.
[[nodiscard]] bool put(FaceState& state, const u32 half_number, FittedRecord record, const usize half_records) noexcept {
    FittedHalf& half = state.halves[half_number];
    // The table is as large as the records in it need (never more than
    // half full), and no larger than the half's most records would need:
    // it grows with them, each record put in again.
    const usize wanted = half.records.count() + 1 < half_records ? half.records.count() + 1 : half_records;
    const usize slots = slots_for(wanted);
    if (half.table.count() < slots) {
        if (half.table.resize(slots).failed()) return false;
        for (usize index = 0; index < slots; ++index) half.table[index] = 0;
        for (usize number = 0; number < half.records.count(); ++number) {
            const FittedRecord& held = half.records[number];
            usize place = slot_of(held.glyph, held.size, held.fit, slots);
            for (usize probe = 0; probe < slots && half.table[place] != 0; ++probe) place = (place + 1) & (slots - 1);
            half.table[place] = static_cast<u32>(number + 1);
        }
    }
    const usize first_kind = half.kinds.count();
    const usize first_point = half.points.count();
    bool held = true;
    for (usize index = 0; held && index < state.work_kinds.count(); ++index) held = half.kinds.push_copy(state.work_kinds[index]).succeeded();
    for (usize index = 0; held && index < state.work_points.count(); ++index) held = half.points.push_copy(state.work_points[index]).succeeded();
    record.first_kind = static_cast<u32>(first_kind);
    record.kind_count = static_cast<u32>(state.work_kinds.count());
    record.first_point = static_cast<u32>(first_point);
    record.point_count = static_cast<u32>(state.work_points.count());
    held = held && half.records.push_copy(record).succeeded();
    if (!held) {
        if (half.kinds.count() > first_kind) half.kinds.truncate(first_kind);
        if (half.points.count() > first_point) half.points.truncate(first_point);
        return false;
    }
    const usize table_slots = half.table.count();
    usize slot = slot_of(record.glyph, record.size, record.fit, table_slots);
    for (usize probe = 0; probe < table_slots && half.table[slot] != 0; ++probe) slot = (slot + 1) & (table_slots - 1);
    half.table[slot] = static_cast<u32>(half.records.count());
    state.held[half_number] = state.held[half_number] + cost_of(state.work_points.count());
    return true;
}

// The glyph's outline at the size, made afresh from the typeface into
// work_kinds and work_points; `record` gets its advance and its box.
[[nodiscard]] Status fit_outline(FaceState& state, const u32 glyph, const i64 size, const Fit fit, FittedRecord& record) noexcept {
    i32 advance = 0;
    const Status made = (*state.typeface).fit(glyph, size, fit, state.fitting, advance);
    if (made.failed()) return made;
    const Status cut = to_segments(state.fitting, state.work_kinds, state.work_points);
    if (cut.failed()) return cut;
    state.fittings = state.fittings + 1;
    record = FittedRecord{};
    record.glyph = glyph;
    record.size = static_cast<u32>(size);
    record.fit = static_cast<u32>(fit);
    record.advance = advance;
    const usize count = state.work_points.count();
    if (count > 0) {
        record.left = state.work_points[0].x;
        record.right = state.work_points[0].x;
        record.bottom = state.work_points[0].y;
        record.top = state.work_points[0].y;
        for (usize index = 1; index < count; ++index) {
            const Point point = state.work_points[index];
            record.left = point.x < record.left ? point.x : record.left;
            record.right = point.x > record.right ? point.x : record.right;
            record.bottom = point.y < record.bottom ? point.y : record.bottom;
            record.top = point.y > record.top ? point.y : record.top;
        }
    }
    return Status::success();
}

// The glyph's outline at the size: found among those held, or fitted and
// held.
[[nodiscard]] Status fitted_of(FaceState& state, const u32 glyph, const i64 size, const Fit fit, FittedGlyph& glyph_out) noexcept {
    const Operation operation("stxlib.glyphs.face");
    glyph_out = FittedGlyph{};
    if (glyph >= state.font.glyph_count || size <= 0 || size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    const u32 size_key = static_cast<u32>(size);
    const u32 fit_key = static_cast<u32>(fit);
    FittedHalf& newer = state.halves[state.newer];
    const usize in_newer = find(newer, glyph, size_key, fit_key);
    if (in_newer != not_found) {
        give(newer, newer.records[in_newer], glyph_out);
        return Status::success();
    }
    const u32 older_index = state.newer == 0 ? 1U : 0U;
    FittedHalf& older = state.halves[older_index];
    const usize in_older = find(older, glyph, size_key, fit_key);
    FittedRecord record{};
    if (in_older != not_found) {
        // Moved to the newer half, by way of the working storage.
        record = older.records[in_older];
        if (state.work_kinds.resize(record.kind_count).failed() || state.work_points.resize(record.point_count).failed()) {
            return refuse_font(ErrorKind::out_of_memory, operation);
        }
        for (usize index = 0; index < record.kind_count; ++index) state.work_kinds[index] = older.kinds[record.first_kind + index];
        for (usize index = 0; index < record.point_count; ++index) state.work_points[index] = older.points[record.first_point + index];
    } else {
        const Status made = fit_outline(state, glyph, size, fit, record);
        if (made.failed()) return made;
    }
    // A half holds a fifth of the limit: its storage grows by doubling,
    // and its table is a sixth as much again, so the two halves' storage
    // stays under the limit.
    const usize half_bytes = state.limits.fitted_bytes / 5;
    const usize half_records = half_bytes / 96;
    const usize cost = cost_of(state.work_points.count());
    if (cost > half_bytes || half_records == 0) {
        // Too large to hold: served from the working storage.
        glyph_out.outline.segments = state.work_kinds.view();
        glyph_out.outline.points = state.work_points.view();
        glyph_out.advance = record.advance;
        glyph_out.left = record.left;
        glyph_out.bottom = record.bottom;
        glyph_out.right = record.right;
        glyph_out.top = record.top;
        return Status::success();
    }
    if (state.held[state.newer] + cost > half_bytes || newer.records.count() >= half_records) {
        // The newer half is full: the older is given up and a new one
        // begun.
        empty(older);
        state.held[older_index] = 0;
        state.newer = older_index;
    }
    FittedHalf& target = state.halves[state.newer];
    if (!put(state, state.newer, record, half_records)) return refuse_font(ErrorKind::out_of_memory, operation);
    give(target, target.records[target.records.count() - 1], glyph_out);
    return Status::success();
}


// a * b + c * d over 65536, each product rounded on its own, halves away
// from zero: a coordinate through a row of a matrix.
[[nodiscard]] constexpr i64 fixed_product(const i64 a, const i64 b) noexcept {
    const i64 product = a * b;
    const i64 size = product < 0 ? -product : product;
    const i64 rounded = (size + 0x8000) >> 16;
    const i64 result = product < 0 ? -rounded : rounded;
    return result;
}

[[nodiscard]] constexpr i64 turned_sum(const i64 a, const i64 b, const i64 c, const i64 d) noexcept {
    const i64 result = fixed_product(a, b) + fixed_product(c, d);
    return result;
}

// Coordinates are kept within what a mask's limits can speak of.
[[nodiscard]] constexpr i32 far_clamped(const i64 value) noexcept {
    const i64 far = static_cast<i64>(1) << 30;
    const i32 result = static_cast<i32>(value < -far ? -far : (value > far ? far : value));
    return result;
}
// A glyph as it is drawn: its fitted outline, taken through the style's
// matrix if it has one (into `turned`) and thickened if the style says so
// (into `thick`).
[[nodiscard]] Status styled_outline(FaceState& state, const u32 glyph, const DrawStyle& style, Outline& outline) noexcept {
    const Operation operation("stxlib.glyphs.face");
    FittedGlyph fitted{};
    const Status had = fitted_of(state, glyph, style.size, style.fit, fitted);
    if (had.failed()) return had;
    outline = fitted.outline;
    if (style.transformed) {
        const usize count = fitted.outline.points.count();
        if (state.turned.resize(count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        // One fixed loop: every point through the matrix, whatever the
        // glyph. A result too far out to draw is held at the edge of what
        // can be measured, where the mask's limits refuse it.
        const i64 xx = style.xx;
        const i64 xy = style.xy;
        const i64 yx = style.yx;
        const i64 yy = style.yy;
        for (usize index = 0; index < count; ++index) {
            const Point point = fitted.outline.points[index];
            state.turned[index] = Point{.x = far_clamped(turned_sum(point.x, xx, point.y, xy)), .y = far_clamped(turned_sum(point.x, yx, point.y, yy))};
        }
        outline.points = state.turned.view();
    }
    if (style.thicken != 0) {
        if (!thicken(outline, style.thicken, state.thick)) return refuse_font(ErrorKind::out_of_memory, operation);
        outline.points = state.thick.view();
    }
    return Status::success();
}

// ---- quarter turns ---------------------------------------------------------------------------------------
//
// A matrix that is a quarter turn, a half turn or three quarters is not
// drawn from the turned outline: the upright mask is drawn, moved by the
// part of a pixel the turn would bring to where it is wanted, and turned
// pixel for pixel. So a quarter-turned glyph is the upright glyph's own
// bytes, hinting and all. (Drawn from the turned outline it would differ
// by a level here and there on curves, the drawing being worked a row at a
// time.) Thickened text does not go this way: its shift is along the
// surface's axes and does not turn.

// 1, 2 or 3 for that many quarter turns counter-clockwise; 0 for no matrix
// or the matrix of no change; 4 for any other matrix.
[[nodiscard]] u32 quarter_of(const DrawStyle& style) noexcept {
    if (!style.transformed) return 0;
    const i32 one = 0x10000;
    u32 result = 4;
    if (style.xx == one && style.xy == 0 && style.yx == 0 && style.yy == one) result = 0;
    if (style.xx == 0 && style.xy == -one && style.yx == one && style.yy == 0) result = 1;
    if (style.xx == -one && style.xy == 0 && style.yx == 0 && style.yy == -one) result = 2;
    if (style.xx == 0 && style.xy == one && style.yx == -one && style.yy == 0) result = 3;
    if (result != 0 && result != 4 && style.thicken != 0) result = 4;
    return result;
}

// What the upright outline is moved by so that, turned, it is moved by
// `shift`: the shift through the turn the other way.
[[nodiscard]] constexpr Point shift_before_quarter(const u32 quarter, const Point shift) noexcept {
    Point result = shift;
    if (quarter == 1) result = Point{.x = shift.y, .y = -shift.x};
    if (quarter == 2) result = Point{.x = -shift.x, .y = -shift.y};
    if (quarter == 3) result = Point{.x = -shift.y, .y = shift.x};
    return result;
}

// The box of the turned mask from the upright one's: (x, y) goes to
// (-y, x), to (-x, -y), or to (y, -x).
[[nodiscard]] constexpr Bounds bounds_after_quarter(const u32 quarter, const Bounds& upright) noexcept {
    const i32 width = static_cast<i32>(upright.width);
    const i32 height = static_cast<i32>(upright.height);
    Bounds result = upright;
    if (quarter == 1) result = Bounds{.left = -upright.top, .top = upright.left + width, .width = upright.height, .height = upright.width};
    if (quarter == 2) result = Bounds{.left = -(upright.left + width), .top = height - upright.top, .width = upright.width, .height = upright.height};
    if (quarter == 3) result = Bounds{.left = upright.top - height, .top = -upright.left, .width = upright.height, .height = upright.width};
    return result;
}

[[nodiscard]] constexpr Bounds bounds_before_quarter(const u32 quarter, const Bounds& turned) noexcept {
    const i32 width = static_cast<i32>(turned.width);
    const i32 height = static_cast<i32>(turned.height);
    Bounds result = turned;
    if (quarter == 1) result = Bounds{.left = turned.top - height, .top = -turned.left, .width = turned.height, .height = turned.width};
    if (quarter == 2) result = Bounds{.left = -(turned.left + width), .top = height - turned.top, .width = turned.width, .height = turned.height};
    if (quarter == 3) result = Bounds{.left = -turned.top, .top = turned.left + width, .width = turned.height, .height = turned.width};
    return result;
}

// The upright mask's bytes into the turned mask's places: one loop for
// each of the three turns, none of which asks anything of a pixel.
void turn_pixels(const u32 quarter, const Bounds& upright, const u8* const from, u8* const to) noexcept {
    const usize width = upright.width;
    const usize height = upright.height;
    if (quarter == 1) {
        // Rows become columns from the left; columns, rows from the bottom.
        for (usize row = 0; row < height; ++row) {
            for (usize column = 0; column < width; ++column) to[(width - 1 - column) * height + row] = from[row * width + column];
        }
    } else if (quarter == 2) {
        const usize count = width * height;
        for (usize index = 0; index < count; ++index) to[count - 1 - index] = from[index];
    } else if (quarter == 3) {
        // Rows become columns from the right; columns, rows from the top.
        for (usize row = 0; row < height; ++row) {
            for (usize column = 0; column < width; ++column) to[column * height + (height - 1 - row)] = from[row * width + column];
        }
    }
}

// ---- turned by the surface ---------------------------------------------------------------------------------
//
// Lightly fitted text at any other turn is not drawn from its turned
// outline either: the upright mask (at no part of a pixel) is made a
// surface once (turn_lattice.hpp), kept, and the turned mask read from
// it. The lattices are held as the fitted outlines are.

// How a style's glyphs are drawn, settled once for a call.
enum class Route : u8 {
    // From the fitted outline, through the matrix if there is one.
    outline = 0,
    // The upright mask turned pixel for pixel.
    quarters = 1,
    // The upright mask's surface, turned.
    surface = 2,
};

struct Drawing final {
    Route route{Route::outline};
    u32 quarter{0};
};

[[nodiscard]] Drawing drawing_of(const FaceState& state, const DrawStyle& style) noexcept {
    Drawing drawing{};
    drawing.quarter = quarter_of(style);
    if (drawing.quarter >= 1 && drawing.quarter <= 3) {
        drawing.route = Route::quarters;
    } else if (drawing.quarter == 4 && style.fit == Fit::light && style.size <= state.limits.lattice_size &&
               detail::is_turn(style.xx, style.xy, style.yx, style.yy)) {
        drawing.route = Route::surface;
    }
    return drawing;
}

// What a lattice takes of a half's storage.
[[nodiscard]] constexpr usize lattice_cost(const usize controls) noexcept {
    const usize result = controls * sizeof(i16) + 64;
    return result;
}

[[nodiscard]] usize find_lattice(const LatticeHalf& half, const u32 glyph, const u32 size, const u32 thickening) noexcept {
    const usize slots = half.table.count();
    usize found = not_found;
    if (slots == 0) return found;
    usize slot = slot_of(glyph, size, thickening, slots);
    // The table is never more than half full, so an empty slot ends this.
    for (usize probe = 0; probe < slots; ++probe) {
        const u32 entry = half.table[slot];
        if (entry == 0) break;
        const LatticeRecord& record = half.records[entry - 1];
        if (record.glyph == glyph && record.size == size && record.thicken == thickening) {
            found = entry - 1;
            break;
        }
        slot = (slot + 1) & (slots - 1);
    }
    return found;
}

void empty(LatticeHalf& half) noexcept {
    half.controls.clear();
    half.records.clear();
    for (usize index = 0; index < half.table.count(); ++index) half.table[index] = 0;
}

// The lattice in lattice_work goes to the end of the half. False: out of
// memory, and the half is as it was.
[[nodiscard]] bool put_lattice(FaceState& state, const u32 half_number, LatticeRecord record, const usize half_records) noexcept {
    LatticeHalf& half = state.lattices[half_number];
    const usize wanted = half.records.count() + 1 < half_records ? half.records.count() + 1 : half_records;
    const usize slots = slots_for(wanted);
    if (half.table.count() < slots) {
        if (half.table.resize(slots).failed()) return false;
        for (usize index = 0; index < slots; ++index) half.table[index] = 0;
        for (usize number = 0; number < half.records.count(); ++number) {
            const LatticeRecord& held = half.records[number];
            usize place = slot_of(held.glyph, held.size, held.thicken, slots);
            for (usize probe = 0; probe < slots && half.table[place] != 0; ++probe) place = (place + 1) & (slots - 1);
            half.table[place] = static_cast<u32>(number + 1);
        }
    }
    const usize first = half.controls.count();
    const usize count = state.lattice_work.count();
    if (half.controls.resize(first + count).failed()) return false;
    record.first = static_cast<u32>(first);
    if (half.records.push_copy(record).failed()) {
        half.controls.truncate(first);
        return false;
    }
    i16* const to = half.controls.mutable_view().data() + first;
    const i16* const from = state.lattice_work.view().data();
    for (usize index = 0; index < count; ++index) to[index] = from[index];
    const usize table_slots = half.table.count();
    usize slot = slot_of(record.glyph, record.size, record.thicken, table_slots);
    for (usize probe = 0; probe < table_slots && half.table[slot] != 0; ++probe) slot = (slot + 1) & (table_slots - 1);
    half.table[slot] = static_cast<u32>(half.records.count());
    state.lattices_held[half_number] = state.lattices_held[half_number] + lattice_cost(count);
    return true;
}

// The box of the glyph's upright mask at no part of a pixel, in the
// upright style (the style without its matrix): from its lattice if one is
// held, else measured.
[[nodiscard]] Status upright_box(FaceState& state, const u32 glyph, const DrawStyle& upright_style, Bounds& upright) noexcept {
    const u32 size_key = static_cast<u32>(upright_style.size);
    const u32 thick_key = static_cast<u32>(upright_style.thicken);
    for (u32 number = 0; number < 2; ++number) {
        const LatticeHalf& half = state.lattices[number];
        const usize found = find_lattice(half, glyph, size_key, thick_key);
        if (found != not_found) {
            upright = half.records[found].upright;
            return Status::success();
        }
    }
    Outline outline{};
    const Status had = styled_outline(state, glyph, upright_style, outline);
    if (had.failed()) return had;
    const Status measured = measure(outline, Point{}, state.limits.mask, upright);
    return measured;
}

// Whether a glyph with this upright box is turned by its surface: it has
// ink, and is not too large for a lattice.
[[nodiscard]] constexpr bool takes_lattice(const Bounds& upright) noexcept {
    const bool result = upright.width >= 1 && upright.height >= 1 && upright.width <= detail::lattice_side && upright.height <= detail::lattice_side;
    return result;
}

// The glyph's lattice: found among those held, or made from its upright
// mask and held. What is given is good until the next call on the face. A
// glyph that takes none (no ink, or too large) is given its upright box and
// no controls.
[[nodiscard]] Status lattice_of(FaceState& state, const u32 glyph, const DrawStyle& upright_style, TurnLattice& lattice) noexcept {
    const Operation operation("stxlib.glyphs.face");
    lattice = TurnLattice{};
    const u32 size_key = static_cast<u32>(upright_style.size);
    const u32 thick_key = static_cast<u32>(upright_style.thicken);
    LatticeHalf& newer = state.lattices[state.lattices_newer];
    const usize in_newer = find_lattice(newer, glyph, size_key, thick_key);
    if (in_newer != not_found) {
        const LatticeRecord& record = newer.records[in_newer];
        lattice = TurnLattice{.controls = newer.controls.view().data() + record.first, .upright = record.upright};
        return Status::success();
    }
    const u32 older_index = state.lattices_newer == 0 ? 1U : 0U;
    LatticeHalf& older = state.lattices[older_index];
    const usize in_older = find_lattice(older, glyph, size_key, thick_key);
    LatticeRecord record{};
    if (in_older != not_found) {
        // Moved to the newer half, by way of the working storage.
        record = older.records[in_older];
        const usize count = detail::lattice_controls(record.upright.width, record.upright.height);
        if (state.lattice_work.resize(count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        const i16* const from = older.controls.view().data() + record.first;
        i16* const to = state.lattice_work.mutable_view().data();
        for (usize index = 0; index < count; ++index) to[index] = from[index];
    } else {
        // The upright mask at no part of a pixel, and its surface.
        Outline outline{};
        const Status had = styled_outline(state, glyph, upright_style, outline);
        if (had.failed()) return had;
        Bounds upright{};
        const Status measured = measure(outline, Point{}, state.limits.mask, upright);
        if (measured.failed()) return measured;
        if (!takes_lattice(upright)) {
            lattice.upright = upright;
            return Status::success();
        }
        if (state.glyph_pixels.resize(static_cast<usize>(upright.width) * upright.height).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        const Status drawn = state.rasterizer.render(outline, Point{}, upright, state.limits.mask, state.glyph_pixels.mutable_view());
        if (drawn.failed()) return drawn;
        if (!detail::build_lattice(state.glyph_pixels.view().data(), upright.width, upright.height, state.lattice_rows, state.lattice_work)) {
            return refuse_font(ErrorKind::out_of_memory, operation);
        }
        state.lattices_made = state.lattices_made + 1;
        record.glyph = glyph;
        record.size = size_key;
        record.thicken = thick_key;
        record.upright = upright;
    }
    // A half holds a fifth of the limit, as a half of the fitted outlines
    // does and for the same reason.
    const usize half_bytes = state.limits.lattice_bytes / 5;
    const usize half_records = half_bytes / 64;
    const usize cost = lattice_cost(state.lattice_work.count());
    if (cost > half_bytes || half_records == 0) {
        // Too large to hold: served from the working storage.
        lattice = TurnLattice{.controls = state.lattice_work.view().data(), .upright = record.upright};
        return Status::success();
    }
    if (state.lattices_held[state.lattices_newer] + cost > half_bytes || newer.records.count() >= half_records) {
        empty(older);
        state.lattices_held[older_index] = 0;
        state.lattices_newer = older_index;
    }
    LatticeHalf& target = state.lattices[state.lattices_newer];
    if (!put_lattice(state, state.lattices_newer, record, half_records)) return refuse_font(ErrorKind::out_of_memory, operation);
    const LatticeRecord& kept = target.records[target.records.count() - 1];
    lattice = TurnLattice{.controls = target.controls.view().data() + kept.first, .upright = kept.upright};
    return Status::success();
}

// Whether a style can be drawn in at all.
[[nodiscard]] bool style_is_valid(const DrawStyle& style) noexcept {
    const bool result = style.size > 0 && style.size <= largest_size && style.thicken >= 0 && style.thicken <= largest_thickening;
    return result;
}

[[nodiscard]] bool out_of_memory(const Status& status) noexcept {
    const bool result = status.failed() && (*status.error()).kind() == ErrorKind::out_of_memory;
    return result;
}

// Every glyph of the run is placed (state.placed) and the run's box taken.
[[nodiscard]] Status place_run(FaceState& state, const View<RunGlyph> glyphs, const DrawStyle& style, RunBox& box) noexcept {
    const Operation operation("stxlib.glyphs.face");
    box = RunBox{};
    if (!style_is_valid(style)) return refuse_font(ErrorKind::invalid_argument, operation);
    if (state.placed.resize(glyphs.count()).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    bool any = false;
    i64 left = 0;
    i64 top = 0;
    i64 right = 0;
    i64 bottom = 0;
    // How the run's glyphs are drawn is settled once: upright, by quarter
    // turns of the upright mask, or through the matrix.
    const Drawing drawing = drawing_of(state, style);
    const u32 quarter = drawing.quarter;
    const bool by_quarters = drawing.route == Route::quarters;
    const bool by_surfaces = drawing.route == Route::surface;
    DrawStyle upright_style = style;
    upright_style.transformed = false;
    for (usize index = 0; index < glyphs.count(); ++index) {
        RunGlyph glyph = glyphs[index];
        if (style.transformed && style.turn_places) {
            const TextMatrix matrix{.xx = style.xx, .xy = style.xy, .yx = style.yx, .yy = style.yy};
            const Point turned = turn_place(matrix, Point{.x = glyph.x, .y = glyph.y});
            glyph.x = turned.x;
            glyph.y = turned.y;
        }
        PlacedGlyph placed{};
        placed.pixel_x = pixel_of(glyph.x);
        placed.pixel_y = pixel_of(glyph.y);
        // What is left of a pixel: rightward, and downward, which on the
        // outline's axes is toward less y.
        placed.shift = Point{.x = glyph.x - placed.pixel_x * 64, .y = -(glyph.y - placed.pixel_y * 64)};
        Outline outline{};
        Status had = Status::success();
        Status measured = Status::success();
        if (by_quarters) {
            // The upright glyph's box, turned.
            Bounds upright{};
            had = styled_outline(state, glyph.glyph, upright_style, outline);
            measured = had;
            if (had.succeeded()) measured = measure(outline, shift_before_quarter(quarter, placed.shift), state.limits.mask, upright);
            if (measured.succeeded()) placed.bounds = bounds_after_quarter(quarter, upright);
        } else {
            // By its surface: the upright mask's box, through the turn. A
            // glyph too large for a lattice goes by its outline.
            Bounds upright{};
            if (by_surfaces) {
                had = upright_box(state, glyph.glyph, upright_style, upright);
                measured = had;
                placed.by_surface = had.succeeded() && takes_lattice(upright);
                if (placed.by_surface && !detail::turned_bounds(upright, style.xx, style.yx, placed.shift, state.limits.mask, placed.bounds)) {
                    measured = refuse_font(ErrorKind::limit_exceeded, operation);
                }
            }
            if (!placed.by_surface && had.succeeded() && !(by_surfaces && (upright.width == 0 || upright.height == 0))) {
                had = styled_outline(state, glyph.glyph, style, outline);
                measured = had;
                if (had.succeeded()) measured = measure(outline, placed.shift, state.limits.mask, placed.bounds);
            }
        }
        if (out_of_memory(had) || out_of_memory(measured)) return refuse_font(ErrorKind::out_of_memory, operation);
        if (measured.failed()) {
            box.missing_glyphs = box.missing_glyphs + 1;
            state.placed[index] = placed;
            continue;
        }
        if (placed.bounds.width == 0 || placed.bounds.height == 0) {
            state.placed[index] = placed;
            continue;
        }
        // The mask's pixels on the surface: rows run downward.
        const i64 glyph_left = static_cast<i64>(placed.pixel_x) + placed.bounds.left;
        const i64 glyph_top = static_cast<i64>(placed.pixel_y) - placed.bounds.top;
        const i64 glyph_right = glyph_left + placed.bounds.width;
        const i64 glyph_bottom = glyph_top + placed.bounds.height;
        if (style.windowed && (glyph_right <= style.window_left || glyph_left >= style.window_right || glyph_bottom <= style.window_top ||
                               glyph_top >= style.window_bottom)) {
            box.outside_glyphs = box.outside_glyphs + 1;
            state.placed[index] = placed;
            continue;
        }
        placed.shown = true;
        state.placed[index] = placed;
        left = (!any || glyph_left < left) ? glyph_left : left;
        top = (!any || glyph_top < top) ? glyph_top : top;
        right = (!any || glyph_right > right) ? glyph_right : right;
        bottom = (!any || glyph_bottom > bottom) ? glyph_bottom : bottom;
        any = true;
    }
    if (!any) return Status::success();
    if (right - left > state.limits.mask.width || bottom - top > state.limits.mask.height || left < -0x40000000LL || top < -0x40000000LL ||
        right > 0x40000000LL || bottom > 0x40000000LL) {
        box.oversized = true;
        return Status::success();
    }
    box.left = static_cast<i32>(left);
    box.top = static_cast<i32>(top);
    box.right = static_cast<i32>(right);
    box.bottom = static_cast<i32>(bottom);
    return Status::success();
}

[[nodiscard]] usize bytes_of(const FittedHalf& half) noexcept {
    const usize result = half.kinds.capacity() * sizeof(Segment) + half.points.capacity() * sizeof(Point) +
                         half.records.capacity() * sizeof(FittedRecord) + half.table.capacity() * sizeof(u32);
    return result;
}

[[nodiscard]] usize bytes_of(const LatticeHalf& half) noexcept {
    const usize result = half.controls.capacity() * sizeof(i16) + half.records.capacity() * sizeof(LatticeRecord) + half.table.capacity() * sizeof(u32);
    return result;
}

[[nodiscard]] usize bytes_of(const TypefaceState& state) noexcept {
    const usize result = sizeof(TypefaceState) + state.store.bytes_held() + state.hinter.bytes_held() +
                         state.measuring.points.capacity() * sizeof(Point) + state.measuring.tags.capacity() * sizeof(PointTag) +
                         state.measuring.contour_ends.capacity() * sizeof(u32);
    return result;
}

[[nodiscard]] usize bytes_of(const FaceState& state) noexcept {
    const usize result = sizeof(FaceState) + state.rasterizer.bytes_held() + bytes_of(state.halves[0]) + bytes_of(state.halves[1]) +
                         state.fitting.points.capacity() * sizeof(Point) + state.fitting.tags.capacity() * sizeof(PointTag) +
                         state.fitting.contour_ends.capacity() * sizeof(u32) + state.work_kinds.capacity() * sizeof(Segment) +
                         state.work_points.capacity() * sizeof(Point) + state.turned.capacity() * sizeof(Point) +
                         state.thick.capacity() * sizeof(Point) + state.glyph_pixels.capacity() + state.turned_pixels.capacity() + state.placed.capacity() * sizeof(PlacedGlyph) +
                         bytes_of(state.lattices[0]) + bytes_of(state.lattices[1]) + state.lattice_rows.capacity() * sizeof(i64) +
                         state.lattice_work.capacity() * sizeof(i16);
    return result;
}

// Whether the fitted outlines a face holds agree with themselves
// (Face::whole).
[[nodiscard]] bool fitted_whole(const FaceState& state) noexcept {
    bool good = state.newer < 2;
    const usize half_bytes = state.limits.fitted_bytes / 5;
    for (u32 number = 0; good && number < 2; ++number) {
        const FittedHalf& half = state.halves[number];
        usize kinds = 0;
        usize points = 0;
        usize cost = 0;
        for (usize index = 0; good && index < half.records.count(); ++index) {
            const FittedRecord& record = half.records[index];
            good = record.first_kind == kinds && record.first_point == points && record.kind_count <= half.kinds.count() - kinds &&
                   record.point_count <= half.points.count() - points;
            // As many points as its segments need.
            usize needed = 0;
            for (usize kind = 0; good && kind < record.kind_count; ++kind) {
                const Segment segment = half.kinds[kinds + kind];
                needed = needed + (segment == Segment::quadratic ? 2 : (segment == Segment::cubic ? 3 : 1));
            }
            good = good && needed == record.point_count && find(half, record.glyph, record.size, record.fit) == index;
            kinds = kinds + record.kind_count;
            points = points + record.point_count;
            cost = cost + cost_of(record.point_count);
        }
        good = good && kinds == half.kinds.count() && points == half.points.count() && cost == state.held[number] && cost <= half_bytes;
        // The table: nothing, or a power of two no more than half full,
        // each of its entries a record and each record in it once.
        const usize slots = half.table.count();
        usize used = 0;
        for (usize slot = 0; good && slot < slots; ++slot) {
            const u32 entry = half.table[slot];
            good = entry <= half.records.count();
            used = used + (entry != 0 ? 1U : 0U);
        }
        const bool power_of_two = slots != 0 && (slots & (slots - 1)) == 0;
        good = good && used == half.records.count() && (slots == 0 ? used == 0 : (power_of_two && used * 2 <= slots));
    }
    return good;
}

// Whether the lattices a face holds agree with themselves (Face::whole):
// each half's lattices lie end to end, each as many controls as its box
// needs and a box a lattice can have, each found by its glyph, size and
// thickening, the bytes counted right and within the limit, and the table
// a power of two no more than half full with each record in it once.
[[nodiscard]] bool lattices_whole(const FaceState& state) noexcept {
    bool good = state.lattices_newer < 2;
    const usize half_bytes = state.limits.lattice_bytes / 5;
    for (u32 number = 0; good && number < 2; ++number) {
        const LatticeHalf& half = state.lattices[number];
        usize controls = 0;
        usize cost = 0;
        for (usize index = 0; good && index < half.records.count(); ++index) {
            const LatticeRecord& record = half.records[index];
            good = takes_lattice(record.upright) && record.first == controls;
            const usize count = good ? detail::lattice_controls(record.upright.width, record.upright.height) : 0;
            good = good && count <= half.controls.count() - controls && find_lattice(half, record.glyph, record.size, record.thicken) == index;
            controls = controls + count;
            cost = cost + lattice_cost(count);
        }
        good = good && controls == half.controls.count() && cost == state.lattices_held[number] && cost <= half_bytes;
        const usize slots = half.table.count();
        usize used = 0;
        for (usize slot = 0; good && slot < slots; ++slot) {
            const u32 entry = half.table[slot];
            good = entry <= half.records.count();
            used = used + (entry != 0 ? 1U : 0U);
        }
        const bool power_of_two = slots != 0 && (slots & (slots - 1)) == 0;
        good = good && used == half.records.count() && (slots == 0 ? used == 0 : (power_of_two && used * 2 <= slots));
    }
    return good;
}

void note_peak(TypefaceState& state) noexcept {
    const usize now = bytes_of(state);
    state.peak = now > state.peak ? now : state.peak;
}

void note_peak(FaceState& state) noexcept {
    const usize now = bytes_of(state);
    state.peak = now > state.peak ? now : state.peak;
}

} // namespace

// ---- the program's typeface ---------------------------------------------------------------------

// ---- turned text: the matrix of an angle, places, the way back, boxes ---------------------------------

namespace {

inline constexpr i64 quarter_turn = static_cast<i64>(90) << 16;
inline constexpr i64 full_turn = static_cast<i64>(360) << 16;
inline constexpr i64 one_q30 = static_cast<i64>(1) << 30;
// pi / 180 * 2^44, to the nearest whole number.
inline constexpr i64 radians_per_degree_q44 = 307041569098LL;

// Two numbers of 30 fraction bits, neither negative, multiplied.
[[nodiscard]] constexpr i64 times_q30(const i64 a, const i64 b) noexcept {
    const i64 result = (a * b) >> 30;
    return result;
}

// The sine and cosine of an angle from 0 to 45 degrees (16.16), as 16.16:
// the series of each to the eleventh and twelfth power, in whole numbers
// of 30 fraction bits, rounded once at the end. Every step is the same on
// every host; the result is within 0.5001 of a 65536th of the true value.
void sine_cosine(const i64 degrees, i32& sine, i32& cosine) noexcept {
    const i64 x = (degrees * radians_per_degree_q44) >> 30;
    const i64 z = times_q30(x, x);
    i64 p = one_q30 - z / 110;
    p = one_q30 - times_q30(z, p) / 72;
    p = one_q30 - times_q30(z, p) / 42;
    p = one_q30 - times_q30(z, p) / 20;
    p = one_q30 - times_q30(z, p) / 6;
    const i64 s = times_q30(x, p);
    p = one_q30 - z / 132;
    p = one_q30 - times_q30(z, p) / 90;
    p = one_q30 - times_q30(z, p) / 56;
    p = one_q30 - times_q30(z, p) / 30;
    p = one_q30 - times_q30(z, p) / 12;
    const i64 c = one_q30 - times_q30(z, p) / 2;
    sine = static_cast<i32>((s + 8192) >> 14);
    cosine = static_cast<i32>((c + 8192) >> 14);
}

// n / d to the nearest whole number, halves away from zero (d not 0).
[[nodiscard]] constexpr i64 rounded_quotient(const i64 n, const i64 d) noexcept {
    const i64 quotient = n / d;
    const i64 rest = n % d;
    const i64 rest_size = rest < 0 ? -rest : rest;
    const i64 d_size = d < 0 ? -d : d;
    const bool away = rest_size >= d_size - rest_size;
    const bool negative = (n < 0) != (d < 0);
    const i64 result = away ? (negative ? quotient - 1 : quotient + 1) : quotient;
    return result;
}

} // namespace

TextMatrix turn_matrix(const i64 degrees) noexcept {
    // The angle within one turn, the quarter it is in, and how far into
    // that quarter: the first half of a quarter is worked out directly,
    // the second from its complement, so that sines and cosines of angles
    // that add to a quarter turn are the same numbers.
    i64 within = degrees % full_turn;
    if (within < 0) within = within + full_turn;
    const i64 quarter = within / quarter_turn;
    const i64 rest = within % quarter_turn;
    i32 s = 0;
    i32 c = 0x10000;
    if (rest <= quarter_turn / 2) {
        sine_cosine(rest, s, c);
    } else {
        sine_cosine(quarter_turn - rest, c, s);
    }
    i32 sine = s;
    i32 cosine = c;
    if (quarter == 1) {
        sine = c;
        cosine = -s;
    } else if (quarter == 2) {
        sine = -s;
        cosine = -c;
    } else if (quarter == 3) {
        sine = -c;
        cosine = s;
    }
    const TextMatrix result{.xx = cosine, .xy = -sine, .yx = sine, .yy = cosine};
    return result;
}

Point turn_place(const TextMatrix& matrix, const Point place) noexcept {
    // The surface's y runs downward, the matrix's upward.
    const i64 x = place.x;
    const i64 up = -static_cast<i64>(place.y);
    const i64 turned_x = turned_sum(x, matrix.xx, up, matrix.xy);
    const i64 turned_up = turned_sum(x, matrix.yx, up, matrix.yy);
    const Point result{.x = far_clamped(turned_x), .y = far_clamped(-turned_up)};
    return result;
}

bool invert_matrix(const TextMatrix& matrix, TextMatrix& inverse) noexcept {
    inverse = TextMatrix{};
    const i64 least = -2147483647LL - 1;
    if (matrix.xx == least || matrix.xy == least || matrix.yx == least || matrix.yy == least) return false;
    const i64 determinant = static_cast<i64>(matrix.xx) * matrix.yy - static_cast<i64>(matrix.xy) * matrix.yx;
    if (determinant == 0) return false;
    // Each entry of the adjugate over the determinant, as 16.16.
    const i64 xx = rounded_quotient(static_cast<i64>(matrix.yy) << 32, determinant);
    const i64 xy = rounded_quotient(-static_cast<i64>(matrix.xy) << 32, determinant);
    const i64 yx = rounded_quotient(-static_cast<i64>(matrix.yx) << 32, determinant);
    const i64 yy = rounded_quotient(static_cast<i64>(matrix.xx) << 32, determinant);
    const i64 most = 2147483647LL;
    if (xx > most || xx < -most || xy > most || xy < -most || yx > most || yx < -most || yy > most || yy < -most) return false;
    inverse = TextMatrix{.xx = static_cast<i32>(xx), .xy = static_cast<i32>(xy), .yx = static_cast<i32>(yx), .yy = static_cast<i32>(yy)};
    return true;
}

PlaceBox turn_box(const TextMatrix& matrix, const PlaceBox& box) noexcept {
    const Point corners[4] = {turn_place(matrix, Point{.x = box.left, .y = box.top}), turn_place(matrix, Point{.x = box.right, .y = box.top}),
                              turn_place(matrix, Point{.x = box.left, .y = box.bottom}), turn_place(matrix, Point{.x = box.right, .y = box.bottom})};
    PlaceBox result{.left = corners[0].x, .top = corners[0].y, .right = corners[0].x, .bottom = corners[0].y};
    for (usize index = 1; index < 4; ++index) {
        result.left = corners[index].x < result.left ? corners[index].x : result.left;
        result.right = corners[index].x > result.right ? corners[index].x : result.right;
        result.top = corners[index].y < result.top ? corners[index].y : result.top;
        result.bottom = corners[index].y > result.bottom ? corners[index].y : result.bottom;
    }
    return result;
}

Typeface::Typeface() noexcept = default;
Typeface::Typeface(Typeface&&) noexcept = default;
Typeface& Typeface::operator=(Typeface&&) noexcept = default;
Typeface::~Typeface() noexcept = default;

Status Typeface::open(const View<u8> bytes, const u32 face_index, const TypefaceLimits& limits) noexcept {
    const Operation operation("stxlib.glyphs.typeface");
    if (!state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    Result<Unique<detail::TypefaceState>> made = Unique<detail::TypefaceState>::make();
    if (made.failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    Unique<detail::TypefaceState> fresh = static_cast<Result<Unique<detail::TypefaceState>>&&>(made).take_value();
    TypefaceState& state = *fresh;
    state.limits = limits;
    const Status opened = open_font(bytes, face_index, state.font);
    if (opened.failed()) return opened;
    const Status stored = state.store.open(state.font, limits.font, limits.store);
    if (stored.failed()) return stored;
    note_peak(state);
    state_ = static_cast<Unique<detail::TypefaceState>&&>(fresh);
    return Status::success();
}

const Font* Typeface::font() const noexcept {
    const Font* const result = state_.empty() ? nullptr : &(*state_).font;
    return result;
}

Status Typeface::keep(const View<u32> characters, usize& kept) noexcept {
    const Operation operation("stxlib.glyphs.typeface");
    kept = 0;
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    const Status status = state.store.keep(state.font, characters, kept);
    note_peak(state);
    return status;
}

Status Typeface::fit(const u32 glyph, const i64 size, const Fit fit, GlyphOutline& outline, i32& advance) noexcept {
    const Operation operation("stxlib.glyphs.typeface");
    advance = 0;
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    TypefaceState& state = *state_;
    if (glyph >= state.font.glyph_count || size <= 0 || size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    const ScopedLock held(state.lock);
    if (fit == Fit::none && state.font.outlines == OutlineKind::truetype) {
        // A TrueType glyph's parts are scaled each on its own: read at the
        // size, as its scaler has it.
        const i64 scale = scale_of(state.font, size);
        const Status scaled = load_scaled_glyph(state.font, glyph, scale, scale, state.limits.font, outline);
        if (scaled.succeeded()) advance = times(advance_of(state.font, glyph), scale);
        note_peak(state);
        return scaled;
    }
    StoredGlyph stored{};
    Status status = state.store.glyph(state.font, glyph, stored);
    if (status.succeeded() && fit == Fit::none) {
        // Scaled as load_scaled_glyph scales a CFF glyph (scale_outline:
        // the same rounding as times, and the reader's bound on the result).
        const usize point_count = stored.points.count();
        const i64 scale = scale_of(state.font, size);
        bool room = outline.points.resize(point_count).succeeded() && outline.tags.resize(point_count).succeeded();
        for (usize index = 0; room && index < point_count; ++index) {
            outline.points[index] = stored.points[index];
            outline.tags[index] = stored.tags[index];
        }
        for (usize index = 0; room && index < stored.contour_ends.count(); ++index) room = outline.contour_ends.push_copy(stored.contour_ends[index]).succeeded();
        advance = times(stored.advance, scale);
        if (!room) {
            status = refuse_font(ErrorKind::out_of_memory, operation);
        } else {
            status = scale_outline(outline, scale, scale);
        }
        if (status.failed()) {
            outline.points.clear();
            outline.tags.clear();
            outline.contour_ends.clear();
            advance = 0;
        }
    } else if (status.succeeded()) {
        // The hinter is prepared when the first glyph is fitted.
        if (!state.prepared) {
            status = state.hinter.prepare(state.font);
            state.prepared = status.succeeded();
        }
        if (status.succeeded()) {
            const HintMode mode = fit == Fit::monochrome ? HintMode::monochrome : HintMode::light;
            status = state.hinter.hint_stored(state.font, glyph, stored, size, mode, state.limits.font, outline, advance);
        }
    }
    note_peak(state);
    STXLIB_GLYPHS_CHECK(status.failed() || detail::outline_whole(outline), "Typeface::fit: the outline is not whole");
    STXLIB_GLYPHS_CHECK(state.store.whole(), "Typeface::fit: the store is not whole");
    return status;
}

bool Typeface::whole() const noexcept {
    if (state_.empty()) return true;
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    const bool result = state.store.whole();
    return result;
}

Status Typeface::ink_box(const u32 glyph, const i64 size, InkBox& box) noexcept {
    const Operation operation("stxlib.glyphs.typeface");
    box = InkBox{};
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    TypefaceState& state = *state_;
    if (glyph >= state.font.glyph_count || size <= 0 || size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    const ScopedLock held(state.lock);
    const i64 scale = scale_of(state.font, size);
    if (state.font.outlines == OutlineKind::cff) {
        // A CFF glyph from the store, so that its charstring is run once
        // (and a refusal remembered): its box in font units, scaled.
        // Scaling is rounding of a product by a scale above 0, which never
        // puts two points in the other order, so the box of the scaled
        // points is the scaled box, and it is load_scaled_glyph's.
        StoredGlyph stored{};
        const Status found = state.store.glyph(state.font, glyph, stored);
        note_peak(state);
        if (found.failed()) return found;
        if (stored.points.empty()) return Status::success();
        GlyphOutline& corners = state.measuring;
        corners.points.clear();
        corners.tags.clear();
        corners.contour_ends.clear();
        Point least = stored.points[0];
        Point most = stored.points[0];
        for (usize index = 1; index < stored.points.count(); ++index) {
            const Point point = stored.points[index];
            least = Point{.x = point.x < least.x ? point.x : least.x, .y = point.y < least.y ? point.y : least.y};
            most = Point{.x = point.x > most.x ? point.x : most.x, .y = point.y > most.y ? point.y : most.y};
        }
        if (corners.points.push_copy(least).failed() || corners.points.push_copy(most).failed()) {
            return refuse_font(ErrorKind::out_of_memory, operation);
        }
        const Status scaled = scale_outline(corners, scale, scale);
        if (scaled.failed()) return scaled;
        box = InkBox{.left = corners.points[0].x, .bottom = corners.points[0].y, .right = corners.points[1].x, .top = corners.points[1].y};
        return Status::success();
    }
    GlyphOutline& outline = state.measuring;
    const Status scaled = load_scaled_glyph(state.font, glyph, scale, scale, state.limits.font, outline);
    if (scaled.succeeded() && !outline.points.empty()) {
        box = InkBox{.left = outline.points[0].x, .bottom = outline.points[0].y, .right = outline.points[0].x, .top = outline.points[0].y};
        for (usize index = 1; index < outline.points.count(); ++index) {
            const Point point = outline.points[index];
            box.left = point.x < box.left ? point.x : box.left;
            box.right = point.x > box.right ? point.x : box.right;
            box.bottom = point.y < box.bottom ? point.y : box.bottom;
            box.top = point.y > box.top ? point.y : box.top;
        }
    }
    note_peak(state);
    return scaled;
}

usize Typeface::glyphs_read() const noexcept {
    if (state_.empty()) return 0;
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    const usize result = state.store.glyphs_read();
    return result;
}

usize Typeface::bytes_held() const noexcept {
    if (state_.empty()) return 0;
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    const usize result = bytes_of(state);
    return result;
}

usize Typeface::bytes_peak() const noexcept {
    if (state_.empty()) return 0;
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    const usize result = state.peak;
    return result;
}

void Typeface::reset_peak() noexcept {
    if (state_.empty()) return;
    TypefaceState& state = *state_;
    const ScopedLock held(state.lock);
    state.peak = bytes_of(state);
}

Result<Shared<Typeface>> open_typeface(const View<u8> bytes, const u32 face_index, const TypefaceLimits& limits) noexcept {
    Result<Shared<Typeface>> made = Shared<Typeface>::make();
    if (made.failed()) return made;
    const Status opened = (*(*made.value())).open(bytes, face_index, limits);
    if (opened.failed()) return Result<Shared<Typeface>>::failure(*opened.error());
    return made;
}

// ---- a thread's face ----------------------------------------------------------------------------

Face::Face() noexcept = default;
Face::Face(Face&&) noexcept = default;
Face& Face::operator=(Face&&) noexcept = default;
Face::~Face() noexcept = default;

Status Face::open(const Shared<Typeface>& typeface, const FaceLimits& limits) noexcept {
    const Operation operation("stxlib.glyphs.face");
    release();
    if (typeface.empty() || (*typeface).font() == nullptr) return refuse_font(ErrorKind::invalid_argument, operation);
    Result<Unique<detail::FaceState>> made = Unique<detail::FaceState>::make();
    if (made.failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    state_ = static_cast<Result<Unique<detail::FaceState>>&&>(made).take_value();
    FaceState& state = *state_;
    state.typeface = typeface;
    state.font = *(*typeface).font();
    state.limits = limits;
    note_peak(state);
    return Status::success();
}

Status Face::open(const View<u8> bytes, const u32 face_index, const FaceLimits& limits, const TypefaceLimits& typeface_limits) noexcept {
    release();
    Result<Shared<Typeface>> made = open_typeface(bytes, face_index, typeface_limits);
    if (made.failed()) return Status::failure(*made.error());
    const Status status = open(*made.value(), limits);
    return status;
}

const Font* Face::font() const noexcept {
    const Font* const result = state_.empty() ? nullptr : &(*state_).font;
    return result;
}

Shared<Typeface> Face::typeface() const noexcept {
    Shared<Typeface> result{};
    if (!state_.empty()) result = (*state_).typeface;
    return result;
}

Status Face::warm(const View<u32> characters, const i64 size, const Fit fit, usize& warmed) noexcept {
    const Operation operation("stxlib.glyphs.face");
    warmed = 0;
    if (state_.empty() || size < 0 || size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    FaceState& state = *state_;
    usize kept = 0;
    const Status keeping = (*state.typeface).keep(characters, kept);
    if (keeping.failed()) return keeping;
    for (usize index = 0; index < characters.count(); ++index) {
        const u32 glyph = glyph_of(state.font, characters[index]);
        if (glyph == 0 || glyph >= state.font.glyph_count) continue;
        if (size == 0) {
            warmed = warmed + 1;
            continue;
        }
        FittedGlyph fitted{};
        const Status had = fitted_of(state, glyph, size, fit, fitted);
        if (out_of_memory(had)) return had;
        if (had.succeeded()) warmed = warmed + 1;
    }
    note_peak(state);
    return Status::success();
}

Status Face::fitted(const u32 glyph, const i64 size, const Fit fit, FittedGlyph& glyph_out) noexcept {
    const Operation operation("stxlib.glyphs.face");
    glyph_out = FittedGlyph{};
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    const Status status = fitted_of(*state_, glyph, size, fit, glyph_out);
    note_peak(*state_);
    STXLIB_GLYPHS_CHECK(fitted_whole(*state_), "Face::fitted: the fitted outlines are not whole");
    return status;
}

bool Face::whole() const noexcept {
    if (state_.empty()) return true;
    const bool result = fitted_whole(*state_) && lattices_whole(*state_) && (*(*state_).typeface).whole();
    return result;
}

Status Face::mask(const u32 glyph, const DrawStyle& style, const Point shift, Bounds& bounds, Vector<u8>& pixels) noexcept {
    const Operation operation("stxlib.glyphs.face");
    bounds = Bounds{};
    // A shift farther than 2^30 either way puts no pixel in any mask; it is
    // refused before it is turned (and negated).
    const i32 far = static_cast<i32>(detail::farthest_point);
    const bool near = shift.x >= -far && shift.x <= far && shift.y >= -far && shift.y <= far;
    if (state_.empty() || !style_is_valid(style) || !near) {
        pixels.clear();
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    FaceState& state = *state_;
    Outline outline{};
    const Drawing drawing = drawing_of(state, style);
    const u32 quarter = drawing.quarter;
    if (drawing.route == Route::surface) {
        // The upright mask's surface, turned. A glyph of no ink has an
        // empty mask; one too large for a lattice goes by its outline,
        // below. The turn writes every byte, so `pixels` is brought to
        // its length without being emptied first.
        DrawStyle upright_style = style;
        upright_style.transformed = false;
        TurnLattice lattice{};
        const Status had_lattice = lattice_of(state, glyph, upright_style, lattice);
        if (had_lattice.failed() || lattice.upright.width == 0 || lattice.upright.height == 0) {
            pixels.clear();
            return had_lattice;
        }
        if (lattice.controls != nullptr) {
            if (!detail::turned_bounds(lattice.upright, style.xx, style.yx, shift, state.limits.mask, bounds)) {
                pixels.clear();
                return refuse_font(ErrorKind::limit_exceeded, operation);
            }
            if (pixels.resize(static_cast<usize>(bounds.width) * bounds.height).failed()) {
                bounds = Bounds{};
                pixels.clear();
                return refuse_font(ErrorKind::out_of_memory, operation);
            }
            detail::turn_lattice(lattice, style.xx, style.yx, shift, bounds, pixels.mutable_view().data());
            note_peak(state);
            STXLIB_GLYPHS_CHECK(fitted_whole(state) && lattices_whole(state), "Face::mask: what the face holds is not whole");
            return Status::success();
        }
    }
    pixels.clear();
    if (drawing.route == Route::quarters) {
        // The upright mask, turned pixel for pixel.
        DrawStyle upright_style = style;
        upright_style.transformed = false;
        const Point upright_shift = shift_before_quarter(quarter, shift);
        Bounds upright{};
        const Status had_upright = styled_outline(state, glyph, upright_style, outline);
        if (had_upright.failed()) return had_upright;
        const Status measured_upright = measure(outline, upright_shift, state.limits.mask, upright);
        if (measured_upright.failed()) return measured_upright;
        bounds = bounds_after_quarter(quarter, upright);
        const usize upright_bytes = static_cast<usize>(upright.width) * upright.height;
        if (upright_bytes == 0) return Status::success();
        if (state.glyph_pixels.resize(upright_bytes).failed() || pixels.resize(upright_bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        const Status drawn_upright =
            style.fit == Fit::monochrome ? state.rasterizer.render_monochrome(outline, upright_shift, upright, state.limits.mask, state.glyph_pixels.mutable_view())
                                         : state.rasterizer.render(outline, upright_shift, upright, state.limits.mask, state.glyph_pixels.mutable_view());
        if (drawn_upright.succeeded()) turn_pixels(quarter, upright, state.glyph_pixels.view().data(), pixels.mutable_view().data());
        note_peak(state);
        return drawn_upright;
    }
    const Status had = styled_outline(state, glyph, style, outline);
    if (had.failed()) return had;
    const Status measured = measure(outline, shift, state.limits.mask, bounds);
    if (measured.failed()) return measured;
    const usize bytes = static_cast<usize>(bounds.width) * bounds.height;
    if (bytes == 0) return Status::success();
    if (pixels.resize(bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    const Status drawn = style.fit == Fit::monochrome
                             ? state.rasterizer.render_monochrome(outline, shift, bounds, state.limits.mask, pixels.mutable_view())
                             : state.rasterizer.render(outline, shift, bounds, state.limits.mask, pixels.mutable_view());
    note_peak(state);
    STXLIB_GLYPHS_CHECK(fitted_whole(state), "Face::mask: the fitted outlines are not whole");
    return drawn;
}

Status Face::measure_run(const View<RunGlyph> glyphs, const DrawStyle& style, RunBox& box) noexcept {
    const Operation operation("stxlib.glyphs.face");
    box = RunBox{};
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    const Status status = place_run(*state_, glyphs, style, box);
    note_peak(*state_);
    STXLIB_GLYPHS_CHECK(fitted_whole(*state_), "Face::measure_run: the fitted outlines are not whole");
    return status;
}

Status Face::draw_run(const View<RunGlyph> glyphs, const DrawStyle& style, RunCoverage& coverage) noexcept {
    const Operation operation("stxlib.glyphs.face");
    coverage.pixels.clear();
    coverage.width = 0;
    coverage.height = 0;
    coverage.left = 0;
    coverage.top = 0;
    coverage.missing_glyphs = 0;
    coverage.outside_glyphs = 0;
    coverage.oversized = false;
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    FaceState& state = *state_;
    RunBox box{};
    const Status placed = place_run(state, glyphs, style, box);
    if (placed.failed()) return placed;
    coverage.missing_glyphs = box.missing_glyphs;
    coverage.outside_glyphs = box.outside_glyphs;
    coverage.oversized = box.oversized;
    if (box.oversized || box.right <= box.left || box.bottom <= box.top) return Status::success();
    const u32 width = static_cast<u32>(box.right - box.left);
    const u32 height = static_cast<u32>(box.bottom - box.top);
    const usize bytes = static_cast<usize>(width) * height;
    if (coverage.pixels.resize(bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    u8* const run_pixels = coverage.pixels.mutable_view().data();
    for (usize index = 0; index < bytes; ++index) run_pixels[index] = 0;
    coverage.width = width;
    coverage.height = height;
    coverage.left = box.left;
    coverage.top = box.top;
    const bool monochrome = style.fit == Fit::monochrome;
    const Drawing drawing = drawing_of(state, style);
    const u32 quarter = drawing.quarter;
    const bool by_quarters = drawing.route == Route::quarters;
    DrawStyle upright_style = style;
    upright_style.transformed = false;
    for (usize index = 0; index < glyphs.count(); ++index) {
        const PlacedGlyph glyph = state.placed[index];
        if (!glyph.shown) continue;
        const usize glyph_bytes = static_cast<usize>(glyph.bounds.width) * glyph.bounds.height;
        // By quarter turns the upright mask is drawn, in the box and with
        // the part of a pixel it has before the turn, and turned.
        const Bounds drawn_bounds = by_quarters ? bounds_before_quarter(quarter, glyph.bounds) : glyph.bounds;
        const Point drawn_shift = by_quarters ? shift_before_quarter(quarter, glyph.shift) : glyph.shift;
        if (glyph.by_surface) {
            // The lattice first: making one draws the upright mask in
            // glyph_pixels.
            TurnLattice lattice{};
            const Status had_lattice = lattice_of(state, glyphs[index].glyph, upright_style, lattice);
            if (had_lattice.failed()) return had_lattice;
            if (lattice.controls == nullptr) return refuse_font(ErrorKind::invalid_argument, operation);
            if (state.turned_pixels.resize(glyph_bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
            detail::turn_lattice(lattice, style.xx, style.yx, glyph.shift, glyph.bounds, state.turned_pixels.mutable_view().data());
        } else {
            Outline outline{};
            const Status had = styled_outline(state, glyphs[index].glyph, by_quarters ? upright_style : style, outline);
            if (had.failed()) return had;
            if (state.glyph_pixels.resize(glyph_bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
            const Status drawn = monochrome ? state.rasterizer.render_monochrome(outline, drawn_shift, drawn_bounds, state.limits.mask,
                                                                                 state.glyph_pixels.mutable_view())
                                            : state.rasterizer.render(outline, drawn_shift, drawn_bounds, state.limits.mask,
                                                                      state.glyph_pixels.mutable_view());
            if (drawn.failed()) return drawn;
        }
        if (by_quarters) {
            if (state.turned_pixels.resize(glyph_bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
            turn_pixels(quarter, drawn_bounds, state.glyph_pixels.view().data(), state.turned_pixels.mutable_view().data());
        }
        // The glyph's mask goes into the run's, row by row.
        const usize column = static_cast<usize>(static_cast<i64>(glyph.pixel_x) + glyph.bounds.left - box.left);
        const usize first_row = static_cast<usize>(static_cast<i64>(glyph.pixel_y) - glyph.bounds.top - box.top);
        const u8* const glyph_pixels = (by_quarters || glyph.by_surface) ? state.turned_pixels.view().data() : state.glyph_pixels.view().data();
        for (usize row = 0; row < glyph.bounds.height; ++row) {
            u8* const target = run_pixels + (first_row + row) * width + column;
            const u8* const source = glyph_pixels + row * glyph.bounds.width;
            if (monochrome) {
                for (usize x = 0; x < glyph.bounds.width; ++x) target[x] = source[x] > target[x] ? source[x] : target[x];
            } else {
                for (usize x = 0; x < glyph.bounds.width; ++x) {
                    const u32 fresh = source[x];
                    const u32 before = target[x];
                    target[x] = static_cast<u8>(fresh + before * (255U - fresh) / 255U);
                }
            }
        }
    }
    note_peak(state);
    STXLIB_GLYPHS_CHECK(fitted_whole(state) && lattices_whole(state), "Face::draw_run: what the face holds is not whole");
    STXLIB_GLYPHS_CHECK(coverage.pixels.count() == static_cast<usize>(coverage.width) * coverage.height, "Face::draw_run: the mask is not its box");
    return Status::success();
}

usize Face::glyphs_fitted() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).fittings;
    return result;
}

usize Face::lattices_made() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).lattices_made;
    return result;
}

usize Face::bytes_held() const noexcept {
    const usize result = state_.empty() ? 0 : bytes_of(*state_);
    return result;
}

usize Face::bytes_peak() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).peak;
    return result;
}

void Face::reset_peak() noexcept {
    if (!state_.empty()) (*state_).peak = bytes_of(*state_);
}

void Face::release() noexcept { state_.reset(); }

} // namespace stxlib::glyphs
