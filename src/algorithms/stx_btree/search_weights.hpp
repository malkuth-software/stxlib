#pragma once

// Search weights and binary search tree shapes, for the optimal tree
// constructions (optimal_tree.hpp) and the static map built on them
// (optimal_map.hpp).
//
// The conventions are the manuscript's (falseywinchnet/papers_please,
// obst/manuscript): n keys 0..n-1 found with weights p_i, and n + 1 gaps
// 0..n searched without success with weights q_j, where gap j lies between
// key j - 1 and key j. A key at depth d (the root has depth 0) costs p (d + 1)
// and a gap whose external position hangs below d keys costs q d: the cost of
// a tree is its number of three-way comparisons, weighted. The interval
// (i, j), i <= j, holds keys i..j-1 and gaps i..j, so its mass is
// W(i, j) = q_i + sum over i <= k < j of (p_k + q_{k+1}).
//
// Costs are exact integers. ExactCost is a signed 128-bit integer, which
// GCC and Clang provide as an extension; the constructions check that their
// largest intermediate value fits before they start.
//
// STXlib adaptation (from stx_auxlib b6e330a, btree/include/stx_btree/
// search_weights.hpp; private to STXlib, namespace stxlib::stx_btree):
// - the weights are borrowed views (View), the caller's storage;
// - a shape's children are STXlib storage (detail::Array) in place of
//   std::vector;
// - validation returns a Status (invalid_argument) where stx_auxlib threw
//   std::invalid_argument;
// - the checks that need working storage return a Result, out_of_memory
//   when it cannot be had, and from_parents returns false then.
// The rules themselves are stx_auxlib's.

#include "algorithms/stx_btree/detail/array.hpp"
#include "stxlib/error.hpp"
#include "stxlib/result.hpp"
#include "stxlib/view.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace stxlib::stx_btree {

__extension__ typedef __int128 ExactCost;

using SearchWeight = std::uint64_t;

// A key's position in sorted order. Signed, so that no_key can mark an
// absent child.
using KeyIndex = std::int32_t;

inline constexpr KeyIndex no_key = -1;

inline constexpr std::size_t maximum_search_keys =
    static_cast<std::size_t>(std::numeric_limits<KeyIndex>::max()) - 1;

// Borrowed for the call that takes them.
struct SearchWeights final {
    View<SearchWeight> keys; // p, one per key
    View<SearchWeight> gaps; // q, one more than keys
};

// A binary search tree over keys 0..n-1 whose in-order is 0..n-1. The root
// is no_key exactly when n = 0.
struct BinaryTreeShape final {
    KeyIndex root = no_key;
    detail::Array<KeyIndex> left;  // n entries, a key or no_key
    detail::Array<KeyIndex> right; // n entries, a key or no_key
};

struct SearchTree final {
    ExactCost cost = 0;
    BinaryTreeShape shape;
};

class SearchWeightRules final {
public:
    // invalid_argument unless there is exactly one more gap than keys and
    // the key count fits KeyIndex.
    [[nodiscard]] static Status validate(const SearchWeights& weights) noexcept {
        if (weights.keys.count() > maximum_search_keys) {
            const ErrorRecord refused = ErrorRecord::make(
                ErrorKind::invalid_argument, "stxlib.stx_btree.search_weights",
                ErrorDetail{.message = "too many keys for a search tree"});
            return Status::failure(refused);
        }
        if (weights.gaps.count() != weights.keys.count() + 1) {
            const ErrorRecord refused = ErrorRecord::make(
                ErrorKind::invalid_argument, "stxlib.stx_btree.search_weights",
                ErrorDetail{.message = "search weights need one more gap weight than key weights"});
            return Status::failure(refused);
        }
        return Status::success();
    }

    [[nodiscard]] static ExactCost total_mass(const SearchWeights& weights) noexcept {
        ExactCost total = 0;
        std::size_t index = 0;
        for (index = 0; index < weights.keys.count(); index += 1) {
            total += static_cast<ExactCost>(weights.keys[index]);
        }
        for (index = 0; index < weights.gaps.count(); index += 1) {
            total += static_cast<ExactCost>(weights.gaps[index]);
        }
        return total;
    }

    [[nodiscard]] static bool all_keys_positive(const SearchWeights& weights) noexcept {
        std::size_t index = 0;
        for (index = 0; index < weights.keys.count(); index += 1) {
            if (weights.keys[index] == 0) {
                return false;
            }
        }
        return true;
    }
};

