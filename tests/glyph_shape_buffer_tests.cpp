// stxlib::glyphs, the shaper's buffer (src/algorithms/glyphs/shape_buffer.hpp):
// every expected glyph and cluster below is worked out by hand from what
// the buffer is said to do.
//
//   B1  characters added, with their clusters; the context kept to five a
//       side; clear and reset;
//   B2  a pass of substitution: items passed on, one replaced, one put in,
//       and the output made the items;
//   B3  several items replaced by one, and one by several: clusters joined
//       where the cluster level says so, marked unsafe to break where not;
//   B4  joining clusters: what shares a cluster with the ends comes too,
//       before the stretch, after it, and in what is already written;
//   B5  an item dropped: its cluster goes to a neighbour if it would be
//       lost; items dropped in place, positions with them;
//   B6  reading moved back and forward; a sort that joins clusters;
//       groups reversed;
//   B7  unsafe to break: which items are marked;
//   B8  the limits: a most length, operations spent, and ligature ids;
//       and the check that a buffer agrees with itself, which sees one
//       that does not.

#include "algorithms/glyphs/shape_buffer.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <vector>

namespace {

namespace detail = stxlib::glyphs::detail;
using stxlib::u32;
using stxlib::usize;

struct Item final {
    u32 glyph{0};
    u32 cluster{0};
};

void fill(detail::ShapeBuffer& buffer, const std::vector<Item>& items) {
    buffer.clear();
    for (const Item& item : items) buffer.add(item.glyph, item.cluster);
}

[[nodiscard]] bool holds(const detail::ShapeBuffer& buffer, const std::vector<Item>& items) {
    bool same = buffer.len == items.size();
    for (usize index = 0; same && index < items.size(); ++index) {
        same = buffer.info[index].glyph == items[index].glyph && buffer.info[index].cluster == items[index].cluster;
    }
    if (!same) {
        std::fprintf(stderr, "  buffer holds:");
        for (usize index = 0; index < buffer.len; ++index) std::fprintf(stderr, " %u/%u", buffer.info[index].glyph, buffer.info[index].cluster);
        std::fprintf(stderr, "\n");
    }
    return same;
}

[[nodiscard]] u32 flags_of(const detail::ShapeBuffer& buffer, const usize index) {
    const u32 result = buffer.info[index].mask & detail::glyph_flags_defined;
    return result;
}

void adding() {
    detail::ShapeBuffer buffer{};
    buffer.add('a', 0);
    buffer.add('b', 1);
    buffer.add(0x1F600, 2);
    CHECK(buffer.successful && holds(buffer, {{'a', 0}, {'b', 1}, {0x1F600, 2}}) && buffer.lookahead_len() == 3 && buffer.backtrack_len() == 0);
    for (u32 character = 0; character < 8; ++character) {
        buffer.add_context(0, 100 + character);
        buffer.add_context(1, 200 + character);
    }
    CHECK(buffer.context_len[0] == 5 && buffer.context_len[1] == 5 && buffer.context[0][0] == 100 && buffer.context[0][4] == 104 &&
          buffer.context[1][4] == 204);
    buffer.flags = detail::buffer_end_of_text;
    buffer.cluster_level = detail::ClusterLevel::characters;
    buffer.clear();
    CHECK(buffer.len == 0 && buffer.context_len[0] == 0 && buffer.flags == detail::buffer_end_of_text &&
          buffer.cluster_level == detail::ClusterLevel::characters);
    buffer.reset();
    CHECK(buffer.flags == 0 && buffer.cluster_level == detail::ClusterLevel::monotone_graphemes);
}

void substitution_pass() {
    detail::ShapeBuffer buffer{};
    fill(buffer, {{'a', 0}, {'b', 1}, {'c', 2}});
    buffer.clear_output();
    buffer.next_glyph();
    CHECK(buffer.backtrack_len() == 1 && buffer.lookahead_len() == 2);
    buffer.replace_glyph('X');
    // A glyph put in before c is of c's cluster.
    buffer.output_glyph('Y');
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'X', 1}, {'Y', 2}, {'c', 2}}));
    // Put in after the last item, it is of the last one written.
    buffer.clear_output();
    buffer.next_glyphs(4);
    buffer.output_glyph('Z');
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'X', 1}, {'Y', 2}, {'c', 2}, {'Z', 2}}));
    // Nothing read and nothing written: nothing to take after.
    fill(buffer, {});
    buffer.clear_output();
    buffer.output_glyph('Q');
    CHECK(buffer.sync() && buffer.len == 0);
    // An item given whole, and one copied without being passed.
    fill(buffer, {{'a', 7}});
    buffer.clear_output();
    detail::ShapeGlyph extra{};
    extra.glyph = 'E';
    extra.cluster = 3;
    buffer.output_info(extra);
    buffer.copy_glyph();
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'E', 3}, {'a', 7}, {'a', 7}}));
}

