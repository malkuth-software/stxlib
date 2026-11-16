// Row D05 (phase D, targeted): the compiled facade over stx_btree's
// sequence tree (src/algorithms/row_height_index.cpp, edits): the facade's
// own logic, which is the translation of each call and the classification
// of every refusal into the failure kind include/stxlib/row_height_index.hpp
// names.
// P1: for every argument and every behaviour the template's contract
//     allows (src/algorithms/stx_btree/row_height_index.hpp, lines 22-27: a
//     refused edit leaves the index unchanged; out_of_memory() tells a
//     page refusal from a limit), each edit's result is the header's:
//     - set_height: invalid_argument for a row past the end or a height
//       outside [0, height_limit], without calling the template; true when
//       the template accepts; false when the row already holds that
//       measured height; out_of_memory on a page refusal; limit_exceeded
//       otherwise;
//     - set_heights: invalid_argument when the range passes the end or a
//       height is outside [0, height_limit]; out_of_memory on a page
//       refusal; limit_exceeded otherwise; success when accepted;
//     - assign, insert, forget: limit_exceeded or out_of_memory as the
//       template's out_of_memory() says; erase and coarsen: out_of_memory.
// Environment model: the template is replaced by a model that accepts or
//     refuses nondeterministically within that contract and records why;
//     STXlib's vocabulary (OperationStatus, OperationResult, ErrorRecord, View) is reduced to
//     the outcome and its kind. The structure itself (every query and walk
//     equals a flat model, I1-I6) is the exhaustive test's
//     (tests/stx_btree/sequence_tree_exhaustive_test.cpp, REPORT batch 3c6).
// Bound: set_heights over at most N heights (N in {2, 3}).
#include "verify.h"

#include "stxlib/types.hpp"

#include <cstdint>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=e8c563f2e84124ab284680198c844b971d8f0e48a2c0bec2f9ee8cf400b67b7f

//@ model-begin reason="STXlib's vocabulary reduced to the outcome and its kind; the template replaced by a nondeterministic model of its refusal contract"
#define STXLIB_REQUIRE(condition, message) STX_ASSERT(condition, message)
namespace stxlib {

enum class ErrorKind : u8 { none = 0, invalid_argument = 1, limit_exceeded = 2, out_of_memory = 3 };
using Operation = const char*;

struct ErrorRecord final {
    ErrorKind kind{ErrorKind::none};
    static ErrorRecord make(const ErrorKind kind, Operation) noexcept {
        const ErrorRecord record{kind};
        return record;
    }
};

struct OperationStatus final {
    bool ok{false};
    ErrorKind kind{ErrorKind::none};
    static OperationStatus success() noexcept { return OperationStatus{true, ErrorKind::none}; }
    static OperationStatus failure(const ErrorRecord record) noexcept { return OperationStatus{false, record.kind}; }
};

template <typename T>
struct OperationResult final {
    bool ok{false};
    T value{};
    ErrorKind kind{ErrorKind::none};
    static OperationResult success(const T value) noexcept { return OperationResult{true, value, ErrorKind::none}; }
    static OperationResult failure(const ErrorRecord record) noexcept {
        return OperationResult{false, T{}, record.kind};
    }
};

template <typename T>
struct View final {
    const T* data_;
    usize count_;
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] const T& operator[](const usize index) const noexcept { return data_[index]; }
};

namespace stx_btree {

using RowCount = std::uint64_t;
struct RowIndex final {
    std::uint64_t value{};
};
struct HeightUnits final {
    std::int64_t value{};
};
enum class RowState : u8 { estimated = 0, measured = 1, approximate = 2 };
struct RowExtent final {
    HeightUnits top{};
    HeightUnits height{};
    RowState state{RowState::estimated};
};

constexpr std::int64_t height_limit = (static_cast<std::int64_t>(1) << 38) - 1;

// Why the model refused or accepted the last edit.
enum class Reason : u8 { none, accepted, invalid, same, limit, memory };

class Model final {
public:
    std::uint64_t rows{0};
    Reason last{Reason::none};
    int calls{0};
    RowExtent queried{};  // what extent_of answers (fixed per run, nondeterministic)

    [[nodiscard]] bool out_of_memory() const noexcept { return last == Reason::memory; }
    [[nodiscard]] RowExtent extent_of(RowIndex) const noexcept { return queried; }

