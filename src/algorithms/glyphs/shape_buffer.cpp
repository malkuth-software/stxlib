// stxlib::glyphs: the shaper's buffer (shape_buffer.hpp).

#include "algorithms/glyphs/shape_buffer.hpp"

#include "algorithms/glyphs/glyph_checks.hpp"

namespace stxlib::glyphs::detail {

namespace {

inline constexpr usize most_length_factor = 256;
inline constexpr usize most_length_least = 65536;
inline constexpr usize most_length_default = 0x3FFFFFFF;
inline constexpr i32 most_operations_factor = 4096;
inline constexpr i32 most_operations_least = 65536;
inline constexpr i32 most_operations_default = 0x1FFFFFFF;

[[nodiscard]] constexpr bool is_monotone(const ClusterLevel level) noexcept {
    const bool result = level == ClusterLevel::monotone_graphemes || level == ClusterLevel::monotone_characters;
    return result;
}

[[nodiscard]] constexpr bool is_graphemes(const ClusterLevel level) noexcept {
    const bool result = level == ClusterLevel::monotone_graphemes || level == ClusterLevel::graphemes;
    return result;
}

[[nodiscard]] bool same_cluster(const ShapeGlyph& before, const ShapeGlyph& after) noexcept {
    const bool result = before.cluster == after.cluster;
    return result;
}

// `count` operations off what is left; false when that is gone.
[[nodiscard]] bool spend(ShapeBuffer& buffer, const usize count) noexcept {
    const i64 left = static_cast<i64>(buffer.max_ops) - static_cast<i64>(count < 0x7FFFFFFF ? count : 0x7FFFFFFF);
    buffer.max_ops = left < -0x7FFFFFFF ? -0x7FFFFFFF : static_cast<i32>(left);
    if (buffer.max_ops < 0) buffer.successful = false;
    const bool result = buffer.max_ops >= 0;
    return result;
}

// The least cluster of items[start, end), no more than `cluster`: of all
// of them when clusters may be in any order, else of the two ends.
[[nodiscard]] u32 least_cluster(const ShapeBuffer& buffer, const Vector<ShapeGlyph>& items, const usize start, const usize end,
                                const u32 cluster) noexcept {
    u32 least = cluster;
    if (start >= end || end > items.count()) return least;
    if (buffer.cluster_level == ClusterLevel::characters) {
        for (usize index = start; index < end; ++index) least = items[index].cluster < least ? items[index].cluster : least;
    }
    const u32 first = items[start].cluster;
    const u32 last = items[end - 1].cluster;
    least = first < least ? first : least;
    least = last < least ? last : least;
    return least;
}

// `mask` is set on the items of [start, end) that are not of `cluster`:
// all of them when clusters may be in any order or `cluster` is at neither
// end, else those on the far side of the run of `cluster`.
void flag_other_clusters(ShapeBuffer& buffer, Vector<ShapeGlyph>& items, const usize start, const usize end, const u32 cluster,
                         const u32 mask) noexcept {
    if (start >= end) return;
    static_cast<void>(spend(buffer, end - start));
    if (end > items.count()) {
        buffer.successful = false;
        return;
    }
    const u32 first = items[start].cluster;
    const u32 last = items[end - 1].cluster;
    if (buffer.cluster_level == ClusterLevel::characters || (cluster != first && cluster != last)) {
        for (usize index = start; index < end; ++index) {
            if (items[index].cluster != cluster) items[index].mask = items[index].mask | mask;
        }
        return;
    }
    if (cluster == first) {
        for (usize index = end; index > start; --index) {
            ShapeGlyph& item = items[index - 1];
            if (item.cluster == first) break;
            if (item.cluster != cluster) item.mask = item.mask | mask;
        }
    } else {
        for (usize index = start; index < end; ++index) {
            ShapeGlyph& item = items[index];
            if (item.cluster == last) break;
            if (item.cluster != cluster) item.mask = item.mask | mask;
        }
    }
}

// The flags in `mask` are set over [start, end): on every item, or
// (`interior`) on those not of the stretch's least cluster. From the
// output, the stretch begins at output place `start` and ends at item
// `end`.
void set_glyph_flags(ShapeBuffer& buffer, const u32 mask, const usize start, const usize wanted_end, const bool interior,
                     const bool from_output) noexcept {
    // A stretch that is given and is too long is not marked.
    if (wanted_end != to_the_end && wanted_end - start > 255) return;
    const usize end = wanted_end < buffer.len ? wanted_end : buffer.len;
    if (interior && !from_output && (end < start || end - start < 2)) return;
    if (!from_output || !buffer.have_output) {
        if (start > end) return;
        if (!interior) {
            for (usize index = start; index < end; ++index) buffer.info[index].mask = buffer.info[index].mask | mask;
        } else {
            const u32 cluster = least_cluster(buffer, buffer.info, start, end, 0xFFFFFFFFU);
            flag_other_clusters(buffer, buffer.info, start, end, cluster, mask);
        }
        return;
    }
    if (start > buffer.out_len || buffer.idx > end) return;
    if (!interior) {
        for (usize index = start; index < buffer.out_len; ++index) buffer.out[index].mask = buffer.out[index].mask | mask;
        for (usize index = buffer.idx; index < end; ++index) buffer.info[index].mask = buffer.info[index].mask | mask;
    } else {
        u32 cluster = least_cluster(buffer, buffer.info, buffer.idx, end, 0xFFFFFFFFU);
        cluster = least_cluster(buffer, buffer.out, start, buffer.out_len, cluster);
        flag_other_clusters(buffer, buffer.out, start, buffer.out_len, cluster, mask);
        flag_other_clusters(buffer, buffer.info, buffer.idx, end, cluster, mask);
    }
}

void merge_clusters_now(ShapeBuffer& buffer, const usize wanted_start, const usize wanted_end) noexcept {
    usize start = wanted_start;
    usize end = wanted_end;
    if (!spend(buffer, end - start)) return;
    if (end > buffer.len || start >= end) {
        buffer.successful = false;
        return;
    }
    Vector<ShapeGlyph>& info = buffer.info;
    u32 cluster = info[start].cluster;
    for (usize index = start + 1; index < end; ++index) cluster = info[index].cluster < cluster ? info[index].cluster : cluster;
    // Whatever shares a cluster with the last item comes too, and with the
    // first.
    if (cluster != info[end - 1].cluster) {
        while (end < buffer.len && info[end - 1].cluster == info[end].cluster) end = end + 1;
    }
    if (cluster != info[start].cluster) {
        while (buffer.idx < start && info[start - 1].cluster == info[start].cluster) start = start - 1;
    }
    // At the beginning of what is unread, the items already written that
    // share the first item's cluster come too.
    const u32 start_cluster = info[start].cluster;
    usize written = buffer.out_len;
    if (buffer.idx == start && start_cluster != cluster) {
        while (written != 0 && written <= buffer.out.count() && buffer.out[written - 1].cluster == start_cluster) {
            set_cluster(buffer.out[written - 1], cluster, 0);
            written = written - 1;
        }
    }
    static_cast<void>(spend(buffer, (wanted_start - start) + (end - wanted_end) + (buffer.out_len - written)));
    for (usize index = start; index < end; ++index) set_cluster(info[index], cluster, 0);
}

void merge_out_clusters_now(ShapeBuffer& buffer, const usize wanted_start, const usize wanted_end) noexcept {
    usize start = wanted_start;
    usize end = wanted_end;
    if (!spend(buffer, end - start)) return;
    if (end > buffer.out_len || end > buffer.out.count() || start >= end) {
        buffer.successful = false;
        return;
    }
    Vector<ShapeGlyph>& out = buffer.out;
    u32 cluster = out[start].cluster;
    for (usize index = start + 1; index < end; ++index) cluster = out[index].cluster < cluster ? out[index].cluster : cluster;
    usize further = 0;
    if (cluster != out[start].cluster) {
        while (start != 0 && out[start - 1].cluster == out[start].cluster) {
            start = start - 1;
            further = further + 1;
        }
    }
    if (cluster != out[end - 1].cluster) {
        while (end < buffer.out_len && out[end - 1].cluster == out[end].cluster) {
            end = end + 1;
            further = further + 1;
        }
    }
    // At the end of what is written, the unread items that share the last
    // item's cluster come too.
    const u32 end_cluster = out[end - 1].cluster;
    if (end == buffer.out_len && end_cluster != cluster) {
        usize index = buffer.idx;
        while (index < buffer.len && buffer.info[index].cluster == end_cluster) {
            set_cluster(buffer.info[index], cluster, 0);
            index = index + 1;
        }
        further = further + (index - buffer.idx);
    }
    static_cast<void>(spend(buffer, further));
    for (usize index = start; index < end; ++index) set_cluster(out[index], cluster, 0);
}

// The items [idx, len) move up by `count`, leaving room before them.
[[nodiscard]] bool shift_forward(ShapeBuffer& buffer, const usize count) noexcept {
    if (!buffer.ensure(buffer.len + count)) return false;
    if (!spend(buffer, buffer.len - buffer.idx)) return false;
    const usize moved = buffer.len - buffer.idx;
    for (usize index = moved; index > 0; --index) buffer.info[buffer.idx + count + index - 1] = buffer.info[buffer.idx + index - 1];
    for (usize index = moved; index < count; ++index) buffer.info[buffer.idx + index] = ShapeGlyph{};
    buffer.len = buffer.len + count;
    buffer.idx = buffer.idx + count;
    return true;
}

} // namespace

void set_cluster(ShapeGlyph& item, const u32 cluster, const u32 mask) noexcept {
    if (item.cluster != cluster) item.mask = (item.mask & ~glyph_flags_defined) | (mask & glyph_flags_defined);
    item.cluster = cluster;
}

void ShapeBuffer::clear() noexcept {
    direction = ShapeDirection::unset;
    script = 0;
    successful = true;
    have_output = false;
    have_positions = false;
    idx = 0;
    len = 0;
    out_len = 0;
    context_len[0] = 0;
    context_len[1] = 0;
    serial = 0;
    scratch = 0;
    max_len = most_length_default;
    max_ops = most_operations_default;
}

void ShapeBuffer::reset() noexcept {
    clear();
    flags = 0;
    cluster_level = ClusterLevel::monotone_graphemes;
    has_invisible = false;
    invisible = 0;
}

bool ShapeBuffer::grow(const usize size) noexcept {
    if (size <= info.count()) return true;
    if (size > max_len) {
        successful = false;
        return false;
    }
    // At least twice what there is, so that a run added a character at a
    // time grows the arrays a few times and not once a character.
    usize wanted = info.count() * 2 > 32 ? info.count() * 2 : 32;
    if (wanted < size) wanted = size;
    if (wanted > max_len) wanted = max_len;
    if (info.resize(wanted).failed() || out.resize(wanted).failed() || pos.resize(wanted).failed()) {
        successful = false;
        return false;
    }
    return true;
}

void ShapeBuffer::add(const u32 character, const u32 cluster) noexcept {
    if (!ensure(len + 1)) return;
    ShapeGlyph item{};
    item.glyph = character;
    item.cluster = cluster;
    info[len] = item;
    len = len + 1;
}

void ShapeBuffer::add_context(const usize side, const u32 character) noexcept {
    if (side > 1 || context_len[side] >= context_length) return;
    context[side][context_len[side]] = character;
    context_len[side] = context_len[side] + 1;
}

void ShapeBuffer::reverse() noexcept { reverse_range(0, len); }

void ShapeBuffer::reverse_range(const usize start, const usize end) noexcept {
    if (end > len || end < start || end - start < 2) return;
    usize low = start;
    usize high = end - 1;
    while (low < high) {
        const ShapeGlyph item = info[low];
        info[low] = info[high];
        info[high] = item;
        if (have_positions) {
            const ShapePosition position = pos[low];
            pos[low] = pos[high];
            pos[high] = position;
        }
        low = low + 1;
        high = high - 1;
    }
}

void ShapeBuffer::reverse_groups(const SameGroup same, const bool merge) noexcept {
    if (len == 0) return;
    usize start = 0;
    usize index = 1;
    for (; index < len; ++index) {
        if (!same(info[index - 1], info[index])) {
            if (merge) merge_clusters(start, index);
            reverse_range(start, index);
            start = index;
        }
    }
    if (merge) merge_clusters(start, index);
    reverse_range(start, index);
    reverse();
}

void ShapeBuffer::reverse_clusters() noexcept { reverse_groups(same_cluster, false); }

void ShapeBuffer::reset_clusters() noexcept {
    for (usize index = 0; index < len; ++index) info[index].cluster = static_cast<u32>(index);
}

usize ShapeBuffer::group_end(const usize start, const SameGroup same) const noexcept {
    usize end = start + 1;
    while (end < len && same(info[end - 1], info[end])) end = end + 1;
    return end;
}

void ShapeBuffer::clear_output() noexcept {
    have_output = true;
    have_positions = false;
    idx = 0;
    out_len = 0;
}

bool ShapeBuffer::sync() noexcept {
    if (!successful) {
        have_output = false;
        out_len = 0;
        idx = 0;
        return false;
    }
    next_glyphs(len - idx);
    if (!successful) {
        have_output = false;
        out_len = 0;
        idx = 0;
        return false;
    }
    // What was written becomes the items.
    Vector<ShapeGlyph> written = static_cast<Vector<ShapeGlyph>&&>(out);
    out = static_cast<Vector<ShapeGlyph>&&>(info);
    info = static_cast<Vector<ShapeGlyph>&&>(written);
    len = out_len;
    have_output = false;
    out_len = 0;
    idx = 0;
    STXLIB_GLYPHS_CHECK(whole(), "ShapeBuffer::sync: the buffer is not whole");
    return true;
}

bool ShapeBuffer::whole() const noexcept {
    const bool result = info.count() == out.count() && info.count() == pos.count() && len <= info.count() && out_len <= out.count() &&
                        (have_output || out_len == 0) && (!successful || idx <= len) && context_len[0] <= context_length &&
                        context_len[1] <= context_length;
    return result;
}

void ShapeBuffer::clear_positions() noexcept {
    have_output = false;
    have_positions = true;
    out_len = 0;
    for (usize index = 0; index < len && index < pos.count(); ++index) pos[index] = ShapePosition{};
}

void ShapeBuffer::replace_glyphs(const usize num_in, const View<u32> glyphs) noexcept {
    const usize num_out = glyphs.count();
    if (!ensure(out_len + num_out)) return;
    if (idx + num_in > len || idx >= len) {
        successful = false;
        return;
    }
    merge_clusters(idx, idx + num_in);
    const ShapeGlyph original = info[idx];
    for (usize index = 0; index < num_out; ++index) {
        ShapeGlyph item = original;
        item.glyph = glyphs[index];
        out[out_len + index] = item;
    }
    idx = idx + num_in;
    out_len = out_len + num_out;
}

void ShapeBuffer::replace_glyph(const u32 glyph) noexcept {
    if (!ensure(out_len + 1)) return;
    if (idx >= len) {
        successful = false;
        return;
    }
    ShapeGlyph item = info[idx];
    item.glyph = glyph;
    out[out_len] = item;
    idx = idx + 1;
    out_len = out_len + 1;
}

void ShapeBuffer::output_glyph(const u32 glyph) noexcept {
    if (!ensure(out_len + 1)) return;
    if (idx == len && out_len == 0) return;
    ShapeGlyph item = idx < len ? info[idx] : out[out_len - 1];
    item.glyph = glyph;
    out[out_len] = item;
    out_len = out_len + 1;
}

void ShapeBuffer::output_info(const ShapeGlyph& item) noexcept {
    if (!ensure(out_len + 1)) return;
    out[out_len] = item;
    out_len = out_len + 1;
}

void ShapeBuffer::copy_glyph() noexcept {
    if (!ensure(out_len + 1)) return;
    if (idx >= len) {
        successful = false;
        return;
    }
    out[out_len] = info[idx];
    out_len = out_len + 1;
}

bool ShapeBuffer::move_to(const usize place) noexcept {
    if (!have_output) {
        idx = place;
        return true;
    }
    if (!successful) return false;
    if (out_len < place) {
        const usize count = place - out_len;
        if (!ensure(out_len + count)) return false;
        if (!spend(*this, count)) return false;
        if (idx + count > len) {
            successful = false;
            return false;
        }
        for (usize index = 0; index < count; ++index) out[out_len + index] = info[idx + index];
        idx = idx + count;
        out_len = out_len + count;
    } else if (out_len > place) {
        // Going back: what was written is read again.
        const usize count = out_len - place;
        if (idx < count && !shift_forward(*this, count - idx)) return false;
        if (!spend(*this, count)) return false;
        idx = idx - count;
        out_len = out_len - count;
        for (usize index = 0; index < count; ++index) info[idx + index] = out[out_len + index];
    }
    return true;
}

void ShapeBuffer::set_masks(const u32 value, const u32 mask, const u32 cluster_start, const u32 cluster_end) noexcept {
    if (mask == 0 || max_ops < 0) return;
    if (!spend(*this, len)) return;
    const u32 bits = value & mask;
    const bool everywhere = cluster_start == 0 && cluster_end == 0xFFFFFFFFU;
    for (usize index = 0; index < len; ++index) {
        ShapeGlyph& item = info[index];
        if (everywhere || (cluster_start <= item.cluster && item.cluster < cluster_end)) item.mask = (item.mask & ~mask) | bits;
    }
}

void ShapeBuffer::merge_clusters(const usize start, const usize end) noexcept {
    if (end < start || end - start < 2) return;
    if (!is_monotone(cluster_level)) {
        unsafe_to_break(start, end);
        return;
    }
    merge_clusters_now(*this, start, end);
}

void ShapeBuffer::merge_grapheme_clusters(const usize start, const usize end) noexcept {
    if (end < start || end - start < 2) return;
    if (!is_graphemes(cluster_level)) {
        unsafe_to_break(start, end);
        return;
    }
    merge_clusters_now(*this, start, end);
}

void ShapeBuffer::merge_out_clusters(const usize start, const usize end) noexcept {
    if (end < start || end - start < 2) return;
    if (!is_monotone(cluster_level)) return;
    merge_out_clusters_now(*this, start, end);
}

void ShapeBuffer::merge_out_grapheme_clusters(const usize start, const usize end) noexcept {
    if (end < start || end - start < 2) return;
    if (!is_graphemes(cluster_level)) return;
    merge_out_clusters_now(*this, start, end);
}

void ShapeBuffer::delete_glyph() noexcept {
    if (idx >= len) {
        successful = false;
        return;
    }
    const u32 cluster = info[idx].cluster;
    const u32 mask = info[idx].mask;
    const bool next_shares = idx + 1 < len && info[idx + 1].cluster == cluster;
    const bool has_previous = out_len != 0 && out_len <= out.count();
    const bool previous_shares = has_previous && out[out_len - 1].cluster == cluster;
    if (!next_shares && !previous_shares) {
        if (has_previous) {
            // Its cluster goes to what was written before it.
            const u32 old_cluster = out[out_len - 1].cluster;
            if (cluster < old_cluster && max_ops >= 0) {
                usize index = out_len;
                while (index != 0 && out[index - 1].cluster == old_cluster) {
                    set_cluster(out[index - 1], cluster, mask);
                    index = index - 1;
                }
                static_cast<void>(spend(*this, out_len - index));
            }
        } else if (idx + 1 < len) {
            // Or to what comes after it.
            merge_clusters(idx, idx + 2);
        }
    }
    skip_glyph();
}

void ShapeBuffer::delete_glyphs_in_place(const Unwanted unwanted) noexcept {
    usize kept = 0;
    for (usize index = 0; index < len; ++index) {
        const ShapeGlyph item = info[index];
        if (unwanted(item)) {
            // As delete_glyph has it, with the kept items for the output.
            if (index + 1 < len && info[index + 1].cluster == item.cluster) continue;
            if (kept != 0) {
                const u32 old_cluster = info[kept - 1].cluster;
                if (item.cluster < old_cluster && max_ops >= 0) {
                    for (usize back = kept; back > 0 && info[back - 1].cluster == old_cluster; --back) {
                        set_cluster(info[back - 1], item.cluster, item.mask);
                        max_ops = max_ops > -0x7FFFFFFF ? max_ops - 1 : max_ops;
                    }
                    if (max_ops < 0) successful = false;
                }
                continue;
            }
            if (index + 1 < len) merge_clusters(index, index + 2);
            continue;
        }
        if (kept != index) {
            info[kept] = item;
            pos[kept] = pos[index];
        }
        kept = kept + 1;
    }
    len = kept;
}

void ShapeBuffer::unsafe_to_break(const usize start, const usize end) noexcept {
    set_glyph_flags(*this, glyph_unsafe_to_break | glyph_unsafe_to_concat, start, end, true, false);
}

void ShapeBuffer::unsafe_to_concat(const usize start, const usize end) noexcept {
    if ((flags & buffer_produce_unsafe_to_concat) == 0) return;
    set_glyph_flags(*this, glyph_unsafe_to_concat, start, end, false, false);
}

void ShapeBuffer::unsafe_to_break_from_output(const usize start, const usize end) noexcept {
    set_glyph_flags(*this, glyph_unsafe_to_break | glyph_unsafe_to_concat, start, end, true, true);
}

void ShapeBuffer::unsafe_to_concat_from_output(const usize start, const usize end) noexcept {
    if ((flags & buffer_produce_unsafe_to_concat) == 0) return;
    set_glyph_flags(*this, glyph_unsafe_to_concat, start, end, false, true);
}

void ShapeBuffer::sort(const usize start, const usize end, const SortsAfter after) noexcept {
    if (end > len) {
        successful = false;
        return;
    }
    for (usize index = start + 1; index < end; ++index) {
        const ShapeGlyph item = info[index];
        usize place = index;
        while (place > start && after(info[place - 1], item)) place = place - 1;
        if (place == index) continue;
        // The item goes to `place`; what was between moves up by one.
        merge_clusters(place, index + 1);
        const ShapeGlyph moved = info[index];
        for (usize back = index; back > place; --back) info[back] = info[back - 1];
        info[place] = moved;
    }
}

void ShapeBuffer::enter() noexcept {
    serial = 0;
    scratch = 0;
    if (len <= (~static_cast<usize>(0)) / most_length_factor) {
        const usize most = len * most_length_factor;
        max_len = most > most_length_least ? most : most_length_least;
    }
    if (len <= static_cast<usize>(0x7FFFFFFF / most_operations_factor)) {
        const i32 most = static_cast<i32>(len) * most_operations_factor;
        max_ops = most > most_operations_least ? most : most_operations_least;
    }
}

void ShapeBuffer::leave() noexcept {
    max_len = most_length_default;
    max_ops = most_operations_default;
    serial = 0;
}

u32 ShapeBuffer::allocate_ligature_id() noexcept {
    u32 id = 0;
    // The serial goes round 1 to 255; one whose low three bits are nothing
    // is passed over, so this ends within eight steps.
    for (u32 step = 0; step < 8 && id == 0; ++step) {
        serial = static_cast<u8>(serial == 255 ? 1 : serial + 1);
        id = serial & 0x07U;
    }
    return id;
}

void ShapeBuffer::release() noexcept {
    clear();
    info.release();
    out.release();
    pos.release();
}

} // namespace stxlib::glyphs::detail