void replacing() {
    detail::ShapeBuffer buffer{};
    const u32 ligature[] = {'L'};
    const u32 three[] = {'x', 'y', 'z'};
    // Two into one: the two clusters become the lesser.
    fill(buffer, {{'a', 0}, {'f', 1}, {'i', 2}, {'b', 3}});
    buffer.clear_output();
    buffer.next_glyph();
    buffer.replace_glyphs(2, stxlib::View<u32>(ligature));
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'L', 1}, {'b', 3}}));
    // One into three: all of its cluster.
    fill(buffer, {{'a', 0}, {'w', 4}, {'b', 9}});
    buffer.clear_output();
    buffer.next_glyph();
    buffer.replace_glyphs(1, stxlib::View<u32>(three));
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'x', 4}, {'y', 4}, {'z', 4}, {'b', 9}}));
    // Where clusters are kept a character each, nothing is joined: the
    // ligature keeps its first item's cluster, and the stretch is marked.
    buffer.reset();
    buffer.cluster_level = detail::ClusterLevel::characters;
    buffer.add('a', 0);
    buffer.add('f', 1);
    buffer.add('i', 2);
    buffer.clear_output();
    buffer.next_glyph();
    buffer.replace_glyphs(2, stxlib::View<u32>(ligature));
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'L', 1}}));
}

void joining() {
    detail::ShapeBuffer buffer{};
    // The second and third items: the least is 0, and the fourth shares
    // the third's cluster.
    fill(buffer, {{'a', 0}, {'b', 0}, {'c', 1}, {'d', 1}, {'e', 2}});
    buffer.merge_clusters(1, 3);
    CHECK(holds(buffer, {{'a', 0}, {'b', 0}, {'c', 0}, {'d', 0}, {'e', 2}}));
    // Right to left text has clusters going down: the least is at the end,
    // and what shares the first one's cluster before the stretch comes too.
    fill(buffer, {{'a', 5}, {'b', 5}, {'c', 3}, {'d', 1}});
    buffer.merge_clusters(1, 3);
    CHECK(holds(buffer, {{'a', 3}, {'b', 3}, {'c', 3}, {'d', 1}}));
    // One item, or none: nothing.
    buffer.merge_clusters(2, 3);
    buffer.merge_clusters(3, 3);
    CHECK(holds(buffer, {{'a', 3}, {'b', 3}, {'c', 3}, {'d', 1}}));
    // In a pass, with two items of cluster 5 written and the same cluster
    // next to be read: joined with a 3, the written ones go to 3 as well.
    fill(buffer, {{'a', 5}, {'b', 5}, {'c', 5}, {'d', 3}});
    buffer.clear_output();
    buffer.next_glyphs(2);
    buffer.merge_clusters(2, 4);
    CHECK(buffer.sync() && holds(buffer, {{'a', 3}, {'b', 3}, {'c', 3}, {'d', 3}}));
    // In what is written: the last two written are joined, and the unread
    // item that shares the last one's cluster follows.
    fill(buffer, {{'a', 1}, {'b', 2}, {'c', 4}, {'d', 4}, {'e', 6}});
    buffer.clear_output();
    buffer.next_glyphs(3);
    buffer.merge_out_clusters(1, 3);
    CHECK(buffer.sync() && holds(buffer, {{'a', 1}, {'b', 2}, {'c', 2}, {'d', 2}, {'e', 6}}));
    // Graphemes only: with clusters a character each and monotone, a
    // grapheme's items are not joined, and are marked instead.
    buffer.reset();
    buffer.cluster_level = detail::ClusterLevel::monotone_characters;
    buffer.add('a', 0);
    buffer.add('m', 1);
    buffer.add('b', 2);
    buffer.merge_grapheme_clusters(0, 2);
    CHECK(holds(buffer, {{'a', 0}, {'m', 1}, {'b', 2}}) && flags_of(buffer, 0) == 0 && flags_of(buffer, 1) != 0);
    buffer.merge_clusters(0, 2);
    CHECK(holds(buffer, {{'a', 0}, {'m', 0}, {'b', 2}}));
}

