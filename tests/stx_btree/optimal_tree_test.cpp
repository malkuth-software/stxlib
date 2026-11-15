// The optimal tree constructions against two independent references: the
// plain cubic recurrence, and for small n every binary search tree
// enumerated and costed directly. Each returned tree is checked to be a
// search tree whose own cost equals the reported cost.
//
// Ported from stx_auxlib's btree/tests/optimal_tree_test.cpp at b6e330a.
// The references and instances are unchanged. Adapted to STXlib's
// interface: the test keeps its weights in std::vector (OwnedWeights) and
// passes views; each construction's Result must succeed (take); a refusal
// is a Result with kind invalid_argument where stx_auxlib threw
// std::invalid_argument; shapes are compared element by element.

#include "algorithms/stx_btree/optimal_tree.hpp"
#include "btree_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace stx_btree = stxlib::stx_btree;

namespace {

using stx_btree::BinaryTreeRules;
using stx_btree::BinaryTreeShape;
using stx_btree::ExactCost;
using stx_btree::KeyIndex;
using stx_btree::OptimalTreeBuilder;
using stx_btree::SearchTree;
using stx_btree::SearchWeight;
using stx_btree::SearchWeights;
using stx_btree::no_key;
using stx_btree_tests::SplitMix64;
using stx_btree_tests::TestFailure;

// The weights in the test's own storage; constructions take views.
struct OwnedWeights final {
    std::vector<SearchWeight> keys;
    std::vector<SearchWeight> gaps;

    [[nodiscard]] SearchWeights view() const noexcept {
        return SearchWeights{.keys = stxlib::View<SearchWeight>(keys.data(), keys.size()),
                             .gaps = stxlib::View<SearchWeight>(gaps.data(), gaps.size())};
    }
};

// A construction that must succeed.
SearchTree take(stxlib::Result<SearchTree> built) {
    TestFailure::expect(built.succeeded(), "construction", "a construction failed");
    if (!built.succeeded()) {
        return SearchTree{};
    }
    return std::move(built).take_value();
}

// A construction that must refuse its weights.
[[nodiscard]] bool refused(const stxlib::Result<SearchTree>& built) {
    return !built.succeeded() && (*built.error()).kind() == stxlib::ErrorKind::invalid_argument;
}

[[nodiscard]] bool search_tree(const BinaryTreeShape& shape, std::size_t key_count) {
    const stxlib::Result<bool> checked = BinaryTreeRules::is_search_tree(shape, key_count);
    TestFailure::expect(checked.succeeded(), "shape check", "memory sufficed");
    return checked.succeeded() && *checked.value();
}

[[nodiscard]] ExactCost cost_of(const BinaryTreeShape& shape, const OwnedWeights& weights) {
    const stxlib::Result<ExactCost> cost = BinaryTreeRules::cost(shape, weights.view());
    TestFailure::expect(cost.succeeded(), "cost", "memory sufficed");
    return cost.succeeded() ? *cost.value() : ExactCost{-1};
}

void copy_into(stx_btree::detail::Array<KeyIndex>& target, const std::vector<KeyIndex>& source) {
    TestFailure::expect(target.assign(source.size(), no_key), "copy", "memory sufficed");
    std::size_t index = 0;
    for (index = 0; index < source.size() && index < target.size(); index += 1) {
        target[index] = source[index];
    }
}

[[nodiscard]] bool same_array(const stx_btree::detail::Array<KeyIndex>& first, const stx_btree::detail::Array<KeyIndex>& second) {
    if (first.size() != second.size()) {
        return false;
    }
    std::size_t index = 0;
    for (index = 0; index < first.size(); index += 1) {
        if (first[index] != second[index]) {
            return false;
        }
    }
    return true;
}

// C(i, j) = W(i, j) + min over i <= r < j of C(i, r) + C(r + 1, j), every
// root tried.
class CubicReference final {
public:
    [[nodiscard]] static ExactCost optimal_cost(const OwnedWeights& weights) {
        const std::size_t key_count = weights.keys.size();
        const std::size_t side = key_count + 1;
        std::vector<ExactCost> cost(side * side, 0);
        std::size_t length = 0;
        for (length = 1; length <= key_count; length += 1) {
            std::size_t first = 0;
            for (first = 0; first + length <= key_count; first += 1) {
                const std::size_t end = first + length;
                ExactCost mass = static_cast<ExactCost>(weights.gaps[first]);
                std::size_t key = 0;
                for (key = first; key < end; key += 1) {
                    mass += static_cast<ExactCost>(weights.keys[key]) + static_cast<ExactCost>(weights.gaps[key + 1]);
                }
                ExactCost best = 0;
                bool have_best = false;
                std::size_t root = 0;
                for (root = first; root < end; root += 1) {
                    const ExactCost value = cost[first * side + root] + cost[(root + 1) * side + end];
                    if (!have_best || value < best) {
                        best = value;
                        have_best = true;
                    }
                }
                cost[first * side + end] = best + mass;
            }
        }
        return cost[key_count];
    }
};

// Every binary search tree over keys [first, end), as child links.
class TreeEnumeration final {
public:
    struct Fragment final {
        KeyIndex root = no_key;
        std::vector<KeyIndex> left;  // indexed by key
        std::vector<KeyIndex> right;
    };

