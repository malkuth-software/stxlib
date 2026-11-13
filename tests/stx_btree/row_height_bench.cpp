// Facade against template: the cost of the compiled facade
// (include/stxlib/row_height_index.hpp) over the private template it wraps
// (src/algorithms/stx_btree/row_height_index.hpp, RowHeightIndex<>), on
// the three operations a virtualized view leans on:
// - row_to_offset: offset_of at random rows;
// - offset_to_row: row_at at random offsets;
// - set_heights: windows of 48 measured heights at random rows.
// Both indexes start from the same rows (a million estimated rows, then
// every row measured, so leaves are dense; the depth is printed) and take
// the same edits, so every answer must agree; a mismatch
// exits 1. Each operation is timed over `rounds` rounds, alternating which
// side runs first, and the best round of each side is reported:
//
//   <operation> template <ns> ns facade <ns> ns ratio <facade / template>
//
// For the integrator (sbi), not ctest: a measurement, not a check; no
// ceiling is applied. Options: --rows N (1000000), --queries Q (1000000),
// --rounds R (5).

#include "algorithms/stx_btree/row_height_index.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/row_height_index.hpp"

#include "btree_test_support.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>

namespace {

namespace stx_btree = stxlib::stx_btree;

using Template = stx_btree::RowHeightIndex<>;
using Clock = std::chrono::steady_clock;
using stx_btree::HeightUnits;
using stx_btree::RowIndex;
using stx_btree_tests::SplitMix64;

constexpr std::size_t window_rows = 48;

struct Options final {
    std::uint64_t rows{1000000};
    std::size_t queries{1000000};
    std::size_t rounds{5};
};

struct Workload final {
    std::vector<std::uint64_t> rows;
    std::vector<std::int64_t> offsets;
    // Window starts and their heights, window_rows per start.
    std::vector<std::uint64_t> window_starts;
    // STXlib's i64, which the template also takes as plain units.
    std::vector<stxlib::i64> window_heights;
};

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "row_height_bench: %s\n", what);
    std::exit(1);
}

[[nodiscard]] double nanoseconds_per(const Clock::duration elapsed, const std::size_t operations) {
    const double total = std::chrono::duration<double, std::nano>(elapsed).count();
    return total / static_cast<double>(operations == 0 ? 1 : operations);
}

// The template on STXlib's allocator, as the facade holds it.
class TemplateHolder final {
public:
    TemplateHolder() {
        storage_ = stxlib::memory::allocate(sizeof(Template), alignof(Template));
        if (storage_ == nullptr) fail("no memory for the template index");
        index_ = ::new (storage_) Template(HeightUnits{5728});
        if (!(*index_).constructed()) fail("the template index has no first page");
    }
    TemplateHolder(const TemplateHolder&) = delete;
    TemplateHolder& operator=(const TemplateHolder&) = delete;
    ~TemplateHolder() {
        (*index_).~Template();
        stxlib::memory::release(storage_);
    }
    [[nodiscard]] Template& index() noexcept { return *index_; }

private:
    void* storage_{nullptr};
    Template* index_{nullptr};
};

void prepare(Template& template_index, stxlib::RowHeightIndex& facade, const Options& options, Workload& work) {
    if (!template_index.assign(options.rows, HeightUnits{5728})) fail("template assign");
    if (!facade.assign(options.rows, 5728).succeeded()) fail("facade assign");
    SplitMix64 random(0x0b7e'e2026'0001ULL);
    std::vector<stxlib::i64> heights(window_rows);
    for (std::uint64_t row = 0; row < options.rows; row += window_rows) {
        const std::uint64_t left = options.rows - row;
        const std::size_t length = static_cast<std::size_t>(left < window_rows ? left : window_rows);
        for (std::size_t index = 0; index < length; index += 1) {
            heights[index] = 64 * (20 + static_cast<stxlib::i64>(random.below(40)));
        }
        const std::span<const stxlib::i64> window(heights.data(), length);
        if (!template_index.set_heights(RowIndex{row}, window)) fail("template measure");
        const stxlib::View<stxlib::i64> view(heights.data(), length);
        if (!facade.set_heights(row, view).succeeded()) fail("facade measure");
    }
    if (template_index.total().value != facade.total()) fail("totals differ after measuring");
    const std::int64_t total = template_index.total().value;
    work.rows.resize(options.queries);
    work.offsets.resize(options.queries);
    for (std::size_t index = 0; index < options.queries; index += 1) {
        work.rows[index] = random.below(options.rows);
        work.offsets[index] = static_cast<std::int64_t>(random.below(static_cast<std::uint64_t>(total)));
    }
    const std::size_t windows = options.queries / window_rows;
    work.window_starts.resize(windows);
    work.window_heights.resize(windows * window_rows);
    for (std::size_t index = 0; index < windows; index += 1) {
        work.window_starts[index] = random.below(options.rows - window_rows);
        for (std::size_t within = 0; within < window_rows; within += 1) {
            work.window_heights[index * window_rows + within] = 64 * (20 + static_cast<stxlib::i64>(random.below(40)));
        }
    }
}

struct Timing final {
    Clock::duration best{Clock::duration::max()};
    std::uint64_t checksum{0};
};

void time_row_to_offset_template(const Template& index, const Workload& work, Timing& timing) {
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (const std::uint64_t row : work.rows) sum += static_cast<std::uint64_t>(index.offset_of(RowIndex{row}).value);
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = sum;
}

void time_row_to_offset_facade(const stxlib::RowHeightIndex& index, const Workload& work, Timing& timing) {
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (const std::uint64_t row : work.rows) sum += static_cast<std::uint64_t>(index.offset_of(row));
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = sum;
}

