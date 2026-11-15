// Tests of FrozenMap and FrozenTextMap (frozen_map.hpp, R5).
//
// What is exhaustive here, and over which domain (proofs/REPORT.md):
// - FrozenMap, every subset of eleven keys at the encoding edges (0..3,
//   0xFF, 0x100, 0xFFFF, 0x10000, 2^32, 2^64 - 2, 2^64 - 1), built uniform
//   and weighted: find, contains, lower_bound, key_at and value_at equal a
//   sorted model for every key and its two neighbours;
// - FrozenMap, every count from 0 to 300 at five strides (1, 3, 257, 65537,
//   2^40 + 1), uniform and weighted (skewed weights, Knuth): the same checks
//   at and around every key, so trees of several node levels are covered;
//   and 5000 keys (the bottom-up construction above knuth_limit) against
//   every query from 0 to 15002;
// - FrozenTextMap, every subset of the 13 texts of up to two characters over
//   {a, b, é}, uniform and weighted, against every text of up to three; the
//   511 texts of up to eight bytes over {a, b}, bare and behind a 70-byte
//   common prefix (node prefixes beyond the inline windows), against every
//   text of up to nine; every subset of the first 12 of the 31 texts of up to
//   two characters over {NUL, a, DEL, é, U+10FFFF}, and all 31;
// - refusals: unsorted, repeated, mismatched lengths and weights, and a
//   refused allocation, each with nothing built.

#include "core/checked.hpp"
#include "stxlib/detail/vocabulary_allocation.hpp"
#include "stxlib/frozen_map.hpp"
#include "stxlib/text.hpp"

#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using stxlib::u64;
using stxlib::usize;

u64 value_for(const u64 key) {
    const u64 result = key * 0x9e3779b97f4a7c15ULL + 1;
    return result;
}

// ---- numbers ------------------------------------------------------------------

bool check_number_map(const stxlib::FrozenMap& map, const std::vector<u64>& keys, const std::vector<u64>& queries) {
    bool same = map.count() == keys.size();
    for (usize index = 0; same && index < keys.size(); ++index) {
        same = map.key_at(index) == keys[index] && map.value_at(index) == value_for(keys[index]);
        same = same && map.keys()[index] == keys[index] && map.values()[index] == value_for(keys[index]);
    }
    for (const u64 query : queries) {
        const std::vector<u64>::const_iterator position = std::lower_bound(keys.begin(), keys.end(), query);
        const usize expected_bound = static_cast<usize>(position - keys.begin());
        const bool present = position != keys.end() && *position == query;
        const u64* const found = map.find(query);
        same = same && map.lower_bound(query) == expected_bound;
        same = same && (found != nullptr) == present && map.contains(query) == present;
        if (present && found != nullptr) same = same && *found == value_for(query);
    }
    return same;
}

// Each point's neighbours wrap modulo 2^64 (0 - 1 is the largest key).
STXLIB_MODULAR std::vector<u64> neighbours_of(const std::vector<u64>& points) {
    std::vector<u64> result;
    for (const u64 point : points) {
        result.push_back(point - 1);
        result.push_back(point);
        result.push_back(point + 1);
    }
    result.push_back(0);
    result.push_back(~static_cast<u64>(0));
    return result;
}

void check_number_builds(const std::vector<u64>& keys, const std::vector<u64>& queries) {
    std::vector<u64> values;
    std::vector<u64> key_weights;
    std::vector<u64> gap_weights;
    for (usize index = 0; index < keys.size(); ++index) {
        values.push_back(value_for(keys[index]));
        key_weights.push_back(1000 / (1 + index % 17));
        gap_weights.push_back(index % 3);
    }
    gap_weights.push_back(1);
    const stxlib::View<u64> key_view(keys.data(), keys.size());
    const stxlib::View<u64> value_view(values.data(), values.size());

    stxlib::Result<stxlib::FrozenMap> uniform = stxlib::FrozenMap::build(key_view, value_view);
    CHECK(uniform.succeeded() && check_number_map(*uniform.value(), keys, queries));
    stxlib::Result<stxlib::FrozenMap> weighted = stxlib::FrozenMap::build_weighted(
        key_view, value_view, stxlib::View<u64>(key_weights.data(), key_weights.size()),
        stxlib::View<u64>(gap_weights.data(), gap_weights.size()));
    CHECK(weighted.succeeded() && check_number_map(*weighted.value(), keys, queries));
}