[[nodiscard]] bool is_x(const detail::ShapeGlyph& item) noexcept {
    const bool result = item.glyph == 'x';
    return result;
}

void dropping() {
    detail::ShapeBuffer buffer{};
    // The middle one of three: its cluster, 1, is more than the one
    // written before it, so nothing takes it.
    fill(buffer, {{'a', 0}, {'x', 1}, {'c', 2}});
    buffer.clear_output();
    buffer.next_glyph();
    buffer.delete_glyph();
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'c', 2}}));
    // The first one: the next takes its cluster.
    fill(buffer, {{'x', 0}, {'b', 1}, {'c', 2}});
    buffer.clear_output();
    buffer.delete_glyph();
    buffer.next_glyphs(2);
    CHECK(buffer.sync() && holds(buffer, {{'b', 0}, {'c', 2}}));
    // Right to left: the one dropped has a lesser cluster than what was
    // written, which takes it.
    fill(buffer, {{'a', 2}, {'x', 1}, {'c', 0}});
    buffer.clear_output();
    buffer.next_glyph();
    buffer.delete_glyph();
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 1}, {'c', 0}}));
    // One of two that share a cluster: nothing changes.
    fill(buffer, {{'a', 0}, {'x', 0}, {'c', 2}});
    buffer.clear_output();
    buffer.next_glyph();
    buffer.delete_glyph();
    buffer.next_glyph();
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'c', 2}}));
    // In place, with positions: the x's go, the first one's cluster to b.
    fill(buffer, {{'x', 0}, {'b', 1}, {'x', 2}, {'d', 3}, {'x', 4}});
    buffer.clear_positions();
    for (usize index = 0; index < 5; ++index) buffer.pos[index].x_advance = static_cast<stxlib::i32>(10 * (index + 1));
    buffer.delete_glyphs_in_place(is_x);
    CHECK(holds(buffer, {{'b', 0}, {'d', 3}}) && buffer.pos[0].x_advance == 20 && buffer.pos[1].x_advance == 40);
}

[[nodiscard]] bool sorts_after(const detail::ShapeGlyph& first, const detail::ShapeGlyph& second) noexcept {
    const bool result = first.stage_data > second.stage_data;
    return result;
}