void time_offset_to_row_template(const Template& index, const Workload& work, Timing& timing) {
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (const std::int64_t offset : work.offsets) sum += index.row_at(HeightUnits{offset}).value;
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = sum;
}

void time_offset_to_row_facade(const stxlib::RowHeightIndex& index, const Workload& work, Timing& timing) {
    std::uint64_t sum = 0;
    const Clock::time_point start = Clock::now();
    for (const std::int64_t offset : work.offsets) sum += index.row_at(static_cast<stxlib::i64>(offset));
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = sum;
}

void time_set_heights_template(Template& index, const Workload& work, Timing& timing) {
    const Clock::time_point start = Clock::now();
    for (std::size_t window = 0; window < work.window_starts.size(); window += 1) {
        const std::span<const stxlib::i64> heights(&work.window_heights[window * window_rows], window_rows);
        if (!index.set_heights(RowIndex{work.window_starts[window]}, heights)) fail("template set_heights");
    }
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = static_cast<std::uint64_t>(index.total().value);
}

void time_set_heights_facade(stxlib::RowHeightIndex& index, const Workload& work, Timing& timing) {
    const Clock::time_point start = Clock::now();
    for (std::size_t window = 0; window < work.window_starts.size(); window += 1) {
        const stxlib::View<stxlib::i64> heights(&work.window_heights[window * window_rows], window_rows);
        if (!index.set_heights(work.window_starts[window], heights).succeeded()) fail("facade set_heights");
    }
    const Clock::duration elapsed = Clock::now() - start;
    if (elapsed < timing.best) timing.best = elapsed;
    timing.checksum = static_cast<std::uint64_t>(index.total());
}

void report(const char* operation, const Timing& template_timing, const Timing& facade_timing,
            const std::size_t operations) {
    if (template_timing.checksum != facade_timing.checksum) fail("the facade's answers differ from the template's");
    const double template_ns = nanoseconds_per(template_timing.best, operations);
    const double facade_ns = nanoseconds_per(facade_timing.best, operations);
    std::printf("%s template %.2f ns facade %.2f ns ratio %.3f\n", operation, template_ns, facade_ns,
                facade_ns / template_ns);
}

[[nodiscard]] bool parse_count(const char* text, std::uint64_t& value) {
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0) return false;
    value = static_cast<std::uint64_t>(parsed);
    return true;
}

} // namespace

int main(int argument_count, char** arguments) {
    Options options{};
    for (int index = 1; index + 1 < argument_count; index += 2) {
        std::uint64_t value = 0;
        if (!parse_count(arguments[index + 1], value)) fail("a count must be a positive integer");
        if (std::strcmp(arguments[index], "--rows") == 0) {
            options.rows = value;
        } else if (std::strcmp(arguments[index], "--queries") == 0) {
            options.queries = static_cast<std::size_t>(value);
        } else if (std::strcmp(arguments[index], "--rounds") == 0) {
            options.rounds = static_cast<std::size_t>(value);
        } else {
            fail("usage: stxlib_row_height_bench [--rows N] [--queries Q] [--rounds R]");
        }
    }
    if (options.rows <= window_rows) fail("--rows must exceed the window of 48 rows");

    TemplateHolder holder{};
    Template& template_index = holder.index();
    stxlib::Result<stxlib::RowHeightIndex> made = stxlib::RowHeightIndex::create(5728);
    if (!made.succeeded()) fail("no memory for the facade index");
    stxlib::RowHeightIndex facade = static_cast<stxlib::Result<stxlib::RowHeightIndex>&&>(made).take_value();
    Workload work{};
    prepare(template_index, facade, options, work);
    std::printf("rows %llu, queries %zu, rounds %zu, depth %zu\n", static_cast<unsigned long long>(options.rows),
                options.queries, options.rounds, facade.depth());

    Timing template_timing{};
    Timing facade_timing{};
    for (std::size_t round = 0; round < options.rounds; round += 1) {
        if (round % 2 == 0) {
            time_row_to_offset_template(template_index, work, template_timing);
            time_row_to_offset_facade(facade, work, facade_timing);
        } else {
            time_row_to_offset_facade(facade, work, facade_timing);
            time_row_to_offset_template(template_index, work, template_timing);
        }
    }
    report("row_to_offset", template_timing, facade_timing, work.rows.size());

    template_timing = Timing{};
    facade_timing = Timing{};
    for (std::size_t round = 0; round < options.rounds; round += 1) {
        if (round % 2 == 0) {
            time_offset_to_row_template(template_index, work, template_timing);
            time_offset_to_row_facade(facade, work, facade_timing);
        } else {
            time_offset_to_row_facade(facade, work, facade_timing);
            time_offset_to_row_template(template_index, work, template_timing);
        }
    }
    report("offset_to_row", template_timing, facade_timing, work.offsets.size());

    // Both take the same windows each round, so their totals stay equal.
    template_timing = Timing{};
    facade_timing = Timing{};
    for (std::size_t round = 0; round < options.rounds; round += 1) {
        if (round % 2 == 0) {
            time_set_heights_template(template_index, work, template_timing);
            time_set_heights_facade(facade, work, facade_timing);
        } else {
            time_set_heights_facade(facade, work, facade_timing);
            time_set_heights_template(template_index, work, template_timing);
        }
    }
    report("set_heights (per window of 48)", template_timing, facade_timing, work.window_starts.size());
    if (!template_index.check_invariants().ok || !facade.invariant_failure().empty()) fail("invariants after the run");
    return 0;
}
