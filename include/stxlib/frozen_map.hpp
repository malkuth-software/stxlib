#pragma once

// FrozenMap and FrozenTextMap: ordered maps built once and never changed
// (roadmap M2, R5; audit B: std::map allocates every node through the
// global heap and throws; owner, 2026-11-15: "a compiled facade, FrozenMap,
// on the RowHeightIndex precedent").
//
// The compiled facade over stx_btree's static optimal map
// (src/algorithms/stx_btree/optimal_map.hpp, from stx_auxlib b6e330a; its
// templates stay private to STXlib). The keys are laid out as an optimal (or
// minimum-path) binary search tree packed into 128-byte multiway nodes that
// keep short windows of the keys, so a lookup reads few cache lines. The
// facade also keeps the keys and values in sorted order, which gives
// lower_bound, positions and in-order iteration exactly.
//
// Two key types:
// - FrozenMap: u64 keys in numeric order;
// - FrozenTextMap: text keys (well-formed UTF-8, copied in) in byte order,
//   which is code point order (compare_text, text.hpp).
// Values are u64: a caller with richer values stores them in its own
// Vector and maps keys to their indices.
//
// Building:
// - build(keys, values): every key equally likely to be searched, and every
//   gap between keys too; the tree is the minimum-internal-path tree, which
//   is optimal for exactly those weights;
// - build_weighted(keys, values, key_weights, gap_weights): one weight per
//   key (how often it is found) and one per gap (how often a search falls
//   between keys[i - 1] and keys[i]; count + 1 of them). The tree is the
//   exact optimum (Knuth) up to knuth_limit keys, and the near-optimal
//   bottom-up construction above it (within 0.4-1.7% on stx_auxlib's
//   benchmark families).
// Keys must be strictly ascending. Failures: invalid_argument for unsorted
// or repeated keys (detail code: the index of the first key out of order),
// for lengths that differ, and for weights the construction refuses;
// limit_exceeded for more than key_limit keys or text whose total length is
// not representable; out_of_memory when storage cannot be had. The inputs
// are only read.
//
// Determinism: the layout is a function of the keys and the weights alone.
// Lookups return the same answers whatever the layout.
//
// Storage: everything comes from STXlib's allocator. A moved-from map is
// empty. Execution: a built map is immutable, so any number of threads may
// read it at once.
//
// tests/frozen_map_tests.cpp compares both maps with a sorted model over the
// domains proofs/REPORT.md states.

// Early exits (owner, 2026-11-15, decisions/2026-11-15-early-exit-
// integrity.md): no consumer code runs inside these maps (the keys are
// numbers and bytes, compared by STXlib's own code). The calls outside
// STXlib are the host C library's memcpy and memcmp and std::string_view's
// inline members; form 1 at each: a build fills local storage and publishes
// it into the result only when complete, and lookups change nothing.

#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

class FrozenMap final {
public:
    // KeyIndex in the template is a signed 32-bit position.
    static constexpr usize key_limit = 0x7ffffffe;
    static constexpr usize knuth_limit = 1024;

    // The empty map.
    FrozenMap() noexcept = default;
    FrozenMap(FrozenMap&& other) noexcept;
    FrozenMap& operator=(FrozenMap&& other) noexcept;
    FrozenMap(const FrozenMap&) = delete;
    FrozenMap& operator=(const FrozenMap&) = delete;
    ~FrozenMap();

    [[nodiscard]] static OperationResult<FrozenMap> build(View<u64> keys, View<u64> values) noexcept;
    [[nodiscard]] static OperationResult<FrozenMap> build_weighted(View<u64> keys, View<u64> values,
                                                                   View<u64> key_weights,
                                                                   View<u64> gap_weights) noexcept;

    [[nodiscard]] usize count() const noexcept { return keys_.count(); }
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }

    // The value of `key`, or null. Borrowed from the map.
    [[nodiscard]] const u64* find(u64 key) const& noexcept;
    const u64* find(u64 key) const&& = delete;
    [[nodiscard]] bool contains(u64 key) const noexcept;

    // The position of the first key not less than `key`, or count().
    [[nodiscard]] usize lower_bound(u64 key) const noexcept;

    // The keys and values in ascending key order. Requirement for the
    // positional forms: index < count().
    [[nodiscard]] u64 key_at(usize index) const noexcept;
    [[nodiscard]] u64 value_at(usize index) const noexcept;
    [[nodiscard]] View<u64> keys() const& noexcept { return keys_.view(); }
    View<u64> keys() const&& = delete;
    [[nodiscard]] View<u64> values() const& noexcept { return values_.view(); }
    View<u64> values() const&& = delete;

private:
    void release() noexcept;

    // The private optimal map from key to position, on memory::allocate;
    // null when empty.
    void* map_{nullptr};
    Vector<u64> keys_;
    Vector<u64> values_;
};

class FrozenTextMap final {
public:
    static constexpr usize key_limit = FrozenMap::key_limit;
    static constexpr usize knuth_limit = FrozenMap::knuth_limit;

    FrozenTextMap() noexcept = default;
    FrozenTextMap(FrozenTextMap&& other) noexcept;
    FrozenTextMap& operator=(FrozenTextMap&& other) noexcept;
    FrozenTextMap(const FrozenTextMap&) = delete;
    FrozenTextMap& operator=(const FrozenTextMap&) = delete;
    ~FrozenTextMap();

    // The keys are copied; the views need only last for the call.
    [[nodiscard]] static OperationResult<FrozenTextMap> build(View<TextView> keys, View<u64> values) noexcept;
    [[nodiscard]] static OperationResult<FrozenTextMap> build_weighted(View<TextView> keys, View<u64> values,
                                                                       View<u64> key_weights,
                                                                       View<u64> gap_weights) noexcept;

    [[nodiscard]] usize count() const noexcept { return values_.count(); }
    [[nodiscard]] bool empty() const noexcept { return values_.empty(); }

    [[nodiscard]] const u64* find(TextView key) const& noexcept;
    const u64* find(TextView key) const&& = delete;
    [[nodiscard]] bool contains(TextView key) const noexcept;
    [[nodiscard]] usize lower_bound(TextView key) const noexcept;

    // Borrowed from the map. Requirement: index < count().
    [[nodiscard]] TextView key_at(usize index) const& noexcept;
    TextView key_at(usize index) const&& = delete;
    [[nodiscard]] u64 value_at(usize index) const noexcept;
    [[nodiscard]] View<u64> values() const& noexcept { return values_.view(); }
    View<u64> values() const&& = delete;

private:
    void release() noexcept;

    void* map_{nullptr};
    Vector<char> bytes_;   // every key's bytes, in key order
    Vector<usize> starts_; // count() + 1 offsets into bytes_
    Vector<u64> values_;
};

} // namespace stxlib
