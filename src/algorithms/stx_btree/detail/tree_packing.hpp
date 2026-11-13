#pragma once

// Packing a binary search tree into nodes of at most `capacity` keys, each
// node a connected piece of the tree, so that the weighted number of nodes a
// search visits is least (after Gil and Itai, "How to pack trees", J.
// Algorithms 1999, specialized to search weights). A connected piece of k
// keys has exactly k + 1 exits, so every node is a multiway search node:
// its keys sorted, its exits between them leading to child nodes or to a
// miss. The static map searches such a node by counting, as bbtree does
// (optimal_map.hpp).
//
// The cost of a packing is the sum over nodes of the mass (search_weights.hpp)
// of the subtree below the node's top key: every search that passes through
// a node pays one visit there. Among packings of the given tree this is
// least; it is not a claim about multiway trees not obtained from it.
//
// Dynamic program over the tree, children before parents. For key v with
// subtree mass W(v), let F(v, k), 1 <= k <= min(capacity, size(v)), be the
// least cost of v's subtree when v's node holds exactly k of its keys:
//     F(v, k) = W(v) + min over a + b = k - 1 of G(left, a) + G(right, b),
//     G(c, 0) = min over k of F(c, k)   (c starts its own node; 0 if absent),
//     G(c, a) = F(c, a) - W(c)          (c shares v's node; its visit is v's).
// Time is the sum over keys of min(capacity, size(left) + 1) times
// min(capacity, size(right) + 1), at most n capacity^2 and much less for
// balanced trees; storage is one F and one choice per (v, k).
//
// STXlib adaptation (from stx_auxlib b6e330a, btree/include/stx_btree/
// detail/tree_packing.hpp; private to STXlib, namespace
// stxlib::stx_btree::detail): STXlib storage (detail::Array) in place of
// std::vector; pack and relayout return false, with the output unusable,
// when working storage cannot be had (where std::vector threw
// std::bad_alloc); the assertions are STXLIB_REQUIRE. The packing and the
// layout are stx_auxlib's.

#include "algorithms/stx_btree/detail/array.hpp"
#include "algorithms/stx_btree/search_weights.hpp"
#include "stxlib/require.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace stxlib::stx_btree::detail {

// Nodes in breadth-first order: node 0 is the root, and the children of
// each node are consecutive, in exit order, so that exit e of node x leads
// to node first_child[x] + (the number of child exits before e).
struct PackedTree final {
    Array<KeyIndex> keys;            // the keys of node 0, then node 1, ..., each node's ascending
    Array<std::uint32_t> key_begin;  // node x holds keys[key_begin[x], key_begin[x + 1])
    Array<std::uint64_t> child_exits; // bit e set when exit e of the node leads to a child node
    Array<std::uint32_t> first_child;
    ExactCost cost = 0;                    // weighted node visits

    [[nodiscard]] std::size_t node_count() const noexcept {
        return first_child.size();
    }
};

class TreePacker final {
public:
    // Exits per node must fit a 64-bit exit mask.
    static constexpr std::size_t maximum_capacity = 63;