void number_subsets_exhaustive() {
    const u64 universe[] = {0, 1, 2, 3, 0xff, 0x100, 0xffff, 0x10000, 0x100000000ULL, ~static_cast<u64>(1),
                            ~static_cast<u64>(0)};
    const usize size = sizeof(universe) / sizeof(universe[0]);
    const std::vector<u64> queries = neighbours_of(std::vector<u64>(universe, universe + size));
    for (usize subset = 0; subset < (static_cast<usize>(1) << size); ++subset) {
        std::vector<u64> keys;
        for (usize bit = 0; bit < size; ++bit) {
            if ((subset >> bit) & 1) keys.push_back(universe[bit]);
        }
        check_number_builds(keys, queries);
    }
}

void number_families() {
    const u64 strides[] = {1, 3, 257, 65537, (static_cast<u64>(1) << 40) + 1};
    for (const u64 stride : strides) {
        for (usize count = 0; count <= 300; ++count) {
            std::vector<u64> keys;
            for (usize index = 0; index < count; ++index) keys.push_back(5 + static_cast<u64>(index) * stride);
            check_number_builds(keys, neighbours_of(keys));
        }
    }
    std::vector<u64> keys;
    for (u64 index = 0; index < 5000; ++index) keys.push_back(index * 3);
    std::vector<u64> queries;
    for (u64 query = 0; query <= 15002; ++query) queries.push_back(query);
    check_number_builds(keys, queries);
}

// ---- text ---------------------------------------------------------------------

stxlib::TextView view_of(const std::string& text) {
    const stxlib::Result<stxlib::TextView> validated = stxlib::validate_utf8(text.data(), text.size());
    if (validated.failed()) return stxlib::TextView();
    const stxlib::TextView result = *validated.value();
    return result;
}