    bool set_height(const RowIndex row, const HeightUnits height) noexcept {
        calls = calls + 1;
        if (row.value >= rows || height.value < 0 || height.value > height_limit) return refuse(Reason::invalid);
        // Already measured at this height: refused, unchanged (no page is needed).
        if (queried.state == RowState::measured && queried.height.value == height.value) return refuse(Reason::same);
        return choose(true);
    }
    template <typename Height>
    bool set_heights(const RowIndex row, const Height* const heights, const usize count) noexcept {
        calls = calls + 1;
        if (row.value >= rows || count > rows - row.value) return refuse(Reason::invalid);
        for (usize index = 0; index < count; ++index) {
            if (heights[index] < 0 || heights[index] > height_limit) return refuse(Reason::invalid);
        }
        return choose(true);
    }
    bool limited_edit() noexcept {
        calls = calls + 1;
        return choose(true);
    }
    bool memory_edit() noexcept {
        calls = calls + 1;
        return choose(false);
    }

private:
    bool refuse(const Reason reason) noexcept {
        last = reason;
        return false;
    }
    // Accept, or refuse for memory, or (when the edit has a limit) for it.
    bool choose(const bool limited) noexcept {
        const unsigned pick = nondet_uint();
        if (pick == 0) {
            last = Reason::accepted;
            return true;
        }
        if (pick == 1 || !limited) return refuse(Reason::memory);
        return refuse(Reason::limit);
    }
};

// The template's calls, as the facade makes them.
class Template final {
public:
    Model model;
    [[nodiscard]] bool out_of_memory() const noexcept { return model.out_of_memory(); }
    [[nodiscard]] RowExtent extent_of(const RowIndex row) const noexcept { return model.extent_of(row); }
    bool set_height(const RowIndex row, const HeightUnits height) noexcept { return model.set_height(row, height); }
    template <typename Span>
    bool set_heights(const RowIndex row, const Span heights) noexcept {
        return model.set_heights(row, heights.data(), heights.size());
    }
    bool assign(RowCount, HeightUnits) noexcept { return model.limited_edit(); }
    bool insert(RowIndex, RowCount) noexcept { return model.limited_edit(); }
    bool erase(RowIndex, RowCount) noexcept { return model.memory_edit(); }
    bool forget(RowIndex, RowCount) noexcept { return model.limited_edit(); }
    bool coarsen(RowIndex, RowCount) noexcept { return model.memory_edit(); }
};

} // namespace stx_btree

} // namespace stxlib

namespace stx_model {
template <typename T>
struct Span final {
    const T* pointer;
    stxlib::usize length;
    Span(const T* const data, const stxlib::usize count) noexcept : pointer(data), length(count) {}
    [[nodiscard]] const T* data() const noexcept { return pointer; }
    [[nodiscard]] stxlib::usize size() const noexcept { return length; }
};
} // namespace stx_model

namespace stxlib {

using Index = stx_btree::Template;
using stx_btree::HeightUnits;
using stx_btree::RowIndex;

class RowHeightIndex final {
public:
    static constexpr i64 height_limit = stx_btree::height_limit;
    explicit RowHeightIndex(void* const tree) noexcept : tree_(tree) {}
    [[nodiscard]] u64 count() const noexcept { return static_cast<Index*>(tree_)->model.rows; }
    [[nodiscard]] OperationStatus assign(u64 rows, i64 estimate) noexcept;
    [[nodiscard]] OperationResult<bool> set_height(u64 row, i64 height) noexcept;
    [[nodiscard]] OperationStatus set_heights(u64 row, View<i64> heights) noexcept;
    [[nodiscard]] OperationStatus insert(u64 row, u64 rows) noexcept;
    [[nodiscard]] OperationStatus erase(u64 row, u64 rows) noexcept;
    [[nodiscard]] OperationStatus forget(u64 row, u64 rows) noexcept;
    [[nodiscard]] OperationStatus coarsen(u64 row, u64 rows) noexcept;

private:
    void* tree_{nullptr};
};

} // namespace stxlib
//@ model-end