    // Recursion bounded by the interval's key count (house style §1; audit A,
    // T1): each level drops at least one key, and the caller enumerates at
    // most 8 keys.
    [[nodiscard]] static std::vector<Fragment> all_trees(KeyIndex first, KeyIndex end, std::size_t width) {
        std::vector<Fragment> trees;
        if (first == end) {
            Fragment empty;
            empty.left.assign(width, no_key);
            empty.right.assign(width, no_key);
            trees.push_back(empty);
            return trees;
        }
        KeyIndex root = 0;
        for (root = first; root < end; root += 1) {
            const std::vector<Fragment> lefts = all_trees(first, root, width);
            const std::vector<Fragment> rights = all_trees(root + 1, end, width);
            std::size_t a = 0;
            for (a = 0; a < lefts.size(); a += 1) {
                std::size_t b = 0;
                for (b = 0; b < rights.size(); b += 1) {
                    Fragment tree;
                    tree.root = root;
                    tree.left.assign(width, no_key);
                    tree.right.assign(width, no_key);
                    std::size_t key = 0;
                    for (key = 0; key < width; key += 1) {
                        if (lefts[a].left[key] != no_key || lefts[a].right[key] != no_key) {
                            tree.left[key] = lefts[a].left[key];
                            tree.right[key] = lefts[a].right[key];
                        }
                        if (rights[b].left[key] != no_key || rights[b].right[key] != no_key) {
                            tree.left[key] = rights[b].left[key];
                            tree.right[key] = rights[b].right[key];
                        }
                    }
                    const std::size_t at = static_cast<std::size_t>(root);
                    tree.left[at] = lefts[a].root;
                    tree.right[at] = rights[b].root;
                    trees.push_back(tree);
                }
            }
        }
        return trees;
    }