void moving() {
    detail::ShapeBuffer buffer{};
    // Three written; back to one written: the two are read again.
    fill(buffer, {{'a', 0}, {'b', 1}, {'c', 2}, {'d', 3}});
    buffer.clear_output();
    buffer.next_glyphs(3);
    CHECK(buffer.move_to(1) && buffer.out_len == 1 && buffer.idx == 1 && buffer.info[buffer.idx].glyph == 'b');
    buffer.replace_glyph('B');
    CHECK(buffer.move_to(4) && buffer.out_len == 4 && buffer.idx == 4);
    CHECK(buffer.sync() && holds(buffer, {{'a', 0}, {'B', 1}, {'c', 2}, {'d', 3}}));
    // Back past where reading is, after more was written than read: the
    // unread items make room.
    const u32 three[] = {'x', 'y', 'z'};
    fill(buffer, {{'w', 0}, {'e', 1}});
    buffer.clear_output();
    buffer.replace_glyphs(1, stxlib::View<u32>(three));
    CHECK(buffer.move_to(0) && buffer.out_len == 0 && buffer.len == 4 && buffer.lookahead_len() == 4);
    CHECK(buffer.sync() && holds(buffer, {{'x', 0}, {'y', 0}, {'z', 0}, {'e', 1}}));
    // Without an output, reading just goes where it is told.
    CHECK(buffer.move_to(2) && buffer.idx == 2);
    // A sort by the items' numbers: 3, 1, 2 become 1, 2, 3, and what
    // passed each other is one cluster.
    fill(buffer, {{'a', 0}, {'p', 1}, {'q', 2}, {'r', 3}, {'z', 4}});
    buffer.info[1].stage_data = 3;
    buffer.info[2].stage_data = 1;
    buffer.info[3].stage_data = 2;
    buffer.sort(1, 4, sorts_after);
    CHECK(holds(buffer, {{'a', 0}, {'q', 1}, {'r', 1}, {'p', 1}, {'z', 4}}));
    // Items already in order are left alone, clusters too.
    fill(buffer, {{'p', 1}, {'q', 2}});
    buffer.info[0].stage_data = 1;
    buffer.info[1].stage_data = 1;
    buffer.sort(0, 2, sorts_after);
    CHECK(holds(buffer, {{'p', 1}, {'q', 2}}));
    // Clusters in reverse order, each cluster's items as they were.
    fill(buffer, {{'a', 0}, {'b', 0}, {'c', 1}, {'d', 2}});
    buffer.reverse_clusters();
    CHECK(holds(buffer, {{'d', 2}, {'c', 1}, {'a', 0}, {'b', 0}}));
    buffer.reverse();
    CHECK(holds(buffer, {{'b', 0}, {'a', 0}, {'c', 1}, {'d', 2}}));
    buffer.reset_clusters();
    CHECK(holds(buffer, {{'b', 0}, {'a', 1}, {'c', 2}, {'d', 3}}));
}

void breaking() {
    detail::ShapeBuffer buffer{};
    const u32 both = detail::glyph_unsafe_to_break | detail::glyph_unsafe_to_concat;
    // Over clusters 0, 0, 1, 2: the items not of the first cluster.
    fill(buffer, {{'a', 0}, {'b', 0}, {'c', 1}, {'d', 2}});
    buffer.unsafe_to_break(0, 4);
    CHECK(flags_of(buffer, 0) == 0 && flags_of(buffer, 1) == 0 && flags_of(buffer, 2) == both && flags_of(buffer, 3) == both);
    // Right to left, clusters 2, 1, 0, 0: the least is at the end, and the
    // items before its run are marked.
    fill(buffer, {{'a', 2}, {'b', 1}, {'c', 0}, {'d', 0}});
    buffer.unsafe_to_break(0, 4);
    CHECK(flags_of(buffer, 0) == both && flags_of(buffer, 1) == both && flags_of(buffer, 2) == 0 && flags_of(buffer, 3) == 0);
    // One item, and a stretch of more than 255 that is named, are not
    // marked; "to the end" is the whole.
    fill(buffer, {{'a', 0}, {'b', 1}});
    buffer.unsafe_to_break(0, 1);
    buffer.unsafe_to_break(0, 300);
    CHECK(flags_of(buffer, 0) == 0 && flags_of(buffer, 1) == 0);
    buffer.unsafe_to_break(0, detail::to_the_end);
    CHECK(flags_of(buffer, 0) == 0 && flags_of(buffer, 1) == both);
    // Unsafe to concatenate is marked only when asked for, and then on
    // every item of the stretch.
    fill(buffer, {{'a', 0}, {'b', 1}});
    buffer.unsafe_to_concat(0, 2);
    CHECK(flags_of(buffer, 0) == 0 && flags_of(buffer, 1) == 0);
    buffer.flags = detail::buffer_produce_unsafe_to_concat;
    buffer.unsafe_to_concat(0, 2);
    CHECK(flags_of(buffer, 0) == detail::glyph_unsafe_to_concat && flags_of(buffer, 1) == detail::glyph_unsafe_to_concat);
    buffer.flags = 0;
    // From the output: from a written place to an unread item.
    fill(buffer, {{'a', 0}, {'b', 1}, {'c', 2}, {'d', 3}});
    buffer.clear_output();
    buffer.next_glyphs(2);
    buffer.unsafe_to_break_from_output(1, 3);
    CHECK(buffer.sync() && flags_of(buffer, 0) == 0 && flags_of(buffer, 1) == 0 && flags_of(buffer, 2) == both && flags_of(buffer, 3) == 0);
    // A cluster that changes loses its flags.
    detail::ShapeGlyph item{};
    item.cluster = 4;
    item.mask = 0xF0 | both;
    detail::set_cluster(item, 4, 0);
    CHECK(item.mask == (0xF0 | both));
    detail::set_cluster(item, 2, detail::glyph_unsafe_to_break);
    CHECK(item.cluster == 2 && item.mask == (0xF0 | detail::glyph_unsafe_to_break));
}

