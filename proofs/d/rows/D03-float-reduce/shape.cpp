// Row D03 (phase D, targeted), exhaustive enumeration: the reduction order
// of src/core/float_reduce.hpp (F5 = 1) over symbolic terms.
// P1 (shape): for every count n in [0, 1100], ordered_sum and ordered_dot
//     build exactly the expression F5's text defines (lanes, the fixed
//     tree, the tail in index order, no added zero, +0 only for n = 0,
//     each product rounded on its own).
// P2 (chunks): for every count in [0, 40] and within 9 of each multiple of
//     chunk_terms up to 3 * 4096 (and 4 * 4096 + 11), chunked_sum and
//     chunked_dot build F5's chunk combination, and the pool's data flow
//     (describe_chunks, reduce_chunk in any order of the chunks, then
//     combine_chunks) builds the same expression: every order of the chunk
//     tasks for up to 4 chunks, forward and reverse beyond.
// Method. T is instantiated with Term, a hash-consed expression: each + and
//     * makes a node naming its two operands in order, so two computations
//     give the same node exactly when they perform the same operations on
//     the same terms in the same order and grouping. IEEE-754 addition and
//     multiplication in one type, rounded to nearest-even with no
//     contraction and no wider intermediates (rules 1, 3 and 5;
//     float_discipline.hpp refuses builds that break them), are functions of
//     their operands, so equal expressions give equal bits for every value of
//     the terms: the claim holds for every input, not a sample. It is
//     stricter than needed (a + b and b + a are different nodes though
//     IEEE addition commutes), so a pass is never too generous.
// The specification below is written from the header's text (lines 12-41)
//     as folds over index lists, not from the code's loops.
// Assumed, not proved here: that the compiler keeps the source's
//     operations and order for T = float and double (the trust chain's T3
//     question); that AtomicPool::run runs every task once and returns
//     after all of them (thread_pool.cpp's property, REPORT batch 2c).
// Bound: the counts above. Tool: this program, compiled by the pinned
//     clang (tools/enumerate_d.sh); exit status 0 only if every check holds.
#include "stxlib/types.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=7d1b60e63a2dc6a28180f30d1941df8b628ea53c676cb251c65495c2a02f4b58

//@ model-begin reason="View and STXLIB_REQUIRE reduced to what the lifted code uses (data, count; a failed requirement stops the run)"
#define STXLIB_REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            std::fprintf(stderr, "requirement failed: %s\n", message); \
            std::exit(2); \
        } \
    } while (false)
namespace stxlib {
template <typename T>
class View final {
public:
    View(const T* const data, const usize count) noexcept : data_(data), count_(count) {}
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    const T* data_;
    usize count_;
};
} // namespace stxlib
//@ model-end

namespace {

// ---- symbolic terms ----------------------------------------------------------

enum Kind : unsigned { zero_kind = 0, leaf_kind = 1, add_kind = 2, multiply_kind = 3 };

class Store final {
public:
    int make(const unsigned kind, const unsigned left, const unsigned right) {
        const unsigned long long key = (static_cast<unsigned long long>(kind) << 62) |
                                       (static_cast<unsigned long long>(left) << 31) | right;
        const auto found = index_.find(key);
        if (found != index_.end()) return found->second;
        const int id = static_cast<int>(count_);
        count_ = count_ + 1;
        index_.emplace(key, id);
        return id;
    }
    [[nodiscard]] unsigned long long count() const { return count_; }

private:
    std::unordered_map<unsigned long long, int> index_;
    unsigned long long count_ = 0;
};

Store store;
unsigned long long failures = 0;

void fail(const char* const what, const unsigned long long n) {
    if (failures < 20) std::printf("FAIL %s at n = %llu\n", what, n);
    failures = failures + 1;
}

struct Term final {
    int id = -1;  // -1: never written (a lane read before its first term)
    Term() = default;
    explicit Term(const int literal) {
        if (literal != 0) std::exit(3);
        id = store.make(zero_kind, 0, 0);
    }
    static Term of(const int node) {
        Term made;
        made.id = node;
        return made;
    }
};

Term operator+(const Term left, const Term right) {
    if (left.id < 0 || right.id < 0) {
        fail("an operand read before it was written", 0);
        return Term::of(-1);
    }
    return Term::of(store.make(add_kind, static_cast<unsigned>(left.id), static_cast<unsigned>(right.id)));
}

Term operator*(const Term left, const Term right) {
    if (left.id < 0 || right.id < 0) {
        fail("an operand read before it was written", 0);
        return Term::of(-1);
    }
    return Term::of(store.make(multiply_kind, static_cast<unsigned>(left.id), static_cast<unsigned>(right.id)));
}

} // namespace

//@ lift source=stxlib path=src/core/float_reduce.hpp lines=60-223 namespace=stxlib::float_reduce sha256=f6df01e5adb86ae6f2cdbf233afefb0f29e05075051fa73349799d249729a7c1
//@ mutant id=m1 source=stxlib path=src/core/float_reduce.hpp line=66 from="(accumulator[0] + accumulator[1]) + (accumulator[2] + accumulator[3])" to="((accumulator[0] + accumulator[1]) + accumulator[2]) + accumulator[3]"

