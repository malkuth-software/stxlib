// stxlib::glyphs: the glyph store (include/stxlib/glyph_store.hpp).

#include "stxlib/glyph_store.hpp"

#include "algorithms/glyphs/font_reader.hpp"
#include "algorithms/glyphs/glyph_checks.hpp"

namespace stxlib::glyphs {

namespace {

// A glyph's place in the index: nowhere, or a part and a number.
inline constexpr u32 nowhere = 0;
inline constexpr u32 part_kept = 0;
inline constexpr u32 part_refused = 3;

// What a glyph takes of a part's storage.
[[nodiscard]] constexpr usize cost_of(const usize points, const usize contours) noexcept {
    const usize result = points * 9 + contours * 4 + 24;
    return result;
}

[[nodiscard]] constexpr u32 code_of(const u32 part, const u32 number) noexcept {
    const u32 result = ((number + 1) << 2) | part;
    return result;
}

} // namespace

// The outline goes to the end of the part. False: out of memory, and the
// part is as it was.
bool GlyphStore::put(const u32 part_number, const u32 glyph, const i32 advance, const View<Point> points, const View<PointTag> tags,
                     const View<u32> contour_ends) noexcept {
    Part& part = parts_[part_number];
    const usize first_point = part.points.count();
    const usize first_contour = part.contour_ends.count();
    const Record record{.glyph = glyph,
                        .first_point = static_cast<u32>(first_point),
                        .point_count = static_cast<u32>(points.count()),
                        .first_contour = static_cast<u32>(first_contour),
                        .contour_count = static_cast<u32>(contour_ends.count()),
                        .advance = advance};
    bool held = part.records.count() < 0x3FFFFFFEU;
    for (usize index = 0; held && index < points.count(); ++index) {
        held = part.points.push_copy(points[index]).succeeded() && part.tags.push_copy(tags[index]).succeeded();
    }
    for (usize index = 0; held && index < contour_ends.count(); ++index) held = part.contour_ends.push_copy(contour_ends[index]).succeeded();
    held = held && part.records.push_copy(record).succeeded();
    if (held) held_[part_number] = held_[part_number] + cost_of(points.count(), contour_ends.count());
    if (!held) {
        if (part.points.count() > first_point) part.points.truncate(first_point);
        if (part.tags.count() > first_point) part.tags.truncate(first_point);
        if (part.contour_ends.count() > first_contour) part.contour_ends.truncate(first_contour);
        return false;
    }
    index_[glyph] = code_of(part_number, static_cast<u32>(part.records.count() - 1));
    return true;
}

// Everything in the part is forgotten. A glyph that has since been moved
// to another part is not the part's any more and stays.
void GlyphStore::give_up(const u32 part_number) noexcept {
    Part& part = parts_[part_number];
    for (usize number = 0; number < part.records.count(); ++number) {
        const u32 glyph = part.records[number].glyph;
        if (index_[glyph] == code_of(part_number, static_cast<u32>(number))) index_[glyph] = nowhere;
    }
    part.points.clear();
    part.tags.clear();
    part.contour_ends.clear();
    part.records.clear();
    held_[part_number] = 0;
}

Status GlyphStore::open(const Font& font, const FontLimits& limits, const StoreLimits& store_limits) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    release();
    limits_ = limits;
    store_limits_ = store_limits;
    if (index_.resize(font.glyph_count).failed()) return detail::refuse_font(ErrorKind::out_of_memory, operation);
    return Status::success();
}

// The glyph is found or read, and put where it belongs: in the kept part
// if `in_kept` and there is room, else among the recent ones.
Status GlyphStore::place(const Font& font, const u32 glyph, const bool in_kept, StoredGlyph& stored) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    stored = StoredGlyph{};
    if (index_.count() != font.glyph_count || glyph >= font.glyph_count) return detail::refuse_font(ErrorKind::invalid_argument, operation);
    const u32 code = index_[glyph];
    const u32 held_in = code & 3U;
    if (code != nowhere && held_in == part_refused) return detail::refuse_font(static_cast<ErrorKind>(code >> 2), operation);
    const bool settled = code != nowhere && (held_in == part_kept || (held_in == newer_ && !in_kept));
    if (!settled) {
        // Its outline: from the part it is in, or from the font.
        i32 advance = 0;
        if (code != nowhere) {
            const Part& from = parts_[held_in];
            const Record record = from.records[(code >> 2) - 1];
            const bool copied = loaded_.points.resize(record.point_count).succeeded() && loaded_.tags.resize(record.point_count).succeeded() &&
                                loaded_.contour_ends.resize(record.contour_count).succeeded();
            if (!copied) return detail::refuse_font(ErrorKind::out_of_memory, operation);
            for (usize index = 0; index < record.point_count; ++index) {
                loaded_.points[index] = from.points[record.first_point + index];
                loaded_.tags[index] = from.tags[record.first_point + index];
            }
            for (usize index = 0; index < record.contour_count; ++index) loaded_.contour_ends[index] = from.contour_ends[record.first_contour + index];
            advance = record.advance;
        } else {
            const Status loaded = load_glyph(font, glyph, limits_, loaded_);
            read_ = read_ + 1;
            if (loaded.failed()) {
                const ErrorKind kind = (*loaded.error()).kind();
                // Out of memory may pass; anything else is the glyph's own.
                if (kind != ErrorKind::out_of_memory) index_[glyph] = (static_cast<u32>(kind) << 2) | part_refused;
                return loaded;
            }
            if (loaded_.tags.count() != loaded_.points.count()) return detail::refuse_font(ErrorKind::invalid_argument, operation);
            advance = advance_of(font, glyph);
        }
        // Storage grows by doubling, so a part holds half of what its
        // storage may come to.
        const usize cost = cost_of(loaded_.points.count(), loaded_.contour_ends.count());
        const usize half = store_limits_.recent_bytes / 4;
        if (in_kept && held_[part_kept] + cost <= store_limits_.kept_bytes / 2) {
            if (!put(part_kept, glyph, advance, loaded_.points.view(), loaded_.tags.view(), loaded_.contour_ends.view())) {
                return detail::refuse_font(ErrorKind::out_of_memory, operation);
            }
        } else if (code != nowhere && held_in == newer_) {
            // Asked to be kept, with no room there: it stays where it is.
        } else if (cost <= half) {
            if (held_[newer_] + cost > half) {
                // The newer half is full: the older is given up and a new
                // one begun.
                const u32 older = newer_ == 1 ? 2U : 1U;
                give_up(older);
                newer_ = older;
            }
            if (!put(newer_, glyph, advance, loaded_.points.view(), loaded_.tags.view(), loaded_.contour_ends.view())) {
                return detail::refuse_font(ErrorKind::out_of_memory, operation);
            }
        } else {
            // Too large to keep: served from where it was read to, and
            // read again next time.
            index_[glyph] = nowhere;
            stored.points = loaded_.points.view();
            stored.tags = loaded_.tags.view();
            stored.contour_ends = loaded_.contour_ends.view();
            stored.advance = advance;
            return Status::success();
        }
    }
    const u32 now = index_[glyph];
    const Part& part = parts_[now & 3U];
    const Record record = part.records[(now >> 2) - 1];
    stored.points = View<Point>(part.points.view().data() + record.first_point, record.point_count);
    stored.tags = View<PointTag>(part.tags.view().data() + record.first_point, record.point_count);
    stored.contour_ends = View<u32>(part.contour_ends.view().data() + record.first_contour, record.contour_count);
    stored.advance = record.advance;
    return Status::success();
}