    // Packs into `packed` (empty on entry). False, `packed` unusable, when
    // working storage cannot be had.
    [[nodiscard]] static bool pack(
        const BinaryTreeShape& shape, const SearchWeights& weights, std::size_t capacity, PackedTree& packed) noexcept {
        STXLIB_REQUIRE(capacity >= 1 && capacity <= maximum_capacity, "TreePacker::pack: capacity out of range");
        const std::size_t key_count = weights.keys.count();
        if (key_count == 0) {
            return true;
        }
        Array<KeyIndex> order;
        Array<std::uint32_t> size;
        Array<ExactCost> mass;
        if (!post_order(shape, order) || !size.assign(key_count, 0) || !mass.assign(key_count, 0)) {
            return false;
        }

        // Subtree sizes and masses, children first.
        std::size_t step = 0;
        for (step = 0; step < order.size(); step += 1) {
            const std::size_t key = static_cast<std::size_t>(order[step]);
            const KeyIndex left_child = shape.left[key];
            const KeyIndex right_child = shape.right[key];
            std::uint32_t keys_below = 1;
            ExactCost mass_below = static_cast<ExactCost>(weights.keys[key]);
            if (left_child == no_key) {
                mass_below += static_cast<ExactCost>(weights.gaps[key]);
            } else {
                keys_below += size[static_cast<std::size_t>(left_child)];
                mass_below += mass[static_cast<std::size_t>(left_child)];
            }
            if (right_child == no_key) {
                mass_below += static_cast<ExactCost>(weights.gaps[key + 1]);
            } else {
                keys_below += size[static_cast<std::size_t>(right_child)];
                mass_below += mass[static_cast<std::size_t>(right_child)];
            }
            size[key] = keys_below;
            mass[key] = mass_below;
        }

        // Each key's table holds F(v, 1..width(v)), width(v) = min(capacity, size(v)).
        Array<std::size_t> table_begin;
        if (!table_begin.assign(key_count + 1, 0)) {
            return false;
        }
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            table_begin[key + 1] = table_begin[key] + width_of(size[key], capacity);
        }
        Array<ExactCost> best_with;      // F(v, k) at table_begin[v] + k - 1
        Array<std::uint8_t> left_share;  // the a that achieves it
        Array<ExactCost> best_alone;     // min over k of F(v, k)
        Array<std::uint8_t> best_width;  // the k that achieves it
        if (!best_with.assign(table_begin[key_count], 0) || !left_share.assign(table_begin[key_count], 0) ||
            !best_alone.assign(key_count, 0) || !best_width.assign(key_count, 0)) {
            return false;
        }

        for (step = 0; step < order.size(); step += 1) {
            key = static_cast<std::size_t>(order[step]);
            const KeyIndex left_child = shape.left[key];
            const KeyIndex right_child = shape.right[key];
            const std::size_t left_width = left_child == no_key ? 0 : width_of(size[static_cast<std::size_t>(left_child)], capacity);
            const std::size_t right_width = right_child == no_key ? 0 : width_of(size[static_cast<std::size_t>(right_child)], capacity);
            const std::size_t width = width_of(size[key], capacity);
            std::size_t keys_here = 0;
            for (keys_here = 1; keys_here <= width; keys_here += 1) {
                const std::size_t shared = keys_here - 1;
                bool have_best = false;
                ExactCost best = 0;
                std::size_t best_left = 0;
                std::size_t left_keys = 0;
                for (left_keys = 0; left_keys <= shared && left_keys <= left_width; left_keys += 1) {
                    const std::size_t right_keys = shared - left_keys;
                    if (right_keys > right_width) {
                        continue;
                    }
                    const ExactCost left_cost = share_cost(left_child, left_keys, mass, table_begin, best_with, best_alone);
                    const ExactCost right_cost = share_cost(right_child, right_keys, mass, table_begin, best_with, best_alone);
                    const ExactCost value = left_cost + right_cost;
                    if (!have_best || value < best) {
                        best = value;
                        best_left = left_keys;
                        have_best = true;
                    }
                }
                STXLIB_REQUIRE(have_best, "TreePacker::pack: every width has a split");
                const std::size_t cell = table_begin[key] + keys_here - 1;
                best_with[cell] = mass[key] + best;
                left_share[cell] = static_cast<std::uint8_t>(best_left);
                if (keys_here == 1 || best_with[cell] < best_alone[key]) {
                    best_alone[key] = best_with[cell];
                    best_width[key] = static_cast<std::uint8_t>(keys_here);
                }
            }
        }
        packed.cost = best_alone[static_cast<std::size_t>(shape.root)];
        return emit_nodes(shape, table_begin, left_share, best_width, packed);
    }