namespace {

using stxlib::usize;
namespace fr = stxlib::float_reduce;

// ---- the specification, from the header's text ----------------------------------

int add(const int left, const int right) {
    return store.make(add_kind, static_cast<unsigned>(left), static_cast<unsigned>(right));
}

// ((start + items[0]) + items[1]) + ...
int fold_left(int start, const std::vector<int>& items, const usize first) {
    for (usize position = first; position < items.size(); ++position) start = add(start, items[position]);
    return start;
}

// F5's shape over the term nodes x[0..n).
int spec_sum(const std::vector<int>& x) {
    const usize n = x.size();
    if (n == 0) return store.make(zero_kind, 0, 0);
    const usize blocks = n / 8;
    if (blocks == 0) return fold_left(x[0], x, 1);
    std::array<int, 8> lane{};
    for (usize l = 0; l < 8; ++l) {
        std::vector<int> column;
        for (usize k = 0; k < blocks; ++k) column.push_back(x[8 * k + l]);
        lane[l] = fold_left(column[0], column, 1);
    }
    const int tree = add(add(add(lane[0], lane[1]), add(lane[2], lane[3])),
                         add(add(lane[4], lane[5]), add(lane[6], lane[7])));
    std::vector<int> tail(x.begin() + static_cast<long>(8 * blocks), x.end());
    return fold_left(tree, tail, 0);
}

// The chunk combination: r0, then r + r1, and so on; +0 for no chunk.
int spec_chunked(const std::vector<int>& x) {
    if (x.empty()) return store.make(zero_kind, 0, 0);
    int total = -1;
    for (usize first = 0; first < x.size(); first += 4096) {
        const usize last = std::min(x.size(), first + 4096);
        const std::vector<int> chunk(x.begin() + static_cast<long>(first), x.begin() + static_cast<long>(last));
        const int partial = spec_sum(chunk);
        total = total < 0 ? partial : add(total, partial);
    }
    return total;
}

std::vector<Term> left_terms;
std::vector<Term> right_terms;

std::vector<int> sum_nodes(const usize n) {
    std::vector<int> nodes;
    for (usize i = 0; i < n; ++i) nodes.push_back(left_terms[i].id);
    return nodes;
}

std::vector<int> dot_nodes(const usize n) {
    std::vector<int> nodes;
    for (usize i = 0; i < n; ++i) {
        nodes.push_back(store.make(multiply_kind, static_cast<unsigned>(left_terms[i].id),
                                   static_cast<unsigned>(right_terms[i].id)));
    }
    return nodes;
}

// The pool's data flow with the chunk tasks run in `order`.
Term pooled(const usize n, const bool dot, const std::vector<usize>& order) {
    std::vector<fr::Chunk<Term>> chunks(fr::chunk_count(n));
    fr::describe_chunks(left_terms.data(), dot ? right_terms.data() : nullptr, n, chunks.data());
    for (const usize index : order) fr::reduce_chunk(chunks[index]);
    return fr::combine_chunks(chunks.data(), chunks.size());
}

void check_pool(const usize n, const bool dot, const int expected) {
    const usize count = fr::chunk_count(n);
    std::vector<usize> order(count);
    for (usize i = 0; i < count; ++i) order[i] = i;
    if (count <= 4) {
        do {
            if (pooled(n, dot, order).id != expected) fail(dot ? "pooled dot (an order)" : "pooled sum (an order)", n);
        } while (std::next_permutation(order.begin(), order.end()));
        return;
    }
    if (pooled(n, dot, order).id != expected) fail(dot ? "pooled dot (forward)" : "pooled sum (forward)", n);
    std::reverse(order.begin(), order.end());
    if (pooled(n, dot, order).id != expected) fail(dot ? "pooled dot (reverse)" : "pooled sum (reverse)", n);
}

} // namespace

int main() {
    const usize largest = 4 * 4096 + 11;
    for (usize i = 0; i < largest; ++i) {
        left_terms.push_back(Term::of(store.make(leaf_kind, static_cast<unsigned>(i), 0)));
        right_terms.push_back(Term::of(store.make(leaf_kind, static_cast<unsigned>(i), 1)));
    }
    unsigned long long shapes = 0;
    for (usize n = 0; n <= 1100; ++n) {
        if (fr::ordered_sum(left_terms.data(), n).id != spec_sum(sum_nodes(n))) fail("ordered_sum", n);
        if (fr::ordered_dot(left_terms.data(), right_terms.data(), n).id != spec_sum(dot_nodes(n))) fail("ordered_dot", n);
        shapes = shapes + 2;
    }
    std::vector<usize> counts;
    for (usize n = 0; n <= 40; ++n) counts.push_back(n);
    for (usize k = 1; k <= 3; ++k) {
        for (usize d = 0; d <= 18; ++d) counts.push_back(k * 4096 - 9 + d);
    }
    counts.push_back(largest);
    unsigned long long chunked = 0;
    for (const usize n : counts) {
        const stxlib::View<Term> left(left_terms.data(), n);
        const stxlib::View<Term> right(right_terms.data(), n);
        const int sum_expected = spec_chunked(sum_nodes(n));
        const int dot_expected = spec_chunked(dot_nodes(n));
        if (fr::chunk_count(n) != (n + 4095) / 4096) fail("chunk_count", n);
        if (fr::chunked_sum(left).id != sum_expected) fail("chunked_sum", n);
        if (fr::chunked_dot(left, right).id != dot_expected) fail("chunked_dot", n);
        check_pool(n, false, sum_expected);
        check_pool(n, true, dot_expected);
        chunked = chunked + 1;
    }
    std::printf("D03 shape: %llu ordered shapes (n = 0..1100), %llu chunked counts, %llu nodes; %llu failures\n",
                shapes, chunked, store.count(), failures);
    return failures == 0 ? 0 : 1;
}