Status GlyphStore::glyph(const Font& font, const u32 glyph, StoredGlyph& stored) noexcept {
    const Status status = place(font, glyph, false, stored);
    STXLIB_GLYPHS_CHECK(whole(), "GlyphStore::glyph: the store is not whole");
    STXLIB_GLYPHS_CHECK(status.failed() || (stored.tags.count() == stored.points.count() && stored.contour_ends.count() <= stored.points.count()),
                        "GlyphStore::glyph: the glyph given is not whole");
    return status;
}

bool GlyphStore::whole() const noexcept {
    bool good = newer_ == 1 || newer_ == 2;
    for (u32 number = 0; good && number < 3; ++number) {
        const Part& part = parts_[number];
        usize points = 0;
        usize contours = 0;
        usize cost = 0;
        for (usize index = 0; good && index < part.records.count(); ++index) {
            const Record& record = part.records[index];
            good = record.first_point == points && record.first_contour == contours && record.glyph < index_.count();
            points = points + record.point_count;
            contours = contours + record.contour_count;
            cost = cost + cost_of(record.point_count, record.contour_count);
        }
        good = good && points == part.points.count() && part.tags.count() == points && contours == part.contour_ends.count() &&
               held_[number] == cost;
        const usize limit = number == part_kept ? store_limits_.kept_bytes / 2 : store_limits_.recent_bytes / 4;
        good = good && cost <= limit;
    }
    for (usize glyph = 0; good && glyph < index_.count(); ++glyph) {
        const u32 code = index_[glyph];
        const u32 held_in = code & 3U;
        if (code == nowhere || held_in == part_refused) continue;
        const usize number = (code >> 2) - 1;
        good = number < parts_[held_in].records.count() && parts_[held_in].records[number].glyph == glyph;
    }
    return good;
}

Status GlyphStore::keep(const Font& font, const View<u32> characters, usize& kept) noexcept {
    const Operation operation("stxlib.glyphs.glyph_store");
    kept = parts_[part_kept].records.count();
    if (index_.count() != font.glyph_count) return detail::refuse_font(ErrorKind::invalid_argument, operation);
    for (usize index = 0; index < characters.count(); ++index) {
        const u32 glyph = glyph_of(font, characters[index]);
        if (glyph == 0 || glyph >= font.glyph_count) continue;
        StoredGlyph stored{};
        const Status status = place(font, glyph, true, stored);
        if (status.failed() && (*status.error()).kind() == ErrorKind::out_of_memory) return status;
    }
    kept = parts_[part_kept].records.count();
    STXLIB_GLYPHS_CHECK(whole(), "GlyphStore::keep: the store is not whole");
    return Status::success();
}

usize GlyphStore::bytes_held() const noexcept {
    usize bytes = index_.capacity() * sizeof(u32) + loaded_.points.capacity() * sizeof(Point) + loaded_.tags.capacity() * sizeof(PointTag) +
                  loaded_.contour_ends.capacity() * sizeof(u32);
    for (usize index = 0; index < 3; ++index) {
        const Part& part = parts_[index];
        bytes = bytes + part.points.capacity() * sizeof(Point) + part.tags.capacity() * sizeof(PointTag) +
                part.contour_ends.capacity() * sizeof(u32) + part.records.capacity() * sizeof(Record);
    }
    return bytes;
}

void GlyphStore::release() noexcept {
    index_.release();
    for (usize index = 0; index < 3; ++index) {
        parts_[index].points.release();
        parts_[index].tags.release();
        parts_[index].contour_ends.release();
        parts_[index].records.release();
        held_[index] = 0;
    }
    newer_ = 1;
    loaded_.points.release();
    loaded_.tags.release();
    loaded_.contour_ends.release();
    read_ = 0;
}

} // namespace stxlib::glyphs