// Evaluation and checking of shapes, independent of how they were built.
class BinaryTreeRules final {
public:
    // Whether `shape` is a binary search tree over keys 0..n-1: every key
    // reached exactly once from the root, and in-order 0..n-1.
    // out_of_memory when the walk's stack cannot be had.
    [[nodiscard]] static Result<bool> is_search_tree(const BinaryTreeShape& shape, std::size_t key_count) noexcept {
        if (shape.left.size() != key_count || shape.right.size() != key_count) {
            return Result<bool>::success(false);
        }
        if (key_count == 0) {
            return Result<bool>::success(shape.root == no_key);
        }
        detail::Array<KeyIndex> pending;
        if (!pending.reserve(key_count)) {
            return Result<bool>::failure(exhausted());
        }
        KeyIndex expected = 0;
        KeyIndex current = shape.root;
        std::size_t steps = 0;
        while (current != no_key || !pending.empty()) {
            while (current != no_key) {
                if (current < 0 || static_cast<std::size_t>(current) >= key_count || steps > key_count) {
                    return Result<bool>::success(false);
                }
                steps += 1;
                // Room reserved: at most key_count keys are pending.
                if (!pending.push_back(current)) {
                    return Result<bool>::failure(exhausted());
                }
                current = shape.left[static_cast<std::size_t>(current)];
            }
            const KeyIndex visited = pending.back();
            pending.pop_back();
            if (visited != expected) {
                return Result<bool>::success(false);
            }
            expected += 1;
            current = shape.right[static_cast<std::size_t>(visited)];
        }
        return Result<bool>::success(static_cast<std::size_t>(expected) == key_count);
    }

    // The weighted comparison count of `shape` (see the head of this file).
    // The shape must satisfy is_search_tree. out_of_memory when the walk's
    // stack cannot be had.
    [[nodiscard]] static Result<ExactCost> cost(const BinaryTreeShape& shape, const SearchWeights& weights) noexcept {
        ExactCost total = 0;
        if (shape.root == no_key) {
            return Result<ExactCost>::success(total); // the one gap hangs below no key
        }
        detail::Array<Visit> pending;
        if (!pending.push_back(Visit{.key = shape.root,
                                     .low = 0,
                                     .high = static_cast<KeyIndex>(weights.keys.count()),
                                     .depth = 0})) {
            return Result<ExactCost>::failure(exhausted());
        }
        while (!pending.empty()) {
            const Visit visit = pending.back();
            pending.pop_back();
            const std::size_t key = static_cast<std::size_t>(visit.key);
            const ExactCost below = visit.depth + 1;
            total += static_cast<ExactCost>(weights.keys[key]) * below;
            const KeyIndex left_child = shape.left[key];
            const KeyIndex right_child = shape.right[key];
            if (left_child == no_key) {
                total += static_cast<ExactCost>(weights.gaps[key]) * below;
            } else if (!pending.push_back(Visit{.key = left_child, .low = visit.low, .high = visit.key, .depth = below})) {
                return Result<ExactCost>::failure(exhausted());
            }
            if (right_child == no_key) {
                total += static_cast<ExactCost>(weights.gaps[key + 1]) * below;
            } else if (!pending.push_back(Visit{.key = right_child, .low = visit.key + 1, .high = visit.high, .depth = below})) {
                return Result<ExactCost>::failure(exhausted());
            }
        }
        return Result<ExactCost>::success(total);
    }

    // Children from a parent array (no_key at the root), as the
    // reconstructions produce it. False, `shape` unusable, when its storage
    // cannot be had.
    [[nodiscard]] static bool from_parents(const detail::Array<KeyIndex>& parent, BinaryTreeShape& shape) noexcept {
        const std::size_t key_count = parent.size();
        shape.root = no_key;
        if (!shape.left.assign(key_count, no_key) || !shape.right.assign(key_count, no_key)) {
            return false;
        }
        std::size_t key = 0;
        for (key = 0; key < key_count; key += 1) {
            const KeyIndex up = parent[key];
            if (up == no_key) {
                shape.root = static_cast<KeyIndex>(key);
            } else if (static_cast<std::size_t>(up) > key) {
                shape.left[static_cast<std::size_t>(up)] = static_cast<KeyIndex>(key);
            } else {
                shape.right[static_cast<std::size_t>(up)] = static_cast<KeyIndex>(key);
            }
        }
        return true;
    }

    // The record for storage that cannot be had.
    [[nodiscard]] static ErrorRecord exhausted() noexcept {
        const ErrorRecord result = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.stx_btree.search_tree");
        return result;
    }

private:
    struct Visit final {
        KeyIndex key;
        KeyIndex low;  // first key of its subtree
        KeyIndex high; // one past its last key
        ExactCost depth;
    };
};

} // namespace stxlib::stx_btree