private:
    [[nodiscard]] static std::size_t width_of(std::uint32_t subtree_size, std::size_t capacity) noexcept {
        const std::size_t size = static_cast<std::size_t>(subtree_size);
        return size < capacity ? size : capacity;
    }

    // G(child, keys): the child's cost when `keys` of its keys share the
    // parent's node (0: it starts its own node, or is absent).
    [[nodiscard]] static ExactCost share_cost(
        KeyIndex child, std::size_t keys, const Array<ExactCost>& mass,
        const Array<std::size_t>& table_begin, const Array<ExactCost>& best_with,
        const Array<ExactCost>& best_alone) noexcept {
        if (child == no_key) {
            return 0;
        }
        const std::size_t at = static_cast<std::size_t>(child);
        if (keys == 0) {
            return best_alone[at];
        }
        return best_with[table_begin[at] + keys - 1] - mass[at];
    }

    // The keys children first into `order` (empty on entry); false when
    // storage cannot be had.
    [[nodiscard]] static bool post_order(const BinaryTreeShape& shape, Array<KeyIndex>& order) noexcept {
        Array<KeyIndex> pending;
        if (!order.reserve(shape.left.size()) || !pending.push_back(shape.root)) {
            return false;
        }
        // Root, right, left reversed is left, right, root.
        while (!pending.empty()) {
            const KeyIndex key = pending.back();
            pending.pop_back();
            if (!order.push_back(key)) {
                return false;
            }
            const std::size_t at = static_cast<std::size_t>(key);
            if (shape.left[at] != no_key && !pending.push_back(shape.left[at])) {
                return false;
            }
            if (shape.right[at] != no_key && !pending.push_back(shape.right[at])) {
                return false;
            }
        }
        std::size_t low = 0;
        std::size_t high = order.size();
        while (low + 1 < high) {
            high -= 1;
            const KeyIndex swapped = order[low];
            order[low] = order[high];
            order[high] = swapped;
            low += 1;
        }
        return true;
    }

    // A node still to emit: its top key and how many keys it holds.
    struct PendingNode final {
        KeyIndex top;
        std::size_t keys;
    };

    // One step of the in-order walk inside a node: a key to visit with the
    // number of keys of its subtree that share the node, or (expanded) a
    // key whose left part is done.
    struct WalkStep final {
        KeyIndex key;
        std::size_t keys;
        bool expanded;
    };

    // Breadth-first over nodes; inside a node an in-order walk over the keys
    // it holds, emitting keys and exits in order and queueing child nodes.
    // False when storage cannot be had.
    [[nodiscard]] static bool emit_nodes(
        const BinaryTreeShape& shape, const Array<std::size_t>& table_begin,
        const Array<std::uint8_t>& left_share, const Array<std::uint8_t>& best_width,
        PackedTree& packed) noexcept {
        Array<PendingNode> queue;
        const std::size_t root = static_cast<std::size_t>(shape.root);
        Array<WalkStep> walk;
        if (!queue.push_back(PendingNode{.top = shape.root, .keys = best_width[root]}) || !packed.key_begin.push_back(0)) {
            return false;
        }
        std::size_t head = 0;
        while (head < queue.size()) {
            const PendingNode node = queue[head];
            head += 1;
            std::uint64_t child_exits = 0;
            std::size_t exit = 0;
            walk.clear();
            if (!packed.first_child.push_back(static_cast<std::uint32_t>(queue.size())) ||
                !walk.push_back(WalkStep{.key = node.top, .keys = node.keys, .expanded = false})) {
                return false;
            }
            while (!walk.empty()) {
                const WalkStep current = walk.back();
                walk.pop_back();
                const std::size_t at = static_cast<std::size_t>(current.key);
                const std::size_t left_keys = left_share[table_begin[at] + current.keys - 1];
                const std::size_t right_keys = current.keys - 1 - left_keys;
                if (!current.expanded) {
                    if (!walk.push_back(WalkStep{.key = current.key, .keys = current.keys, .expanded = true})) {
                        return false;
                    }
                    const KeyIndex left_child = shape.left[at];
                    if (left_keys > 0) {
                        if (!walk.push_back(WalkStep{.key = left_child, .keys = left_keys, .expanded = false})) {
                            return false;
                        }
                    } else {
                        if (left_child != no_key) {
                            const std::size_t child = static_cast<std::size_t>(left_child);
                            if (!queue.push_back(PendingNode{.top = left_child, .keys = best_width[child]})) {
                                return false;
                            }
                            child_exits |= static_cast<std::uint64_t>(1) << exit;
                        }
                        exit += 1;
                    }
                    continue;
                }
                if (!packed.keys.push_back(current.key)) {
                    return false;
                }
                const KeyIndex right_child = shape.right[at];
                if (right_keys > 0) {
                    if (!walk.push_back(WalkStep{.key = right_child, .keys = right_keys, .expanded = false})) {
                        return false;
                    }
                } else {
                    if (right_child != no_key) {
                        const std::size_t child = static_cast<std::size_t>(right_child);
                        if (!queue.push_back(PendingNode{.top = right_child, .keys = best_width[child]})) {
                            return false;
                        }
                        child_exits |= static_cast<std::uint64_t>(1) << exit;
                    }
                    exit += 1;
                }
            }
            STXLIB_REQUIRE(exit == node.keys + 1, "TreePacker: a node of k keys has k + 1 exits");
            if (!packed.child_exits.push_back(child_exits) ||
                !packed.key_begin.push_back(static_cast<std::uint32_t>(packed.keys.size()))) {
                return false;
            }
        }
        return true;
    }
};