//@ lift source=stxlib path=src/algorithms/row_height_index.cpp lines=43-46 namespace=stxlib anonymous sha256=7806ef4b58f2cd8a45f6ed0174407d57f1035ab5cf4f9d597a2e3a604f0142d8
//@ lift source=stxlib path=src/algorithms/row_height_index.cpp lines=58-77 namespace=stxlib anonymous sha256=de8841b98d83cf14d179cadc27903e8114d53950fdeebf8c5e37267b20ac25cd
//@ lift source=stxlib path=src/algorithms/row_height_index.cpp lines=178-245 namespace=stxlib sha256=6bf1a01a4f389dc94bb539e4881e2b8af1c74950248b31356e806e6fa7df2d30
//@ subst source=stxlib path=src/algorithms/row_height_index.cpp line=205 from="std::span<const i64>" to="stx_model::Span<const i64>" reason="std::span reduced to its data and size"
//@ mutant id=m1 source=stxlib path=src/algorithms/row_height_index.cpp line=186 from="height < 0" to="height < -1"

#ifndef STX_N
#define STX_N 2
#endif

using stxlib::ErrorKind;
using stxlib::i64;
using stxlib::u64;
using stxlib::usize;
using stxlib::stx_btree::Reason;

namespace {

bool valid_height(const i64 height) {
    return height >= 0 && height <= stxlib::RowHeightIndex::height_limit;
}

void check_limited(const stxlib::OperationStatus status, const stxlib::stx_btree::Model& model) {
    if (model.last == Reason::accepted) STX_ASSERT(status.ok, "an accepted edit succeeds");
    if (model.last == Reason::memory) STX_ASSERT(!status.ok && status.kind == ErrorKind::out_of_memory, "page refusal");
    if (model.last == Reason::limit) STX_ASSERT(!status.ok && status.kind == ErrorKind::limit_exceeded, "limit refusal");
}

} // namespace

int main() {
    stxlib::stx_btree::Template tree;
    tree.model.rows = nondet_ulonglong();
    tree.model.queried.state = static_cast<stxlib::stx_btree::RowState>(nondet_uchar() % 3);
    tree.model.queried.height.value = static_cast<i64>(nondet_longlong());
    stxlib::RowHeightIndex index(&tree);
    const u64 row = nondet_ulonglong();
    const i64 height = static_cast<i64>(nondet_longlong());

    // set_height
    const stxlib::OperationResult<bool> set = index.set_height(row, height);
    if (row >= tree.model.rows || !valid_height(height)) {
        STX_ASSERT(!set.ok && set.kind == ErrorKind::invalid_argument, "set_height: invalid argument");
        STX_ASSERT(tree.model.calls == 0, "set_height: the template is not called with invalid arguments");
    } else if (tree.model.last == Reason::accepted) {
        STX_ASSERT(set.ok && set.value, "set_height: true when the height or state changed");
    } else if (tree.model.last == Reason::same) {
        STX_ASSERT(set.ok && !set.value, "set_height: false when already measured at that height");
    } else if (tree.model.last == Reason::memory) {
        STX_ASSERT(!set.ok && set.kind == ErrorKind::out_of_memory, "set_height: out of memory");
    } else {
        STX_ASSERT(tree.model.last == Reason::limit, "set_height: the template refused for its limit");
        STX_ASSERT(!set.ok && set.kind == ErrorKind::limit_exceeded, "set_height: limit exceeded");
    }

    // set_heights
    i64 heights[STX_N];
    for (usize position = 0; position < STX_N; ++position) heights[position] = static_cast<i64>(nondet_longlong());
    const usize wanted = nondet_size();
    STX_ASSUME(wanted <= STX_N);
    const u64 first = nondet_ulonglong();
    const stxlib::OperationStatus many = index.set_heights(first, stxlib::View<i64>{heights, wanted});
    bool every_valid = true;
    for (usize position = 0; position < wanted; ++position) every_valid = every_valid && valid_height(heights[position]);
    const bool in_range = first < tree.model.rows && wanted <= tree.model.rows - first;
    if (!in_range || !every_valid) {
        STX_ASSERT(!many.ok && many.kind == ErrorKind::invalid_argument, "set_heights: invalid argument");
    } else {
        check_limited(many, tree.model);
    }

    // The edits whose refusals are a limit or memory, and memory only.
    const u64 count = nondet_ulonglong();
    check_limited(index.assign(count, height), tree.model);
    check_limited(index.insert(row, count), tree.model);
    check_limited(index.forget(row, count), tree.model);
    const stxlib::OperationStatus erased = index.erase(row, count);
    STX_ASSERT(tree.model.last != Reason::limit, "erase has no limit");
    check_limited(erased, tree.model);
    const stxlib::OperationStatus coarsened = index.coarsen(row, count);
    check_limited(coarsened, tree.model);
    STX_VACUITY_PROBE();
    return 0;
}
