#pragma once

// RowHeightIndex: the heights of a list of rows, for virtualized views. It
// keeps every row's height in 1/64 logical units (26.6 fixed point) with a
// state (estimated, measured, approximate) and answers the offset of a row
// (the exact sum of the heights before it), the row at an offset and a
// row's extent, each in O(log n), with cursors that step in O(1) amortized
// and seek from where they are in O(log_B d) for a target d rows away.
//
// The compiled facade over stx_btree's sequence tree (coordinator's choice,
// option 1: the tree's templates stay private to STXlib, under
// src/algorithms/stx_btree/, from stx_auxlib b6e330a). The tree runs in the
// configuration stx_auxlib measured as P5/P6 (branches of 21 slots with the
// counting child search, leaves of 127 words, dense leaves admitted; study
// src/algorithms/stx_btree/ORDER_STATISTIC_RESEARCH.md §9). Memory per
// row: a measured row in a dense leaf takes 4 bytes; a run of estimated or
// approximate rows takes one 8-byte entry, and coarsen turns detail the
// view no longer needs into such runs while keeping every offset outside
// the coarsened range unchanged bit for bit. The design, the invariants
// (I1-I6) and their proof sketches are the study's §3 and §8.
//
// The caller keeps the conversion between its logical units and these
// units, the view policies (anchor, overscan, when to forget and coarsen)
// and its counters.
//
// Units: heights and offsets are i64 units; rows and row counts are u64.
// Limits: one row at most height_limit units; the total at most
// total_limit; at most row_limit rows.
//
// Failure: an edit that cannot be done fails with a record and leaves the
// index unchanged (every page an edit needs is reserved before it changes
// anything):
// - invalid_argument: a row past the end where one is required, or a
//   height outside [0, height_limit];
// - limit_exceeded: the total or the row count would pass its limit;
// - out_of_memory: a page could not be allocated.
// Queries and cursor moves never fail.
//
// Storage: the index and its pages come from memory::allocate. The index
// object is one allocation of about 44 KB (the tree's fixed edit scratch);
// pages are 1,024-byte leaves and 512-byte branches. A moved-from index may
// only be destroyed or assigned to.
//
// Execution: one thread at a time. A cursor belongs to one index and is
// valid until that index's next edit; a stale cursor (or one from another
// index) re-seeks: from the root for the seeks, from its row for advance.

#include "stxlib/result.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib {

enum class RowState : u8 {
    estimated = 0,
    measured = 1,
    approximate = 2,
};

// Where a row is: its top, its height and its state.
struct RowExtent final {
    i64 top{0};
    i64 height{0};
    RowState state{RowState::estimated};
};

class RowHeightIndex final {
public:
    // The tallest row: one packed entry holds it.
    static constexpr i64 height_limit = (static_cast<i64>(1) << 38) - 1;
    static constexpr i64 total_limit = (static_cast<i64>(1) << 62) - 1;
    static constexpr u64 row_limit = (static_cast<u64>(1) << 62) - 1;
    // The tree's cursor (its path from the root), held opaquely; the
    // implementation checks at compile time that it fits.
    static constexpr usize cursor_path_bytes = 2304;

    // A position with the path to it. A plain value: copies are
    // independent cursors.
    class Cursor final {
    public:
        // A cursor at no position (at_end()); any seek places it.
        Cursor() noexcept;

        [[nodiscard]] u64 row() const noexcept { return row_; }
        [[nodiscard]] i64 top() const noexcept { return top_; }
        // 0 at the end.
        [[nodiscard]] i64 height() const noexcept { return height_; }
        [[nodiscard]] RowState state() const noexcept { return state_; }
        [[nodiscard]] bool at_end() const noexcept { return at_end_; }

    private:
        friend class RowHeightIndex;

        alignas(8) Byte path_[cursor_path_bytes];
        // The position as last placed, so the readers above stay inline.
        u64 row_{0};
        i64 top_{0};
        i64 height_{0};
        RowState state_{RowState::estimated};
        bool at_end_{true};
    };

    // An empty index whose new rows are estimated at `estimate` units; an
    // estimate outside [1, height_limit] falls back to 64 (one logical
    // unit). Failure: out_of_memory.
    [[nodiscard]] static Result<RowHeightIndex> create(i64 estimate) noexcept;