std::vector<std::string> all_texts(const std::vector<std::string>& pieces, const usize longest) {
    std::vector<std::string> result{std::string()};
    usize start = 0;
    for (usize length = 1; length <= longest; ++length) {
        const usize end = result.size();
        for (usize index = start; index < end; ++index) {
            for (const std::string& piece : pieces) result.push_back(result[index] + piece);
        }
        start = end;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

u64 text_value(const std::string& text) {
    u64 result = 0xcbf29ce484222325ULL;
    for (const char byte : text) result = (result ^ static_cast<unsigned char>(byte)) * 0x100000001b3ULL;
    return result;
}

bool check_text_map(const stxlib::FrozenTextMap& map, const std::vector<std::string>& keys,
                    const std::vector<std::string>& queries) {
    bool same = map.count() == keys.size();
    for (usize index = 0; same && index < keys.size(); ++index) {
        same = map.key_at(index) == view_of(keys[index]) && map.value_at(index) == text_value(keys[index]);
    }
    for (const std::string& query : queries) {
        const std::vector<std::string>::const_iterator position = std::lower_bound(keys.begin(), keys.end(), query);
        const usize expected_bound = static_cast<usize>(position - keys.begin());
        const bool present = position != keys.end() && *position == query;
        const stxlib::TextView text = view_of(query);
        const u64* const found = map.find(text);
        same = same && map.lower_bound(text) == expected_bound;
        same = same && (found != nullptr) == present && map.contains(text) == present;
        if (present && found != nullptr) same = same && *found == text_value(query);
    }
    return same;
}

void check_text_builds(const std::vector<std::string>& keys, const std::vector<std::string>& queries) {
    std::vector<stxlib::TextView> views;
    std::vector<u64> values;
    std::vector<u64> key_weights;
    std::vector<u64> gap_weights;
    for (usize index = 0; index < keys.size(); ++index) {
        views.push_back(view_of(keys[index]));
        values.push_back(text_value(keys[index]));
        key_weights.push_back(1 + (index * 7) % 11);
        gap_weights.push_back(index % 2);
    }
    gap_weights.push_back(3);
    const stxlib::View<stxlib::TextView> key_view(views.data(), views.size());
    const stxlib::View<u64> value_view(values.data(), values.size());
    stxlib::Result<stxlib::FrozenTextMap> uniform = stxlib::FrozenTextMap::build(key_view, value_view);
    CHECK(uniform.succeeded() && check_text_map(*uniform.value(), keys, queries));
    stxlib::Result<stxlib::FrozenTextMap> weighted = stxlib::FrozenTextMap::build_weighted(
        key_view, value_view, stxlib::View<u64>(key_weights.data(), key_weights.size()),
        stxlib::View<u64>(gap_weights.data(), gap_weights.size()));
    CHECK(weighted.succeeded() && check_text_map(*weighted.value(), keys, queries));
}

void check_subsets(const std::vector<std::string>& candidates, const std::vector<std::string>& queries) {
    for (usize subset = 0; subset < (static_cast<usize>(1) << candidates.size()); ++subset) {
        std::vector<std::string> keys;
        for (usize bit = 0; bit < candidates.size(); ++bit) {
            if ((subset >> bit) & 1) keys.push_back(candidates[bit]);
        }
        check_text_builds(keys, queries);
    }
}

void text_exhaustive() {
    const std::vector<std::string> pieces{"a", "b", "\xc3\xa9"};
    check_subsets(all_texts(pieces, 2), all_texts(pieces, 3));

    const std::vector<std::string> bare = all_texts({"a", "b"}, 8);
    const std::vector<std::string> bare_queries = all_texts({"a", "b"}, 9);
    check_text_builds(bare, bare_queries);
    const std::string prefix(70, 'p');
    std::vector<std::string> prefixed;
    for (const std::string& text : bare) prefixed.push_back(prefix + text);
    std::vector<std::string> prefixed_queries = bare_queries;
    for (const std::string& text : bare_queries) prefixed_queries.push_back(prefix + text);
    prefixed_queries.push_back(std::string(69, 'p'));
    prefixed_queries.push_back(std::string(71, 'p'));
    std::sort(prefixed_queries.begin(), prefixed_queries.end());
    check_text_builds(prefixed, prefixed_queries);

    const std::vector<std::string> edges{std::string(1, '\0'), "a", "\x7f", "\xc3\xa9", "\xf4\x8f\xbf\xbf"};
    const std::vector<std::string> edge_texts = all_texts(edges, 2);
    const std::vector<std::string> edge_queries = all_texts(edges, 3);
    check_text_builds(edge_texts, edge_queries);
    check_subsets(std::vector<std::string>(edge_texts.begin(), edge_texts.begin() + 12), edge_queries);
}

// ---- refusals -------------------------------------------------------------------

void refusals() {
    const u64 unsorted[] = {1, 3, 2};
    const u64 repeated[] = {1, 2, 2};
    const u64 values[] = {7, 8, 9};
    stxlib::Result<stxlib::FrozenMap> out_of_order =
        stxlib::FrozenMap::build(stxlib::View<u64>(unsorted), stxlib::View<u64>(values));
    CHECK(out_of_order.failed() && (*out_of_order.error()).kind() == stxlib::ErrorKind::invalid_argument &&
          (*out_of_order.error()).code() == 2);
    CHECK(stxlib::FrozenMap::build(stxlib::View<u64>(repeated), stxlib::View<u64>(values)).failed());
    CHECK(stxlib::FrozenMap::build(stxlib::View<u64>(values), stxlib::View<u64>(values, 2)).failed());
    const u64 key_weights[] = {1, 1, 1};
    CHECK(stxlib::FrozenMap::build_weighted(stxlib::View<u64>(values), stxlib::View<u64>(values),
                                            stxlib::View<u64>(key_weights), stxlib::View<u64>(key_weights))
              .failed());

    stxlib::detail::arm_vocabulary_refusals(0);
    stxlib::Result<stxlib::FrozenMap> refused =
        stxlib::FrozenMap::build(stxlib::View<u64>(values), stxlib::View<u64>(values));
    stxlib::detail::disarm_vocabulary_refusals();
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::out_of_memory);

    const stxlib::TextView texts[] = {"b", "a"};
    stxlib::Result<stxlib::FrozenTextMap> texts_out_of_order =
        stxlib::FrozenTextMap::build(stxlib::View<stxlib::TextView>(texts), stxlib::View<u64>(values, 2));
    CHECK(texts_out_of_order.failed() && (*texts_out_of_order.error()).code() == 1);

    // Moves: the source is left empty and still usable.
    stxlib::Result<stxlib::FrozenMap> built =
        stxlib::FrozenMap::build(stxlib::View<u64>(values), stxlib::View<u64>(values));
    CHECK(built.succeeded());
    stxlib::FrozenMap map = static_cast<stxlib::Result<stxlib::FrozenMap>&&>(built).take_value();
    stxlib::FrozenMap moved = static_cast<stxlib::FrozenMap&&>(map);
    CHECK(map.empty() && map.find(8) == nullptr && map.lower_bound(8) == 0);
    CHECK(moved.count() == 3 && moved.find(8) != nullptr && *moved.find(8) == 8);
    stxlib::FrozenMap empty;
    CHECK(empty.find(0) == nullptr && empty.lower_bound(0) == 0);
}

} // namespace

int main() {
    number_subsets_exhaustive();
    number_families();
    text_exhaustive();
    refusals();
    return stxlib_tests::finish("frozen_map");
}