// Packing a fixed binary tree leaves many part-filled nodes: a connected
// piece of the tree cannot take keys from beside it, so a subtree of 60 keys
// becomes a node of 48 and pieces of the rest, and even a balanced tree fills
// 48-key nodes about half. Where the weights are flat that costs memory and
// a level without saving visits. This re-lays such subtrees as full
// count-balanced multiway nodes (a B-tree's bulk load):
//
// Count layout: an interval of c keys takes all of them if c <= capacity,
// otherwise the fewest evenly spaced separators that leave every child
// within one level less than the interval needs.
//
// Top-down from the root, a packed node's subtree is laid out by count when
// that layout's weighted visits are at most (100 + slack_percent) / 100 of
// the packing's in the same subtree; below such a node everything is laid
// out by count, elsewhere the packing's keys stay. With slack 0 the result
// never costs more visits than the packing. The result's cost is recomputed.
class ColdSubtreeLayout final {
public:
    // Lays out into `laid` (empty on entry). False, `laid` unusable, when
    // working storage cannot be had.
    [[nodiscard]] static bool relayout(
        const PackedTree& packed, const SearchWeights& weights, std::size_t capacity, std::uint64_t slack_percent,
        PackedTree& laid) noexcept {
        STXLIB_REQUIRE(capacity >= 1 && capacity <= TreePacker::maximum_capacity, "ColdSubtreeLayout: capacity out of range");
        const std::size_t key_count = weights.keys.count();
        if (key_count == 0) {
            return true;
        }
        Array<ExactCost> prefix; // sum over t < k of p_t + q_{t+1}
        if (!prefix.assign(key_count + 1, 0)) {
            return false;
        }
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            prefix[key + 1] = prefix[key] + static_cast<ExactCost>(weights.keys[key]) +
                              static_cast<ExactCost>(weights.gaps[key + 1]);
        }

