// stxlib::glyphs: a face to draw text from (include/stxlib/glyph_face.hpp).

#include "stxlib/glyph_face.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "stxlib/glyph_hinter.hpp"

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

// A glyph of a run as placed: its pixel, the part of a pixel left over,
// and its mask's bounds.
struct PlacedGlyph final {
    i32 pixel_x{0};
    i32 pixel_y{0};
    Point shift{};
    Bounds bounds{};
    bool shown{false};
};

struct FaceState final {
    Font font{};
    FaceLimits limits{};
    GlyphStore store{};
    Hinter hinter{};
    Rasterizer rasterizer{};
    FittedHalf halves[2]{};
    u32 newer{0};
    bool prepared{false};
    usize fittings{0};
    // Working storage: the outline being fitted, its segments, a turned
    // outline's points, a glyph's mask, and a run's glyphs as placed.
    GlyphOutline fitting{};
    Vector<Segment> work_kinds{};
    Vector<Point> work_points{};
    Vector<Point> turned{};
    Vector<u8> glyph_pixels{};
    Vector<PlacedGlyph> placed{};
};

} // namespace detail

namespace {

using detail::FaceState;
using detail::FittedHalf;
using detail::FittedRecord;
using detail::PlacedGlyph;
using detail::refuse_font;

inline constexpr usize not_found = ~static_cast<usize>(0);
inline constexpr i64 largest_size = static_cast<i64>(1) << 24;

// a * b / 65536, rounded to the nearest, halves away from zero.
[[nodiscard]] constexpr i32 times(const i64 a, const i64 b) noexcept {
    const i64 product = a * b;
    const i64 size = product < 0 ? -product : product;
    const i64 rounded = (size + 0x8000) >> 16;
    const i32 result = static_cast<i32>(product < 0 ? -rounded : rounded);
    return result;
}

// The whole pixel a 26.6 coordinate is in.
[[nodiscard]] constexpr i32 pixel_of(const i32 value) noexcept {
    const i32 result = value >> 6;
    return result;
}

[[nodiscard]] constexpr usize slot_of(const u32 glyph, const u32 size, const u32 fit, const usize slots) noexcept {
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
[[nodiscard]] bool put(FaceState& state, FittedHalf& half, FittedRecord record, const usize half_records) noexcept {
    const usize slots = slots_for(half_records);
    if (half.table.count() != slots) {
        if (half.table.resize(slots).failed()) return false;
        for (usize index = 0; index < slots; ++index) half.table[index] = 0;
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
    usize slot = slot_of(record.glyph, record.size, record.fit, slots);
    for (usize probe = 0; probe < slots && half.table[slot] != 0; ++probe) slot = (slot + 1) & (slots - 1);
    half.table[slot] = static_cast<u32>(half.records.count());
    return true;
}

// The glyph's outline at the size, made afresh into work_kinds and
// work_points; `record` gets its advance and its box.
[[nodiscard]] Status fit_outline(FaceState& state, const u32 glyph, const i64 size, const Fit fit, FittedRecord& record) noexcept {
    const Operation operation("stxlib.glyphs.face");
    StoredGlyph stored{};
    const Status had = state.store.glyph(state.font, glyph, stored);
    if (had.failed()) return had;
    i32 advance = 0;
    if (fit == Fit::none) {
        const usize point_count = stored.points.count();
        const i64 scale = scale_of(state.font, size);
        GlyphOutline& outline = state.fitting;
        outline.contour_ends.clear();
        if (outline.points.resize(point_count).failed() || outline.tags.resize(point_count).failed()) {
            return refuse_font(ErrorKind::out_of_memory, operation);
        }
        for (usize index = 0; index < point_count; ++index) {
            outline.points[index] = Point{.x = times(stored.points[index].x, scale), .y = times(stored.points[index].y, scale)};
            outline.tags[index] = stored.tags[index];
        }
        for (usize index = 0; index < stored.contour_ends.count(); ++index) {
            if (outline.contour_ends.push_copy(stored.contour_ends[index]).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        }
        advance = times(stored.advance, scale);
    } else {
        // The hinter is prepared when the first glyph is fitted.
        if (!state.prepared) {
            const Status prepared = state.hinter.prepare(state.font);
            if (prepared.failed()) return prepared;
            state.prepared = true;
        }
        const HintMode mode = fit == Fit::monochrome ? HintMode::monochrome : HintMode::light;
        const Status hinted = state.hinter.hint_stored(state.font, glyph, stored, size, mode, state.limits.font, state.fitting, advance);
        if (hinted.failed()) return hinted;
    }
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
    const usize half_points = state.limits.fitted_points / 2;
    const usize half_records = state.limits.fitted_glyphs / 2;
    if (state.work_points.count() > half_points || half_records == 0) {
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
    if (newer.points.count() + state.work_points.count() > half_points || newer.records.count() >= half_records) {
        // The newer half is full: the older is given up and a new one
        // begun.
        empty(older);
        state.newer = older_index;
    }
    FittedHalf& target = state.halves[state.newer];
    if (!put(state, target, record, half_records)) return refuse_font(ErrorKind::out_of_memory, operation);
    give(target, target.records[target.records.count() - 1], glyph_out);
    return Status::success();
}

// A run's glyph as it is drawn: its fitted outline, taken through the
// style's matrix if it has one (into `turned`).
[[nodiscard]] Status run_outline(FaceState& state, const RunGlyph& glyph, const RunStyle& style, Outline& outline) noexcept {
    const Operation operation("stxlib.glyphs.face");
    FittedGlyph fitted{};
    const Status had = fitted_of(state, glyph.glyph, style.size, style.fit, fitted);
    if (had.failed()) return had;
    outline = fitted.outline;
    if (!style.transformed) return Status::success();
    const usize count = fitted.outline.points.count();
    if (state.turned.resize(count).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    for (usize index = 0; index < count; ++index) {
        const Point point = fitted.outline.points[index];
        state.turned[index] = Point{.x = times(point.x, style.xx) + times(point.y, style.xy), .y = times(point.x, style.yx) + times(point.y, style.yy)};
    }
    outline.points = state.turned.view();
    return Status::success();
}

[[nodiscard]] bool out_of_memory(const Status& status) noexcept {
    const bool result = status.failed() && (*status.error()).kind() == ErrorKind::out_of_memory;
    return result;
}

// Every glyph of the run is placed (state.placed) and the run's box taken.
[[nodiscard]] Status place_run(FaceState& state, const View<RunGlyph> glyphs, const RunStyle& style, RunBox& box) noexcept {
    const Operation operation("stxlib.glyphs.face");
    box = RunBox{};
    if (style.transformed && style.fit != Fit::none) return refuse_font(ErrorKind::invalid_argument, operation);
    if (style.size <= 0 || style.size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    if (state.placed.resize(glyphs.count()).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    bool any = false;
    i64 left = 0;
    i64 top = 0;
    i64 right = 0;
    i64 bottom = 0;
    for (usize index = 0; index < glyphs.count(); ++index) {
        const RunGlyph glyph = glyphs[index];
        PlacedGlyph placed{};
        placed.pixel_x = pixel_of(glyph.x);
        placed.pixel_y = pixel_of(glyph.y);
        // What is left of a pixel: rightward, and downward, which on the
        // outline's axes is toward less y.
        placed.shift = Point{.x = glyph.x - placed.pixel_x * 64, .y = -(glyph.y - placed.pixel_y * 64)};
        Outline outline{};
        const Status had = run_outline(state, glyph, style, outline);
        Status measured = had;
        if (had.succeeded()) measured = measure(outline, placed.shift, state.limits.mask, placed.bounds);
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

} // namespace

Face::Face() noexcept = default;
Face::Face(Face&&) noexcept = default;
Face& Face::operator=(Face&&) noexcept = default;
Face::~Face() noexcept = default;

Status Face::open(const View<u8> bytes, const u32 face_index, const FaceLimits& limits) noexcept {
    const Operation operation("stxlib.glyphs.face");
    release();
    Result<Unique<detail::FaceState>> made = Unique<detail::FaceState>::make();
    if (made.failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    Unique<detail::FaceState> fresh = static_cast<Result<Unique<detail::FaceState>>&&>(made).take_value();
    FaceState& state = *fresh;
    state.limits = limits;
    const Status opened = open_font(bytes, face_index, state.font);
    if (opened.failed()) return opened;
    const Status stored = state.store.open(state.font, limits.font, limits.store);
    if (stored.failed()) return stored;
    state_ = static_cast<Unique<detail::FaceState>&&>(fresh);
    return Status::success();
}

const Font* Face::font() const noexcept {
    const Font* const result = state_.empty() ? nullptr : &(*state_).font;
    return result;
}

Status Face::warm(const View<u32> characters, const i64 size, const Fit fit, usize& warmed) noexcept {
    const Operation operation("stxlib.glyphs.face");
    warmed = 0;
    if (state_.empty() || size < 0 || size > largest_size) return refuse_font(ErrorKind::invalid_argument, operation);
    FaceState& state = *state_;
    usize kept = 0;
    const Status keeping = state.store.keep(state.font, characters, kept);
    if (keeping.failed()) return keeping;
    for (usize index = 0; index < characters.count(); ++index) {
        const u32 glyph = glyph_of(state.font, characters[index]);
        if (glyph == 0 || glyph >= state.font.glyph_count) continue;
        if (size == 0) {
            StoredGlyph stored{};
            if (state.store.glyph(state.font, glyph, stored).succeeded()) warmed = warmed + 1;
            continue;
        }
        FittedGlyph fitted{};
        const Status had = fitted_of(state, glyph, size, fit, fitted);
        if (out_of_memory(had)) return had;
        if (had.succeeded()) warmed = warmed + 1;
    }
    return Status::success();
}

Status Face::fitted(const u32 glyph, const i64 size, const Fit fit, FittedGlyph& glyph_out) noexcept {
    const Operation operation("stxlib.glyphs.face");
    glyph_out = FittedGlyph{};
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    const Status status = fitted_of(*state_, glyph, size, fit, glyph_out);
    return status;
}

Status Face::mask(const u32 glyph, const i64 size, const Fit fit, const Point shift, Bounds& bounds, Vector<u8>& pixels) noexcept {
    const Operation operation("stxlib.glyphs.face");
    bounds = Bounds{};
    pixels.clear();
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    FaceState& state = *state_;
    FittedGlyph fitted{};
    const Status had = fitted_of(state, glyph, size, fit, fitted);
    if (had.failed()) return had;
    const Status measured = measure(fitted.outline, shift, state.limits.mask, bounds);
    if (measured.failed()) return measured;
    const usize bytes = static_cast<usize>(bounds.width) * bounds.height;
    if (bytes == 0) return Status::success();
    if (pixels.resize(bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    const Status drawn = fit == Fit::monochrome
                             ? state.rasterizer.render_monochrome(fitted.outline, shift, bounds, state.limits.mask, pixels.mutable_view())
                             : state.rasterizer.render(fitted.outline, shift, bounds, state.limits.mask, pixels.mutable_view());
    return drawn;
}

Status Face::measure_run(const View<RunGlyph> glyphs, const RunStyle& style, RunBox& box) noexcept {
    const Operation operation("stxlib.glyphs.face");
    box = RunBox{};
    if (state_.empty()) return refuse_font(ErrorKind::invalid_argument, operation);
    const Status status = place_run(*state_, glyphs, style, box);
    return status;
}

Status Face::draw_run(const View<RunGlyph> glyphs, const RunStyle& style, RunCoverage& coverage) noexcept {
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
    for (usize index = 0; index < glyphs.count(); ++index) {
        const PlacedGlyph glyph = state.placed[index];
        if (!glyph.shown) continue;
        Outline outline{};
        const Status had = run_outline(state, glyphs[index], style, outline);
        if (had.failed()) return had;
        const usize glyph_bytes = static_cast<usize>(glyph.bounds.width) * glyph.bounds.height;
        if (state.glyph_pixels.resize(glyph_bytes).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        const Status drawn = monochrome ? state.rasterizer.render_monochrome(outline, glyph.shift, glyph.bounds, state.limits.mask,
                                                                             state.glyph_pixels.mutable_view())
                                        : state.rasterizer.render(outline, glyph.shift, glyph.bounds, state.limits.mask,
                                                                  state.glyph_pixels.mutable_view());
        if (drawn.failed()) return drawn;
        // The glyph's mask goes into the run's, row by row.
        const usize column = static_cast<usize>(static_cast<i64>(glyph.pixel_x) + glyph.bounds.left - box.left);
        const usize first_row = static_cast<usize>(static_cast<i64>(glyph.pixel_y) - glyph.bounds.top - box.top);
        const u8* const glyph_pixels = state.glyph_pixels.view().data();
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
    return Status::success();
}

usize Face::glyphs_read() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).store.glyphs_read();
    return result;
}

usize Face::glyphs_fitted() const noexcept {
    const usize result = state_.empty() ? 0 : (*state_).fittings;
    return result;
}

usize Face::bytes_held() const noexcept {
    if (state_.empty()) return 0;
    const FaceState& state = *state_;
    const usize result = sizeof(FaceState) + state.store.bytes_held() + bytes_of(state.halves[0]) + bytes_of(state.halves[1]) +
                         state.fitting.points.capacity() * sizeof(Point) + state.fitting.tags.capacity() * sizeof(PointTag) +
                         state.fitting.contour_ends.capacity() * sizeof(u32) + state.work_kinds.capacity() * sizeof(Segment) +
                         state.work_points.capacity() * sizeof(Point) + state.turned.capacity() * sizeof(Point) +
                         state.glyph_pixels.capacity() + state.placed.capacity() * sizeof(PlacedGlyph);
    return result;
}

void Face::release() noexcept { state_.reset(); }

} // namespace stxlib::glyphs