void limits() {
    detail::ShapeBuffer buffer{};
    fill(buffer, {{'a', 0}, {'b', 1}, {'c', 2}});
    buffer.enter();
    CHECK(buffer.max_len == 65536 && buffer.max_ops == 65536);
    // Operations spent.
    CHECK(buffer.charge(65536) && buffer.successful && !buffer.charge(1) && !buffer.successful);
    buffer.leave();
    CHECK(buffer.max_ops == 0x1FFFFFFF);
    // A most length: the item that would pass it is not added.
    fill(buffer, {{'a', 0}});
    buffer.max_len = 2;
    buffer.add('b', 1);
    CHECK(buffer.successful && buffer.len == 2);
    buffer.add('c', 2);
    CHECK(!buffer.successful && buffer.len == 2);
    // A pass that failed makes nothing of its output.
    buffer.clear_output();
    CHECK(!buffer.sync() && buffer.len == 2);
    // Ligature ids go 1 to 7 and round, never 0.
    fill(buffer, {});
    buffer.enter();
    bool good = true;
    const u32 expected[] = {1, 2, 3, 4, 5, 6, 7, 1, 2, 3};
    for (const u32 id : expected) good = good && buffer.allocate_ligature_id() == id;
    for (int round = 0; round < 1000; ++round) {
        const u32 id = buffer.allocate_ligature_id();
        good = good && id >= 1 && id <= 7;
    }
    CHECK(good);
    // Long runs: the limits follow the length.
    buffer.clear();
    for (u32 index = 0; index < 1000; ++index) buffer.add('a', index);
    buffer.enter();
    CHECK(buffer.max_len == 256000 && buffer.max_ops == 4096000);
    CHECK(buffer.whole());
    // The check sees a buffer that does not agree with itself.
    detail::ShapeBuffer broken{};
    fill(broken, {{'a', 0}, {'b', 1}});
    CHECK(broken.whole());
    broken.len = 3;
    CHECK(!broken.whole());
    broken.len = 2;
    broken.out_len = 1;
    CHECK(!broken.whole());
    broken.out_len = 0;
    broken.idx = 3;
    CHECK(!broken.whole());
    broken.idx = 0;
    broken.context_len[1] = 6;
    CHECK(!broken.whole());
    broken.context_len[1] = 0;
    CHECK(broken.whole());
    buffer.release();
    CHECK(buffer.len == 0 && buffer.info.count() == 0 && buffer.whole());
}

} // namespace

int main() {
    adding();
    substitution_pass();
    replacing();
    joining();
    dropping();
    moving();
    breaking();
    limits();
    const int status = stxlib_tests::finish("stxlib_glyph_shape_buffer_tests");
    return status;
}
