// stxlib::glyphs: the glyph store (include/stxlib/glyph_store.hpp).

#include "stxlib/glyph_store.hpp"

#include "algorithms/glyphs/font_reader.hpp"

namespace stxlib::glyphs {

namespace {

// Where a glyph is: nowhere, in part 0, 1 or 2 (the number less one), or
// refused.
inline constexpr u32 where_nowhere = 0;
inline constexpr u32 where_kept = 1;
inline constexpr u32 where_refused = 4;

} // namespace

// The outline goes to the end of the part. False: out of memory, and the
// part is as it was.
bool GlyphStore::put(Part& part, const u32 where, const u32 glyph, const i32 advance, const View<Point> points, const View<PointTag> tags,
                     const View<u32> contour_ends) noexcept {
    const usize first_point = part.points.count();
    const usize first_contour = part.contour_ends.count();
    const usize first_glyph = part.glyphs.count();
    bool held = part.glyphs.push_copy(glyph).succeeded();
    for (usize index = 0; held && index < points.count(); ++index) {
        held = part.points.push_copy(points[index]).succeeded() && part.tags.push_copy(tags[index]).succeeded();
    }
    for (usize index = 0; held && index < contour_ends.count(); ++index) held = part.contour_ends.push_copy(contour_ends[index]).succeeded();
    if (!held) {
        if (part.points.count() > first_point) part.points.truncate(first_point);
        if (part.tags.count() > first_point) part.tags.truncate(first_point);
        if (part.contour_ends.count() > first_contour) part.contour_ends.truncate(first_contour);
        if (part.glyphs.count() > first_glyph) part.glyphs.truncate(first_glyph);
        return false;
    }
    const Entry entry{.where = where,
                      .first_point = static_cast<u32>(first_point),
                      .point_count = static_cast<u32>(points.count()),
                      .first_contour = static_cast<u32>(first_contour),
                      .contour_count = static_cast<u32>(contour_ends.count()),
                      .advance = advance};
    entries_[glyph] = entry;
    return true;
}

// Everything in the part is forgotten. A glyph that has since been moved
// to another part is not the part's any more and stays.
void GlyphStore::give_up(Part& part) noexcept {
    const u32 where = static_cast<u32>(&part - &parts_[0]) + 1;
    for (usize index = 0; index < part.glyphs.count(); ++index) {
        const u32 glyph = part.glyphs[index];
        if (entries_[glyph].where == where) entries_[glyph] = Entry{};
    }
    part.points.clear();
    part.tags.clear();
    part.contour_ends.clear();
    part.glyphs.clear();
}

Status GlyphStore::open(const Font& font, const FontLimits& limits, const StoreLimits& store_limits) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    release();
    limits_ = limits;
    store_limits_ = store_limits;
    if (entries_.resize(font.glyph_count).failed()) return detail::refuse_font(ErrorKind::out_of_memory, operation);
    return Status::success();
}

