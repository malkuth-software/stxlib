// stxlib::glyphs: the hinter (include/stxlib/glyph_hinter.hpp). After
// Skrifa 0.50.0, outline/autohint/hint/mod.rs and instance.rs: a glyph's
// outline measured, fitted and moved, one axis and then the other, and its
// advance. The care for accents along y is after FreeType 2.14.2
// (hinter_accents.cpp).

#include "stxlib/glyph_hinter.hpp"

#include "algorithms/glyphs/hinter.hpp"

namespace stxlib::glyphs {

namespace {

using detail::axis_horizontal;
using detail::axis_vertical;
using detail::HintOutline;
using detail::HintPoint;
using detail::HintScale;
using detail::HintScratch;
using detail::no_index;
using detail::pixel_round;
using detail::refuse_font;
using detail::ScaledStyleMetrics;
using detail::times_fixed;

// What hinting an outline leaves for its advance.
struct Fitted final {
    i32 x_scale{0};
    bool has_edges{false};
    i32 left_original{0};
    i32 left{0};
    i32 right_original{0};
    i32 right{0};
    bool no_memory{false};
};

[[nodiscard]] Fitted hint_outline(HintOutline& outline, const StyleMetrics& metrics, const u32 glyph_style, const u32 adjustment,
                                  HintScratch& scratch, const ScaledStyleMetrics& scaled) noexcept {
    Fitted fitted{};
    const HintScale& scale = scaled.scale;
    const detail::TableScript& script = detail::script_of_style(metrics.style);
    const bool cjk = script.group != detail::group_default;
    // Every point at the size, before and after hinting.
    HintPoint* const points = outline.points.mutable_view().data();
    const usize point_count = outline.points.count();
    for (usize index = 0; index < point_count; ++index) {
        const i32 x = times_fixed(points[index].fx, scale.x_scale) + scale.x_delta;
        const i32 y = times_fixed(points[index].fy, scale.y_scale) + scale.y_delta;
        points[index].ox = x;
        points[index].x = x;
        points[index].oy = y;
        points[index].y = y;
    }
    fitted.x_scale = scale.x_scale;
    // A default script with no blue zones is not hinted at all.
    if (!cjk && scaled.axes[1].blues.empty()) return fitted;
    detail::HintAxis& axis = scratch.axis;
    for (u32 dimension = 0; dimension < 2; ++dimension) {
        if (dimension == axis_horizontal && (scale.flags & detail::scale_no_horizontal) != 0) continue;
        if (dimension == axis_vertical && (scale.flags & detail::scale_no_vertical) != 0) continue;
        // Accents (hinter_accents.cpp) are seen to along y, for the scripts
        // hinted the default way.
        const bool accents = dimension == axis_vertical && !cjk && adjustment != 0;
        if (accents && !detail::stretch_tildes(outline, adjustment, scratch.reaches)) fitted.no_memory = true;
        detail::reset_axis(axis, dimension, outline.winding);
        static_cast<void>(detail::compute_segments(outline, axis, fitted.no_memory));
        const Vector<i32>& widths = metrics.axes[dimension].widths;
        const i32 widest = widths.empty() ? 0 : widths[widths.count() - 1];
        detail::link_segments(outline, axis, scaled.axes[dimension].scale, cjk, widest);
        if (!detail::compute_edges(axis, scaled.axes[dimension], script.top_to_bottom, scale.y_scale, cjk)) fitted.no_memory = true;
        i32 accent_height_limit = 0;
        if (accents) accent_height_limit = detail::keep_accents_off_blues(axis, scale, scaled.axes[dimension].blues.view(), adjustment);
        if (dimension == axis_vertical) {
            // Marks (accents) are not caught by blue zones.
            if (cjk || (glyph_style & detail::style_non_base) == 0) {
                detail::compute_blue_edges(axis, scale, metrics.axes[dimension].blues.view(), scaled.axes[dimension].blues.view(), cjk);
            }
        } else {
            fitted.x_scale = scaled.axes[0].scale;
        }
        detail::hint_edges(outline, axis, scaled.axes[dimension], cjk, scale, script.top_to_bottom);
        detail::align_edge_points(outline, axis, cjk, scale);
        detail::align_strong_points(outline, axis);
        detail::align_weak_points(outline, dimension);
        if (accents && !detail::separate_accents(outline, adjustment, accent_height_limit, scratch.reaches)) fitted.no_memory = true;
        if (dimension == axis_horizontal && axis.edges.count() > 1) {
            const detail::HintEdge& left = axis.edges[0];
            const detail::HintEdge& right = axis.edges[axis.edges.count() - 1];
            fitted.has_edges = true;
            fitted.left_original = left.original;
            fitted.left = left.position;
            fitted.right_original = right.original;
            fitted.right = right.position;
        }
    }
    return fitted;
}

// The measures of a style, taken from the font the first time a glyph of
// the style is hinted. `found` is their place in `all`.
[[nodiscard]] Status measures_of(const Font& font, const u32 style, const FontLimits& limits, HintScratch& scratch, Vector<StyleMetrics>& all,
                                 usize& found) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    found = no_index;
    for (usize index = 0; index < all.count(); ++index) {
        if (all[index].style == style) {
            found = index;
            break;
        }
    }
    if (found != no_index) return Status::success();
    StyleMetrics measured{};
    const Status status = detail::compute_style_metrics(font, style, limits, scratch, measured);
    if (status.failed()) return status;
    if (all.push(static_cast<StyleMetrics&&>(measured)).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    found = all.count() - 1;
    return Status::success();
}

// A glyph's outline in font units, hinted.
[[nodiscard]] Status hint_loaded(const Font& font, const StyleMetrics& metrics, const u32 glyph_style, const u32 adjustment,
                                 const View<Point> glyph_points, const View<PointTag> glyph_tags, const View<u32> contour_ends,
                                 const i32 unscaled_advance, const i64 size, const HintMode mode, detail::HintWork& work, GlyphOutline& outline,
                                 i32& advance) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    HintScratch& scratch = work.scratch;
    const u32 style = glyph_style & detail::style_mask;
    const i32 units = static_cast<i32>(font.units_per_em);
    const Status filled = detail::fill_hint_outline(glyph_points, glyph_tags, contour_ends, units, unscaled_advance, scratch.outline);
    if (filled.failed()) return filled;
    // The style's measures at this size and mode: those of the last glyph
    // if it was of the same style, size and mode.
    if (!work.scaled_held || work.scaled_style != style || work.scaled_size != size || work.scaled_mode != mode) {
        const u32 group = detail::script_of_style(style).group;
        const HintScale wanted = detail::make_scale(font, size, mode, group);
        work.scaled_held = false;
        if (detail::scale_style_metrics(metrics, wanted, work.scaled).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        work.scaled_held = true;
        work.scaled_style = style;
        work.scaled_size = size;
        work.scaled_mode = mode;
    }
    const HintScale& scale = work.scaled.scale;
    const Fitted fitted = hint_outline(scratch.outline, metrics, glyph_style, adjustment, scratch, work.scaled);
    if (fitted.no_memory) return refuse_font(ErrorKind::out_of_memory, operation);
    // The advance: the scaled advance to a whole pixel, and in monochrome
    // mode made to follow where the outermost edges went.
    i32 left_point = 0;
    i32 right_point = times_fixed(unscaled_advance, fitted.x_scale);
    const bool light = mode == HintMode::light;
    if (!light && (scale.flags & detail::scale_no_advance) == 0 && fitted.has_edges) {
        const i32 old_right_bearing = right_point - fitted.right_original;
        const i32 old_left_bearing = fitted.left_original;
        const i32 new_left_bearing = fitted.left;
        i32 left_unrounded = new_left_bearing - old_left_bearing;
        i32 right_unrounded = fitted.right + old_right_bearing;
        // A glyph that began close to its origin, or ended close to its
        // advance, is given a little room.
        if (old_left_bearing < 24) left_unrounded = left_unrounded - 8;
        if (old_right_bearing < 24) right_unrounded = right_unrounded + 8;
        left_point = pixel_round(left_unrounded);
        right_point = pixel_round(right_unrounded);
        if (left_point >= new_left_bearing && old_left_bearing > 0) left_point = left_point - 64;
        if (right_point <= fitted.right && old_right_bearing > 0) right_point = right_point + 64;
    } else {
        left_point = pixel_round(left_point);
        right_point = pixel_round(right_point);
    }
    const usize point_count = scratch.outline.points.count();
    if (outline.points.resize(point_count).failed() || outline.tags.resize(point_count).failed()) {
        return refuse_font(ErrorKind::out_of_memory, operation);
    }
    for (usize index = 0; index < point_count; ++index) {
        const HintPoint& point = scratch.outline.points[index];
        outline.points[index] = Point{.x = point.x - left_point, .y = point.y};
        outline.tags[index] = point.tag;
    }
    for (usize index = 0; index < contour_ends.count(); ++index) {
        if (outline.contour_ends.push_copy(contour_ends[index]).failed()) return refuse_font(ErrorKind::out_of_memory, operation);
    }
    i32 fitted_advance = 0;
    if (!light && (font.fixed_pitch || (metrics.digits_same_width && (glyph_style & detail::style_digit) != 0))) {
        fitted_advance = times_fixed(unscaled_advance, detail::make_scale(font, size, mode, detail::script_of_style(style).group).x_scale);
    } else if (unscaled_advance != 0) {
        fitted_advance = right_point - left_point;
    }
    advance = pixel_round(fitted_advance);
    return Status::success();
}

} // namespace

Hinter::Hinter() noexcept = default;
Hinter::Hinter(Hinter&&) noexcept = default;
Hinter& Hinter::operator=(Hinter&&) noexcept = default;
Hinter::~Hinter() noexcept = default;

Status Hinter::prepare(const Font& font) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    metrics_.clear();
    if (work_.empty()) {
        Result<Unique<detail::HintWork>> made = Unique<detail::HintWork>::make();
        if (made.failed()) return refuse_font(ErrorKind::out_of_memory, operation);
        work_ = static_cast<Result<Unique<detail::HintWork>>&&>(made).take_value();
    }
    (*work_).scaled_held = false;
    Status status = detail::compute_glyph_styles(font, styles_);
    if (status.succeeded()) status = detail::compute_glyph_adjustments(font, adjustments_);
    if (status.failed()) {
        styles_.clear();
        adjustments_.clear();
    }
    return status;
}