        // Each packed node's key interval, parents first, then the visits of
        // its packed subtree, children first.
        const std::size_t packed_count = packed.node_count();
        Array<std::size_t> interval_first;
        Array<std::size_t> interval_end;
        Array<ExactCost> packed_visits;
        if (!interval_first.assign(packed_count, 0) || !interval_end.assign(packed_count, 0) ||
            !packed_visits.assign(packed_count, 0)) {
            return false;
        }
        interval_end[0] = key_count;
        std::size_t node = 0;
        for (node = 0; node < packed_count; node += 1) {
            const std::size_t first = packed.key_begin[node];
            const std::size_t length = packed.key_begin[node + 1] - first;
            std::size_t child = packed.first_child[node];
            std::size_t exit = 0;
            for (exit = 0; exit <= length; exit += 1) {
                if (((packed.child_exits[node] >> exit) & 1) == 0) {
                    continue;
                }
                interval_first[child] = exit > 0 ? static_cast<std::size_t>(packed.keys[first + exit - 1]) + 1 : interval_first[node];
                interval_end[child] = exit < length ? static_cast<std::size_t>(packed.keys[first + exit]) : interval_end[node];
                child += 1;
            }
        }
        node = packed_count;
        while (node > 0) {
            node -= 1;
            ExactCost visits = interval_mass(prefix, weights, interval_first[node], interval_end[node]);
            const std::size_t children = static_cast<std::size_t>(std::popcount(packed.child_exits[node]));
            std::size_t child = 0;
            for (child = 0; child < children; child += 1) {
                visits += packed_visits[packed.first_child[node] + child];
            }
            packed_visits[node] = visits;
        }

        Array<PendingInterval> queue;
        Array<KeyIndex> node_keys;
        Array<KeyIndex> scratch;
        Array<KeyInterval> pending;
        if (!queue.push_back(PendingInterval{.first = 0, .end = key_count, .packed_node = 0}) ||
            !laid.key_begin.push_back(0)) {
            return false;
        }
        std::size_t head = 0;
        while (head < queue.size()) {
            const PendingInterval interval = queue[head];
            head += 1;
            laid.cost += interval_mass(prefix, weights, interval.first, interval.end);
            bool by_count = interval.packed_node == no_packed_node;
            if (!by_count) {
                ExactCost count_visits = 0;
                if (!count_layout_visits(prefix, weights, interval.first, interval.end, capacity, scratch, pending,
                                         &count_visits)) {
                    return false;
                }
                const ExactCost allowed = packed_visits[interval.packed_node] * static_cast<ExactCost>(100 + slack_percent);
                by_count = count_visits * 100 <= allowed;
            }
            node_keys.clear();
            if (by_count) {
                if (!count_separators(interval.first, interval.end, capacity, node_keys)) {
                    return false;
                }
            } else {
                const std::size_t first = packed.key_begin[interval.packed_node];
                const std::size_t end = packed.key_begin[interval.packed_node + 1];
                if (!node_keys.append(packed.keys.data() + first, end - first)) {
                    return false;
                }
            }
            if (!laid.first_child.push_back(static_cast<std::uint32_t>(queue.size()))) {
                return false;
            }
            std::uint64_t child_exits = 0;
            std::size_t packed_child = by_count ? 0 : packed.first_child[interval.packed_node];
            const std::uint64_t packed_exits = by_count ? 0 : packed.child_exits[interval.packed_node];
            std::size_t exit = 0;
            for (exit = 0; exit <= node_keys.size(); exit += 1) {
                const std::size_t child_first =
                    exit > 0 ? static_cast<std::size_t>(node_keys[exit - 1]) + 1 : interval.first;
                const std::size_t child_end =
                    exit < node_keys.size() ? static_cast<std::size_t>(node_keys[exit]) : interval.end;
                std::size_t child_node = no_packed_node;
                if (!by_count && ((packed_exits >> exit) & 1) != 0) {
                    child_node = packed_child;
                    packed_child += 1;
                }
                if (child_first == child_end) {
                    continue;
                }
                STXLIB_REQUIRE(by_count || child_node != no_packed_node, "ColdSubtreeLayout: a packed exit has its child");
                if (!queue.push_back(PendingInterval{.first = child_first, .end = child_end, .packed_node = child_node})) {
                    return false;
                }
                child_exits |= static_cast<std::uint64_t>(1) << exit;
            }
            if (!laid.keys.append(node_keys.data(), node_keys.size()) || !laid.child_exits.push_back(child_exits) ||
                !laid.key_begin.push_back(static_cast<std::uint32_t>(laid.keys.size()))) {
                return false;
            }
        }
        return true;
    }