// The glyph is found or read, and put where it belongs: in the kept part
// if `in_kept` and there is room, else among the recent ones.
Status GlyphStore::place(const Font& font, const u32 glyph, const bool in_kept, StoredGlyph& stored) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    stored = StoredGlyph{};
    if (entries_.count() != font.glyph_count || glyph >= font.glyph_count) return detail::refuse_font(ErrorKind::invalid_argument, operation);
    Entry entry = entries_[glyph];
    if (entry.where == where_refused) return detail::refuse_font(static_cast<ErrorKind>(entry.first_point), operation);
    const u32 newer = newer_ + 1;
    const bool settled = entry.where == where_kept || (entry.where == newer && !in_kept);
    if (!settled) {
        i32 advance = entry.advance;
        // Its outline: from the part it is in, or from the font.
        if (entry.where != where_nowhere) {
            const Part& from = parts_[entry.where - 1];
            bool copied = loaded_.points.resize(entry.point_count).succeeded() && loaded_.tags.resize(entry.point_count).succeeded() &&
                          loaded_.contour_ends.resize(entry.contour_count).succeeded();
            if (!copied) return detail::refuse_font(ErrorKind::out_of_memory, operation);
            for (usize index = 0; index < entry.point_count; ++index) {
                loaded_.points[index] = from.points[entry.first_point + index];
                loaded_.tags[index] = from.tags[entry.first_point + index];
            }
            for (usize index = 0; index < entry.contour_count; ++index) loaded_.contour_ends[index] = from.contour_ends[entry.first_contour + index];
        } else {
            const Status loaded = load_glyph(font, glyph, limits_, loaded_);
            read_ = read_ + 1;
            if (loaded.failed()) {
                const ErrorKind kind = (*loaded.error()).kind();
                // Out of memory may pass; anything else is the glyph's own.
                if (kind != ErrorKind::out_of_memory) entries_[glyph] = Entry{.where = where_refused, .first_point = static_cast<u32>(kind)};
                return loaded;
            }
            if (loaded_.tags.count() != loaded_.points.count()) return detail::refuse_font(ErrorKind::invalid_argument, operation);
            advance = advance_of(font, glyph);
        }
        const usize point_count = loaded_.points.count();
        const usize half = store_limits_.recent_points / 2;
        Part& kept_part = parts_[0];
        if (in_kept && kept_part.points.count() + point_count <= store_limits_.kept_points) {
            if (!put(kept_part, where_kept, glyph, advance, loaded_.points.view(), loaded_.tags.view(), loaded_.contour_ends.view())) {
                return detail::refuse_font(ErrorKind::out_of_memory, operation);
            }
        } else if (entry.where == newer) {
            // Asked to be kept, with no room there: it stays where it is.
        } else if (point_count <= half) {
            if (parts_[newer_].points.count() + point_count > half) {
                // The newer half is full: the older is given up and a new
                // one begun.
                const u32 older = newer_ == 1 ? 2U : 1U;
                give_up(parts_[older]);
                newer_ = older;
            }
            if (!put(parts_[newer_], newer_ + 1, glyph, advance, loaded_.points.view(), loaded_.tags.view(), loaded_.contour_ends.view())) {
                return detail::refuse_font(ErrorKind::out_of_memory, operation);
            }
        } else {
            // Too large to keep: served from where it was read to, and
            // read again next time.
            entries_[glyph] = Entry{};
            stored.points = loaded_.points.view();
            stored.tags = loaded_.tags.view();
            stored.contour_ends = loaded_.contour_ends.view();
            stored.advance = advance;
            return Status::success();
        }
        entry = entries_[glyph];
    }
    const Part& part = parts_[entry.where - 1];
    stored.points = View<Point>(part.points.view().data() + entry.first_point, entry.point_count);
    stored.tags = View<PointTag>(part.tags.view().data() + entry.first_point, entry.point_count);
    stored.contour_ends = View<u32>(part.contour_ends.view().data() + entry.first_contour, entry.contour_count);
    stored.advance = entry.advance;
    return Status::success();
}

Status GlyphStore::glyph(const Font& font, const u32 glyph, StoredGlyph& stored) noexcept {
    const Status status = place(font, glyph, false, stored);
    return status;
}

Status GlyphStore::keep(const Font& font, const View<u32> characters, usize& kept) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    kept = parts_[0].glyphs.count();
    if (entries_.count() != font.glyph_count) return detail::refuse_font(ErrorKind::invalid_argument, operation);
    for (usize index = 0; index < characters.count(); ++index) {
        const u32 glyph = glyph_of(font, characters[index]);
        if (glyph == 0 || glyph >= font.glyph_count) continue;
        StoredGlyph stored{};
        const Status status = place(font, glyph, true, stored);
        if (status.failed() && (*status.error()).kind() == ErrorKind::out_of_memory) return status;
    }
    kept = parts_[0].glyphs.count();
    return Status::success();
}

void GlyphStore::release() noexcept {
    entries_.release();
    for (usize index = 0; index < 3; ++index) {
        parts_[index].points.release();
        parts_[index].tags.release();
        parts_[index].contour_ends.release();
        parts_[index].glyphs.release();
    }
    newer_ = 1;
    loaded_.points.release();
    loaded_.tags.release();
    loaded_.contour_ends.release();
    read_ = 0;
}

} // namespace stxlib::glyphs