Status Hinter::hint_glyph(const Font& font, const u32 glyph, const i64 size, const HintMode mode, const FontLimits& limits,
                          GlyphOutline& outline, i32& advance) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    advance = 0;
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count || work_.empty() || styles_.count() != font.glyph_count || adjustments_.count() != font.glyph_count ||
        size <= 0 || size > (static_cast<i64>(1) << 24)) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    detail::HintWork& work = *work_;
    const u32 glyph_style = styles_[glyph];
    usize found = no_index;
    const Status measured = measures_of(font, glyph_style & detail::style_mask, limits, work.scratch, metrics_, found);
    if (measured.failed()) return measured;
    // The glyph is read from the font here, each time: a caller with many
    // glyphs to hint keeps them in a GlyphStore and calls hint_stored.
    GlyphOutline& loaded = work.scratch.glyph;
    const Status read = load_glyph(font, glyph, limits, loaded);
    if (read.failed()) return read;
    const Status status = hint_loaded(font, metrics_[found], glyph_style, adjustments_[glyph], loaded.points.view(), loaded.tags.view(),
                                      loaded.contour_ends.view(), advance_of(font, glyph), size, mode, work, outline, advance);
    return status;
}

Status Hinter::hint_stored(const Font& font, const u32 glyph, const StoredGlyph& stored, const i64 size, const HintMode mode,
                           const FontLimits& limits, GlyphOutline& outline, i32& advance) noexcept {
    const Operation operation("stxlib.glyphs.hint_glyph");
    advance = 0;
    outline.points.clear();
    outline.tags.clear();
    outline.contour_ends.clear();
    if (glyph >= font.glyph_count || work_.empty() || styles_.count() != font.glyph_count || adjustments_.count() != font.glyph_count ||
        size <= 0 || size > (static_cast<i64>(1) << 24) || stored.points.count() > limits.points) {
        return refuse_font(ErrorKind::invalid_argument, operation);
    }
    detail::HintWork& work = *work_;
    const u32 glyph_style = styles_[glyph];
    usize found = no_index;
    const Status measured = measures_of(font, glyph_style & detail::style_mask, limits, work.scratch, metrics_, found);
    if (measured.failed()) return measured;
    const Status status = hint_loaded(font, metrics_[found], glyph_style, adjustments_[glyph], stored.points, stored.tags, stored.contour_ends,
                                      stored.advance, size, mode, work, outline, advance);
    return status;
}