    RowHeightIndex(RowHeightIndex&& other) noexcept;
    RowHeightIndex& operator=(RowHeightIndex&& other) noexcept;
    RowHeightIndex(const RowHeightIndex&) = delete;
    RowHeightIndex& operator=(const RowHeightIndex&) = delete;
    ~RowHeightIndex();

    // Replaces every row with `rows` estimated rows at `estimate` (falling
    // back as create's does), which becomes the estimate. Failures:
    // limit_exceeded; out_of_memory.
    [[nodiscard]] Status assign(u64 rows, i64 estimate) noexcept;

    [[nodiscard]] u64 count() const noexcept;
    // Rows in the measured state, O(1).
    [[nodiscard]] u64 measured_count() const noexcept;
    [[nodiscard]] i64 total() const noexcept;
    [[nodiscard]] i64 estimate() const noexcept;

    // The exact sum of the heights of rows before `row`; total() at or past
    // the end.
    [[nodiscard]] i64 offset_of(u64 row) const noexcept;
    // The least row r with offset_of(r + 1) > offset; row 0 below zero or
    // when empty, the last row at or past total().
    [[nodiscard]] u64 row_at(i64 offset) const noexcept;
    // One descent. Past the end: the bottom with height 0.
    [[nodiscard]] RowExtent extent_of(u64 row) const noexcept;

    // Records a measured height: true when the row's height or state
    // changed, false when it was already measured at that height (nothing
    // changes). Failures: invalid_argument (a row past the end, a height
    // outside [0, height_limit]); limit_exceeded (the total); out_of_memory.
    [[nodiscard]] Result<bool> set_height(u64 row, i64 height) noexcept;
    // Records measured heights for rows [row, row + heights.count()), one
    // leaf pass per leaf touched; refused as a whole. An empty view
    // succeeds. Failures: invalid_argument (the range passes the end, or a
    // height outside [0, height_limit]); limit_exceeded; out_of_memory.
    [[nodiscard]] Status set_heights(u64 row, View<i64> heights) noexcept;
    // Inserts `rows` estimated rows before `row` (clamped to count()).
    // Failures: limit_exceeded; out_of_memory.
    [[nodiscard]] Status insert(u64 row, u64 rows) noexcept;
    // Erases rows [row, row + rows), clamped to the end. Failure:
    // out_of_memory (an erase inside a run can split it).
    [[nodiscard]] Status erase(u64 row, u64 rows) noexcept;
    // Returns rows [row, row + rows) (clamped) to the estimate, unmeasured;
    // every row keeps its position. Failures: limit_exceeded;
    // out_of_memory.
    [[nodiscard]] Status forget(u64 row, u64 rows) noexcept;
    // Replaces rows [row, row + rows) (clamped) by one approximate run
    // holding their exact total; the offset of every row at or before `row`
    // and at or after the range's end is unchanged bit for bit. Failure:
    // out_of_memory.
    [[nodiscard]] Status coarsen(u64 row, u64 rows) noexcept;

    // Cursors: seek_row places at `row` (past the end: at_end()); seek_offset
    // at row_at(offset); seek_offset_from moves a valid cursor there in
    // O(log_B d), climbing only as far as needed; advance steps to the next
    // row, O(1) amortized.
    void seek_row(Cursor& cursor, u64 row) const noexcept;
    void seek_offset(Cursor& cursor, i64 offset) const noexcept;
    void seek_offset_from(Cursor& cursor, i64 offset) const noexcept;
    void advance(Cursor& cursor) const noexcept;

    // Storage, for bounded-memory checks, all O(1): nodes, runs (packed
    // entries plus dense rows), the depth, and the bytes held (the index
    // object and its live pages; the few spare pages kept for edits, at most
    // 8 leaves and 32 branches, are not counted).
    [[nodiscard]] usize node_count() const noexcept;
    [[nodiscard]] usize run_count() const noexcept;
    [[nodiscard]] usize depth() const noexcept;
    [[nodiscard]] usize storage_bytes() const noexcept;
    // The first broken structural or arithmetic invariant (I1-I6, study
    // §8.2), or empty when all hold; the text has static storage and a NUL
    // after it. O(n), for tests.
    [[nodiscard]] TextView invariant_failure() const noexcept;

private:
    explicit RowHeightIndex(void* tree) noexcept : tree_(tree) {}
    // Copies the tree cursor's position into the cursor's readers.
    static void settle(Cursor& cursor) noexcept;

    // The private sequence tree, on memory::allocate; null only when moved
    // from.
    void* tree_{nullptr};
};

} // namespace stxlib