    [[nodiscard]] static ExactCost optimal_cost(const OwnedWeights& weights) {
        const KeyIndex key_count = static_cast<KeyIndex>(weights.keys.size());
        const std::vector<Fragment> trees = all_trees(0, key_count, weights.keys.size());
        ExactCost best = 0;
        std::size_t index = 0;
        for (index = 0; index < trees.size(); index += 1) {
            BinaryTreeShape shape;
            shape.root = trees[index].root;
            copy_into(shape.left, trees[index].left);
            copy_into(shape.right, trees[index].right);
            TestFailure::expect(search_tree(shape, weights.keys.size()), "enumeration", "shape");
            const ExactCost cost = cost_of(shape, weights);
            if (index == 0 || cost < best) {
                best = cost;
            }
        }
        return best;
    }
};

enum class WeightFamily : std::uint8_t {
    small_random,    // 0..4, zeros common
    keys_only,       // gaps all zero
    gaps_only,       // keys all zero
    large_random,    // up to 2^40
    uniform,
    zipf_like,       // key k weighs about 1000 / (rank + 1), shuffled
    odd_ramp,        // the manuscript's dense-core lower-bound family
    ruler,           // the manuscript's dense-support, linear family
};

class WeightMaker final {
public:
    [[nodiscard]] static OwnedWeights make(WeightFamily family, std::size_t key_count, SplitMix64& random) {
        OwnedWeights weights;
        weights.keys.assign(key_count, 0);
        weights.gaps.assign(key_count + 1, 0);
        std::size_t index = 0;
        switch (family) {
        case WeightFamily::small_random:
            for (index = 0; index < key_count; index += 1) {
                weights.keys[index] = random.below(5);
            }
            for (index = 0; index <= key_count; index += 1) {
                weights.gaps[index] = random.below(5);
            }
            break;
        case WeightFamily::keys_only:
            for (index = 0; index < key_count; index += 1) {
                weights.keys[index] = 1 + random.below(9);
            }
            break;
        case WeightFamily::gaps_only:
            for (index = 0; index <= key_count; index += 1) {
                weights.gaps[index] = 1 + random.below(9);
            }
            break;
        case WeightFamily::large_random:
            for (index = 0; index < key_count; index += 1) {
                weights.keys[index] = random.below(static_cast<SearchWeight>(1) << 40);
            }
            for (index = 0; index <= key_count; index += 1) {
                weights.gaps[index] = random.below(static_cast<SearchWeight>(1) << 40);
            }
            break;
        case WeightFamily::uniform:
            for (index = 0; index < key_count; index += 1) {
                weights.keys[index] = 1;
            }
            for (index = 0; index <= key_count; index += 1) {
                weights.gaps[index] = 1;
            }
            break;
        case WeightFamily::zipf_like: {
            std::vector<SearchWeight> ranked(key_count, 0);
            for (index = 0; index < key_count; index += 1) {
                ranked[index] = 1000 / (index + 1) + 1;
            }
            random.shuffle(ranked);
            weights.keys = ranked;
            for (index = 0; index <= key_count; index += 1) {
                weights.gaps[index] = random.below(2);
            }
            break;
        }
        case WeightFamily::odd_ramp:
            // p_i = 2 i + 1, gaps zero.
            for (index = 0; index < key_count; index += 1) {
                weights.keys[index] = 2 * index + 1;
            }
            break;
        case WeightFamily::ruler:
            // p_i = 1 + trailing zero bits of (i + 1), gaps zero.
            for (index = 0; index < key_count; index += 1) {
                SearchWeight value = 1;
                std::size_t rest = index + 1;
                while ((rest & 1) == 0) {
                    value += 1;
                    rest >>= 1;
                }
                weights.keys[index] = value;
            }
            break;
        }
        return weights;
    }
};

constexpr WeightFamily all_families[] = {
    WeightFamily::small_random, WeightFamily::keys_only, WeightFamily::gaps_only, WeightFamily::large_random,
    WeightFamily::uniform, WeightFamily::zipf_like, WeightFamily::odd_ramp, WeightFamily::ruler,
};

void expect_valid(const SearchTree& tree, const OwnedWeights& weights, const char* label) {
    TestFailure::expect(search_tree(tree.shape, weights.keys.size()), label, "not a search tree");
    const ExactCost own = cost_of(tree.shape, weights);
    TestFailure::expect(own == tree.cost, label, "reported cost differs from the tree's cost");
}

[[nodiscard]] bool same_shape(const BinaryTreeShape& first, const BinaryTreeShape& second) {
    return first.root == second.root && same_array(first.left, second.left) && same_array(first.right, second.right);
}

[[nodiscard]] bool all_keys_zero(const OwnedWeights& weights) {
    std::size_t key = 0;
    for (key = 0; key < weights.keys.size(); key += 1) {
        if (weights.keys[key] != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool constant_weights(const OwnedWeights& weights) {
    std::size_t index = 0;
    for (index = 1; index < weights.keys.size(); index += 1) {
        if (weights.keys[index] != weights.keys[0]) {
            return false;
        }
    }
    for (index = 1; index < weights.gaps.size(); index += 1) {
        if (weights.gaps[index] != weights.gaps[0]) {
            return false;
        }
    }
    return true;
}

// Every construction but Knuth's own on these weights,
// against the optimum: the scattered Knuth exact and with Knuth's tree,
// Garsia-Wachs exact on gap-only weights and refusing others, the balanced
// tree exact on constant weights, the approximations valid and no cheaper.
void expect_other_constructions(const OwnedWeights& weights, const SearchTree& by_knuth, ExactCost optimum, const char* label) {
    const SearchTree scattered = take(OptimalTreeBuilder::knuth_scattered(weights.view()));
    expect_valid(scattered, weights, "knuth_scattered");
    TestFailure::expect(scattered.cost == optimum, label, "knuth_scattered not optimal");
    TestFailure::expect(same_shape(scattered.shape, by_knuth.shape), label, "knuth_scattered tree differs from knuth's");
    if (all_keys_zero(weights)) {
        const SearchTree alphabetic = take(OptimalTreeBuilder::garsia_wachs(weights.view()));
        expect_valid(alphabetic, weights, "garsia_wachs");
        TestFailure::expect(alphabetic.cost == optimum, label, "garsia_wachs not optimal");
    } else {
        TestFailure::expect(refused(OptimalTreeBuilder::garsia_wachs(weights.view())), label,
                            "garsia_wachs accepted key weights");
    }
    const SearchTree balanced = take(OptimalTreeBuilder::balanced(weights.view()));
    expect_valid(balanced, weights, "balanced");
    TestFailure::expect(balanced.cost >= optimum, label, "balanced below the optimum");
    if (constant_weights(weights)) {
        TestFailure::expect(balanced.cost == optimum, label, "balanced not optimal on constant weights");
    }
    const SearchTree joined = take(OptimalTreeBuilder::bottom_up(weights.view()));
    expect_valid(joined, weights, "bottom_up");
    TestFailure::expect(joined.cost >= optimum, label, "bottom_up below the optimum");
    const SearchTree bisected = take(OptimalTreeBuilder::weight_balanced(weights.view()));
    expect_valid(bisected, weights, "weight_balanced");
    TestFailure::expect(bisected.cost >= optimum, label, "weight_balanced below the optimum");
}

void check_against_enumeration(SplitMix64& random) {
    std::size_t checked = 0;
    for (const WeightFamily family : all_families) {
        std::size_t key_count = 0;
        for (key_count = 0; key_count <= 8; key_count += 1) {
            std::size_t trial = 0;
            for (trial = 0; trial < 6; trial += 1) {
                const OwnedWeights weights = WeightMaker::make(family, key_count, random);
                const ExactCost exhaustive = TreeEnumeration::optimal_cost(weights);
                const ExactCost cubic = CubicReference::optimal_cost(weights);
                TestFailure::expect(exhaustive == cubic, "enumeration", "cubic recurrence disagrees with enumeration");
                const SearchTree by_knuth = take(OptimalTreeBuilder::knuth(weights.view()));
                expect_valid(by_knuth, weights, "knuth small");
                TestFailure::expect(by_knuth.cost == exhaustive, "knuth small", "not optimal");
                expect_other_constructions(weights, by_knuth, exhaustive, "small");
                checked += 1;
            }
        }
    }
    std::printf("enumeration: %zu instances, n <= 8\n", checked);
}

void check_against_cubic(SplitMix64& random) {
    std::size_t checked = 0;
    for (const WeightFamily family : all_families) {
        std::size_t trial = 0;
        for (trial = 0; trial < 12; trial += 1) {
            const std::size_t key_count = 9 + static_cast<std::size_t>(random.below(140));
            const OwnedWeights weights = WeightMaker::make(family, key_count, random);
            const ExactCost cubic = CubicReference::optimal_cost(weights);
            const SearchTree by_knuth = take(OptimalTreeBuilder::knuth(weights.view()));
            expect_valid(by_knuth, weights, "knuth");
            TestFailure::expect(by_knuth.cost == cubic, "knuth", "not optimal");
            expect_other_constructions(weights, by_knuth, cubic, "cubic");
            checked += 1;
        }
    }
    std::printf("cubic: %zu instances, 9 <= n < 149\n", checked);
}

void check_larger(SplitMix64& random) {
    std::size_t checked = 0;
    for (const WeightFamily family : all_families) {
        const std::size_t key_count = 600 + static_cast<std::size_t>(random.below(600));
        const OwnedWeights weights = WeightMaker::make(family, key_count, random);
        const SearchTree by_knuth = take(OptimalTreeBuilder::knuth(weights.view()));
        expect_valid(by_knuth, weights, "knuth large");
        expect_other_constructions(weights, by_knuth, by_knuth.cost, "large");
        checked += 1;
    }
    std::printf("knuth: %zu instances, 600 <= n < 1200\n", checked);
}

void check_edges() {
    OwnedWeights empty;
    empty.gaps.assign(1, 7);
    TestFailure::expect(take(OptimalTreeBuilder::knuth(empty.view())).cost == 0, "edges", "empty knuth");
    TestFailure::expect(take(OptimalTreeBuilder::weight_balanced(empty.view())).shape.root == no_key, "edges", "empty balanced");

    OwnedWeights zeros;
    zeros.keys.assign(5, 0);
    zeros.gaps.assign(6, 0);
    const SearchTree zero_tree = take(OptimalTreeBuilder::knuth(zeros.view()));
    expect_valid(zero_tree, zeros, "zero weights");
    TestFailure::expect(zero_tree.cost == 0, "zero weights", "cost");

    OwnedWeights mismatched;
    mismatched.keys.assign(3, 1);
    mismatched.gaps.assign(3, 1);
    TestFailure::expect(refused(OptimalTreeBuilder::knuth(mismatched.view())), "edges", "mismatched lengths accepted");

    // Largest weights: the 128-bit domain still holds them at this size.
    OwnedWeights heavy;
    heavy.keys.assign(64, ~static_cast<SearchWeight>(0));
    heavy.gaps.assign(65, ~static_cast<SearchWeight>(0));
    const SearchTree heavy_knuth = take(OptimalTreeBuilder::knuth(heavy.view()));
    // These weights take the 128-bit Knuth path; the cubic reference agrees.
    TestFailure::expect(heavy_knuth.cost == CubicReference::optimal_cost(heavy), "edges", "128-bit knuth not optimal");
    expect_other_constructions(heavy, heavy_knuth, heavy_knuth.cost, "heavy");
    OwnedWeights heavy_gaps;
    heavy_gaps.keys.assign(40, 0);
    heavy_gaps.gaps.assign(41, ~static_cast<SearchWeight>(0));
    heavy_gaps.gaps[7] = 3;
    const SearchTree heavy_gaps_knuth = take(OptimalTreeBuilder::knuth(heavy_gaps.view()));
    TestFailure::expect(heavy_gaps_knuth.cost == CubicReference::optimal_cost(heavy_gaps), "edges", "128-bit knuth, gaps");
    expect_other_constructions(heavy_gaps, heavy_gaps_knuth, heavy_gaps_knuth.cost, "heavy gaps");
    expect_other_constructions(zeros, take(OptimalTreeBuilder::knuth(zeros.view())), 0, "zeros");

    TestFailure::expect(take(OptimalTreeBuilder::knuth_scattered(empty.view())).cost == 0, "edges", "empty knuth_scattered");
    TestFailure::expect(take(OptimalTreeBuilder::garsia_wachs(empty.view())).shape.root == no_key, "edges", "empty garsia_wachs");
    TestFailure::expect(take(OptimalTreeBuilder::balanced(empty.view())).shape.root == no_key, "edges", "empty balanced");
    TestFailure::expect(take(OptimalTreeBuilder::bottom_up(empty.view())).shape.root == no_key, "edges", "empty bottom_up");

    // Constant weights other than one: the balanced tree is optimal.
    OwnedWeights constant;
    constant.keys.assign(37, 3);
    constant.gaps.assign(38, 5);
    expect_other_constructions(constant, take(OptimalTreeBuilder::knuth(constant.view())), take(OptimalTreeBuilder::knuth(constant.view())).cost, "constant");
}

// A construction refused for its size (maximum_table_cells), before it
// allocates.
[[nodiscard]] bool too_large(const stxlib::Result<SearchTree>& built) {
    return !built.succeeded() && (*built.error()).kind() == stxlib::ErrorKind::limit_exceeded;
}

// Audit A, A1 and A2: bottom_up's chain is linked iteratively, so weights
// that make its spines as long as the halves build a valid tree; Knuth's
// triangle over 23,169 keys exceeds maximum_table_cells and both layouts
// refuse it before allocating.
void check_limits() {
    const std::size_t key_count = 1500;
    OwnedWeights ramp;
    ramp.keys.assign(key_count, 0);
    ramp.gaps.assign(key_count + 1, 0);
    std::size_t key = 0;
    for (key = 0; key < key_count; key += 1) {
        const SearchWeight distance = static_cast<SearchWeight>(key_count - key);
        ramp.keys[key] = distance * distance * distance;
    }
    const SearchTree by_knuth = take(OptimalTreeBuilder::knuth(ramp.view()));
    const SearchTree joined = take(OptimalTreeBuilder::bottom_up(ramp.view()));
    expect_valid(joined, ramp, "bottom_up ramp");
    TestFailure::expect(joined.cost >= by_knuth.cost, "bottom_up ramp", "below the optimum");

    OwnedWeights wide;
    wide.keys.assign(23169, 1);
    wide.gaps.assign(23170, 1);
    TestFailure::expect(too_large(OptimalTreeBuilder::knuth(wide.view())), "limits", "knuth over the table limit");
    TestFailure::expect(too_large(OptimalTreeBuilder::knuth_scattered(wide.view())), "limits",
                        "knuth_scattered over the table limit");
    std::printf("limits: bottom_up on a cubic ramp of %zu keys; knuth refused at 23169 keys\n", key_count);
}

// Garsia-Wachs at sizes the enumeration and cubic checks do not reach, on
// gap weights with zeros, ties and wide ranges, against Knuth.
void check_garsia_wachs(SplitMix64& random) {
    std::size_t checked = 0;
    std::size_t trial = 0;
    for (trial = 0; trial < 24; trial += 1) {
        const std::size_t key_count = 200 + static_cast<std::size_t>(random.below(1400));
        OwnedWeights weights;
        weights.keys.assign(key_count, 0);
        weights.gaps.assign(key_count + 1, 0);
        std::size_t gap = 0;
        for (gap = 0; gap <= key_count; gap += 1) {
            switch (trial % 4) {
            case 0:
                weights.gaps[gap] = random.below(4);
                break;
            case 1:
                weights.gaps[gap] = random.below(static_cast<SearchWeight>(1) << 40);
                break;
            case 2:
                weights.gaps[gap] = static_cast<SearchWeight>(1000000 / (1 + random.below(key_count)));
                break;
            default:
                weights.gaps[gap] = gap < key_count / 2 ? gap + 1 : key_count - gap + 1;
                break;
            }
        }
        const SearchTree by_knuth = take(OptimalTreeBuilder::knuth(weights.view()));
        const SearchTree alphabetic = take(OptimalTreeBuilder::garsia_wachs(weights.view()));
        expect_valid(alphabetic, weights, "garsia_wachs large");
        TestFailure::expect(alphabetic.cost == by_knuth.cost, "garsia_wachs large", "not optimal");
        checked += 1;
    }
    std::printf("garsia_wachs: %zu gap-only instances, 200 <= n < 1600\n", checked);
}

} // namespace

int main() {
    SplitMix64 random(0x0b57'2026'1025ULL);
    check_edges();
    check_against_enumeration(random);
    check_against_cubic(random);
    check_larger(random);
    check_garsia_wachs(random);
    check_limits();
    std::printf("optimal tree tests passed\n");
    return 0;
}
