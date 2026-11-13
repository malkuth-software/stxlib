// The compiled facade over stx_btree's sequence tree
// (include/stxlib/row_height_index.hpp): STXlib's vocabulary outside, the
// private template RowHeightIndex<> (src/algorithms/stx_btree/
// row_height_index.hpp, from stx_auxlib b6e330a) inside. Every call is one
// call into the template; refusals are classified only after the template
// refused, so a successful edit costs nothing extra.

#include "stxlib/row_height_index.hpp"

#include "algorithms/stx_btree/row_height_index.hpp"
#include "stxlib/error.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"

#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>

namespace stxlib {

namespace {

using Index = stx_btree::RowHeightIndex<>;
using TreeCursor = Index::Cursor;
using stx_btree::HeightUnits;
using stx_btree::RowIndex;

static_assert(sizeof(TreeCursor) <= RowHeightIndex::cursor_path_bytes,
              "the tree's cursor must fit RowHeightIndex::Cursor's storage");
static_assert(alignof(TreeCursor) <= 8, "the tree's cursor must fit RowHeightIndex::Cursor's alignment");
static_assert(std::is_trivially_copyable_v<TreeCursor> && std::is_trivially_destructible_v<TreeCursor>,
              "RowHeightIndex::Cursor copies the tree's cursor as bytes");
static_assert(Index::height_limit.value == RowHeightIndex::height_limit, "height limits agree");
static_assert(Index::total_limit.value == RowHeightIndex::total_limit, "total limits agree");
static_assert(Index::row_limit == RowHeightIndex::row_limit, "row limits agree");
static_assert(static_cast<u8>(stx_btree::RowState::estimated) == static_cast<u8>(RowState::estimated) &&
                  static_cast<u8>(stx_btree::RowState::measured) == static_cast<u8>(RowState::measured) &&
                  static_cast<u8>(stx_btree::RowState::approximate) == static_cast<u8>(RowState::approximate),
              "row states agree");

[[nodiscard]] Index& index_of(void* const tree) noexcept {
    STXLIB_REQUIRE(tree != nullptr, "stxlib::RowHeightIndex used after a move");
    return *static_cast<Index*>(tree);
}

[[nodiscard]] const Index& index_of(const void* const tree) noexcept {
    STXLIB_REQUIRE(tree != nullptr, "stxlib::RowHeightIndex used after a move");
    return *static_cast<const Index*>(tree);
}

[[nodiscard]] RowState state_of(const stx_btree::RowState state) noexcept {
    const RowState converted = static_cast<RowState>(static_cast<u8>(state));
    return converted;
}

[[nodiscard]] RowIndex row_index(const u64 row) noexcept {
    const RowIndex index{static_cast<std::uint64_t>(row)};
    return index;
}

[[nodiscard]] HeightUnits units(const i64 value) noexcept {
    const HeightUnits held{static_cast<std::int64_t>(value)};
    return held;
}

[[nodiscard]] Status failed(const ErrorKind kind, const Operation operation) noexcept {
    return Status::failure(ErrorRecord::make(kind, operation));
}

// After the template refused an edit for which only a limit or memory can
// refuse it.
[[nodiscard]] Status limit_or_memory(const Index& index, const Operation operation) noexcept {
    if (index.out_of_memory()) return failed(ErrorKind::out_of_memory, operation);
    return failed(ErrorKind::limit_exceeded, operation);
}

void destroy(void* const tree) noexcept {
    if (tree == nullptr) return;
    (*static_cast<Index*>(tree)).~Index();
    memory::release(tree);
}

} // namespace

// ---- cursor -----------------------------------------------------------------

RowHeightIndex::Cursor::Cursor() noexcept {
    // The whole storage is defined, so a copy never reads indeterminate
    // bytes past the tree's cursor.
    std::memset(path_, 0, sizeof(path_));
    ::new (static_cast<void*>(path_)) TreeCursor();
}

void RowHeightIndex::settle(Cursor& cursor) noexcept {
    const TreeCursor& path = *std::launder(reinterpret_cast<const TreeCursor*>(cursor.path_));
    cursor.row_ = static_cast<u64>(path.row().value);
    cursor.top_ = static_cast<i64>(path.top().value);
    cursor.height_ = static_cast<i64>(path.height().value);
    cursor.state_ = state_of(path.state());
    cursor.at_end_ = path.at_end();
}

// ---- lifetime ---------------------------------------------------------------

Result<RowHeightIndex> RowHeightIndex::create(const i64 estimate) noexcept {
    void* const storage = memory::allocate(sizeof(Index), alignof(Index));
    if (storage == nullptr) {
        return Result<RowHeightIndex>::failure(
            ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.row_height_index.create"));
    }
    Index* const index = ::new (storage) Index(units(estimate));
    if (!(*index).constructed()) {
        destroy(storage);
        return Result<RowHeightIndex>::failure(
            ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.row_height_index.create"));
    }
    RowHeightIndex made(storage);
    return Result<RowHeightIndex>::success(static_cast<RowHeightIndex&&>(made));
}

RowHeightIndex::RowHeightIndex(RowHeightIndex&& other) noexcept : tree_(other.tree_) {
    other.tree_ = nullptr;
}

RowHeightIndex& RowHeightIndex::operator=(RowHeightIndex&& other) noexcept {
    if (this == &other) return *this;
    destroy(tree_);
    tree_ = other.tree_;
    other.tree_ = nullptr;
    return *this;
}

RowHeightIndex::~RowHeightIndex() {
    destroy(tree_);
}

// ---- queries ----------------------------------------------------------------

u64 RowHeightIndex::count() const noexcept {
    return static_cast<u64>(index_of(tree_).count());
}

u64 RowHeightIndex::measured_count() const noexcept {
    return static_cast<u64>(index_of(tree_).measured_count());
}

i64 RowHeightIndex::total() const noexcept {
    return static_cast<i64>(index_of(tree_).total().value);
}

i64 RowHeightIndex::estimate() const noexcept {
    return static_cast<i64>(index_of(tree_).estimate().value);
}

i64 RowHeightIndex::offset_of(const u64 row) const noexcept {
    return static_cast<i64>(index_of(tree_).offset_of(row_index(row)).value);
}

u64 RowHeightIndex::row_at(const i64 offset) const noexcept {
    return static_cast<u64>(index_of(tree_).row_at(units(offset)).value);
}

RowExtent RowHeightIndex::extent_of(const u64 row) const noexcept {
    const stx_btree::RowExtent found = index_of(tree_).extent_of(row_index(row));
    const RowExtent extent{static_cast<i64>(found.top.value), static_cast<i64>(found.height.value),
                           state_of(found.state)};
    return extent;
}

// ---- edits ------------------------------------------------------------------

Status RowHeightIndex::assign(const u64 rows, const i64 estimate) noexcept {
    Index& index = index_of(tree_);
    if (index.assign(static_cast<stx_btree::RowCount>(rows), units(estimate))) return Status::success();
    return limit_or_memory(index, "stxlib.row_height_index.assign");
}

Result<bool> RowHeightIndex::set_height(const u64 row, const i64 height) noexcept {
    Index& index = index_of(tree_);
    if (row >= count() || height < 0 || height > height_limit) {
        return Result<bool>::failure(
            ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.row_height_index.set_height"));
    }
    if (index.set_height(row_index(row), units(height))) return Result<bool>::success(true);
    if (index.out_of_memory()) {
        return Result<bool>::failure(ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.row_height_index.set_height"));
    }
    // Refused with the row and height valid: either the row already holds
    // this measured height, or the total would pass its limit.
    const stx_btree::RowExtent extent = index.extent_of(row_index(row));
    if (extent.state == stx_btree::RowState::measured && static_cast<i64>(extent.height.value) == height) {
        return Result<bool>::success(false);
    }
    return Result<bool>::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.row_height_index.set_height"));
}

Status RowHeightIndex::set_heights(const u64 row, const View<i64> heights) noexcept {
    Index& index = index_of(tree_);
    const std::span<const i64> values(heights.data(), heights.count());
    if (index.set_heights(row_index(row), values)) return Status::success();
    if (index.out_of_memory()) return failed(ErrorKind::out_of_memory, "stxlib.row_height_index.set_heights");
    // Refused (unchanged): the range, a height, or the total.
    const u64 rows = count();
    const u64 wanted = static_cast<u64>(heights.count());
    if (row >= rows || wanted > rows - row) {
        return failed(ErrorKind::invalid_argument, "stxlib.row_height_index.set_heights");
    }
    for (usize position = 0; position < heights.count(); ++position) {
        const i64 height = heights[position];
        if (height < 0 || height > height_limit) {
            return failed(ErrorKind::invalid_argument, "stxlib.row_height_index.set_heights");
        }
    }
    return failed(ErrorKind::limit_exceeded, "stxlib.row_height_index.set_heights");
}

Status RowHeightIndex::insert(const u64 row, const u64 rows) noexcept {
    Index& index = index_of(tree_);
    if (index.insert(row_index(row), static_cast<stx_btree::RowCount>(rows))) return Status::success();
    return limit_or_memory(index, "stxlib.row_height_index.insert");
}

Status RowHeightIndex::erase(const u64 row, const u64 rows) noexcept {
    Index& index = index_of(tree_);
    if (index.erase(row_index(row), static_cast<stx_btree::RowCount>(rows))) return Status::success();
    return failed(ErrorKind::out_of_memory, "stxlib.row_height_index.erase");
}

Status RowHeightIndex::forget(const u64 row, const u64 rows) noexcept {
    Index& index = index_of(tree_);
    if (index.forget(row_index(row), static_cast<stx_btree::RowCount>(rows))) return Status::success();
    return limit_or_memory(index, "stxlib.row_height_index.forget");
}

Status RowHeightIndex::coarsen(const u64 row, const u64 rows) noexcept {
    Index& index = index_of(tree_);
    if (index.coarsen(row_index(row), static_cast<stx_btree::RowCount>(rows))) return Status::success();
    return failed(ErrorKind::out_of_memory, "stxlib.row_height_index.coarsen");
}

// ---- cursors ----------------------------------------------------------------

void RowHeightIndex::seek_row(Cursor& cursor, const u64 row) const noexcept {
    TreeCursor& path = *std::launder(reinterpret_cast<TreeCursor*>(cursor.path_));
    index_of(tree_).seek_row(path, row_index(row));
    settle(cursor);
}

void RowHeightIndex::seek_offset(Cursor& cursor, const i64 offset) const noexcept {
    TreeCursor& path = *std::launder(reinterpret_cast<TreeCursor*>(cursor.path_));
    index_of(tree_).seek_offset(path, units(offset));
    settle(cursor);
}

void RowHeightIndex::seek_offset_from(Cursor& cursor, const i64 offset) const noexcept {
    TreeCursor& path = *std::launder(reinterpret_cast<TreeCursor*>(cursor.path_));
    index_of(tree_).seek_offset_from(path, units(offset));
    settle(cursor);
}

void RowHeightIndex::advance(Cursor& cursor) const noexcept {
    TreeCursor& path = *std::launder(reinterpret_cast<TreeCursor*>(cursor.path_));
    index_of(tree_).advance(path);
    settle(cursor);
}

// ---- storage ----------------------------------------------------------------

usize RowHeightIndex::node_count() const noexcept {
    return static_cast<usize>(index_of(tree_).node_count());
}

usize RowHeightIndex::run_count() const noexcept {
    return static_cast<usize>(index_of(tree_).run_count());
}

usize RowHeightIndex::depth() const noexcept {
    return static_cast<usize>(index_of(tree_).depth());
}

usize RowHeightIndex::storage_bytes() const noexcept {
    return static_cast<usize>(sizeof(Index) + index_of(tree_).page_bytes());
}

TextView RowHeightIndex::invariant_failure() const noexcept {
    const stx_btree::detail::InvariantReport report = index_of(tree_).check_invariants();
    if (report.ok) return TextView{};
    // The tree's failures are ASCII string literals.
    return TextView::adopt_validated(report.failure, std::strlen(report.failure));
}

} // namespace stxlib