usize Hinter::bytes_held() const noexcept {
    usize bytes = styles_.capacity() * sizeof(u16) + adjustments_.capacity() * sizeof(u16) + metrics_.capacity() * sizeof(StyleMetrics);
    for (usize index = 0; index < metrics_.count(); ++index) {
        for (u32 dimension = 0; dimension < 2; ++dimension) {
            bytes = bytes + metrics_[index].axes[dimension].widths.capacity() * sizeof(i32) +
                    metrics_[index].axes[dimension].blues.capacity() * sizeof(UnscaledBlue);
        }
    }
    if (!work_.empty()) {
        const detail::HintWork& work = *work_;
        const HintScratch& scratch = work.scratch;
        bytes = bytes + sizeof(detail::HintWork) + scratch.glyph.points.capacity() * sizeof(Point) + scratch.glyph.tags.capacity() * sizeof(PointTag) +
                scratch.glyph.contour_ends.capacity() * sizeof(u32) + scratch.outline.points.capacity() * sizeof(HintPoint) +
                scratch.outline.contours.capacity() * sizeof(detail::HintContour) + scratch.axis.segments.capacity() * sizeof(detail::HintSegment) +
                scratch.axis.edges.capacity() * sizeof(detail::HintEdge) + scratch.reaches.capacity() * sizeof(detail::ContourReach);
        for (u32 dimension = 0; dimension < 2; ++dimension) {
            bytes = bytes + work.scaled.axes[dimension].widths.capacity() * sizeof(detail::ScaledWidth) +
                    work.scaled.axes[dimension].blues.capacity() * sizeof(detail::ScaledBlue);
        }
    }
    return bytes;
}

void Hinter::release() noexcept {
    styles_.release();
    adjustments_.release();
    metrics_.release();
    work_.reset();
}

} // namespace stxlib::glyphs