private:
    static constexpr std::size_t no_packed_node = static_cast<std::size_t>(-1);

    struct PendingInterval final {
        std::size_t first;
        std::size_t end;
        std::size_t packed_node; // the packing's node for this interval, or none below a count layout
    };

    [[nodiscard]] static ExactCost interval_mass(
        const Array<ExactCost>& prefix, const SearchWeights& weights, std::size_t first, std::size_t end) noexcept {
        return prefix[end] - prefix[first] + static_cast<ExactCost>(weights.gaps[first]);
    }

    struct KeyInterval final {
        std::size_t first;
        std::size_t end;
    };

    // Weighted visits of the count layout of keys [first, end), without
    // building it, into *visits_out. `scratch` and `pending` are reused
    // storage. False when storage cannot be had.
    [[nodiscard]] static bool count_layout_visits(
        const Array<ExactCost>& prefix, const SearchWeights& weights, std::size_t first, std::size_t end,
        std::size_t capacity, Array<KeyIndex>& scratch, Array<KeyInterval>& pending, ExactCost* visits_out) noexcept {
        ExactCost visits = 0;
        pending.clear();
        if (!pending.push_back(KeyInterval{.first = first, .end = end})) {
            return false;
        }
        while (!pending.empty()) {
            const KeyInterval interval = pending.back();
            pending.pop_back();
            visits += interval_mass(prefix, weights, interval.first, interval.end);
            if (interval.end - interval.first <= capacity) {
                continue; // a leaf: its exits are misses
            }
            scratch.clear();
            if (!count_separators(interval.first, interval.end, capacity, scratch)) {
                return false;
            }
            std::size_t child_first = interval.first;
            std::size_t index = 0;
            for (index = 0; index <= scratch.size(); index += 1) {
                const std::size_t child_end = index < scratch.size() ? static_cast<std::size_t>(scratch[index]) : interval.end;
                if (child_end > child_first && !pending.push_back(KeyInterval{.first = child_first, .end = child_end})) {
                    return false;
                }
                child_first = child_end + 1;
            }
        }
        *visits_out = visits;
        return true;
    }

    // Separators for keys [first, end) laid out by count (see the class
    // comment), ascending, appended to `separators`. False when storage
    // cannot be had.
    [[nodiscard]] static bool count_separators(std::size_t first, std::size_t end, std::size_t capacity,
                                               Array<KeyIndex>& separators) noexcept {
        const std::size_t count = end - first;
        std::size_t key = 0;
        if (count <= capacity) {
            for (key = first; key < end; key += 1) {
                if (!separators.push_back(static_cast<KeyIndex>(key))) {
                    return false;
                }
            }
            return true;
        }
        // child_capacity: the keys a subtree one level shorter holds,
        // (capacity + 1)^(levels - 1) - 1 for the fewest levels holding count.
        std::size_t child_capacity = capacity;
        while ((child_capacity + 1) * (capacity + 1) - 1 < count) {
            child_capacity = (child_capacity + 1) * (capacity + 1) - 1;
        }
        // m separators leave count - m keys in m + 1 children of at most
        // child_capacity: m >= (count - child_capacity) / (child_capacity + 1),
        // whose ceiling is floor(count / (child_capacity + 1)).
        const std::size_t separator_count = count / (child_capacity + 1);
        const std::size_t child_keys = count - separator_count;
        const std::size_t children = separator_count + 1;
        std::size_t position = first;
        std::size_t child = 0;
        for (child = 0; child < separator_count; child += 1) {
            const std::size_t share = child_keys / children + (child < child_keys % children ? 1 : 0);
            position += share;
            if (!separators.push_back(static_cast<KeyIndex>(position))) {
                return false;
            }
            position += 1;
        }
        return true;
    }
};

} // namespace stxlib::stx_btree::detail
