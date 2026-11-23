// The PSFT's tables (plan_state.hpp, WalkTables; the kernels are walk.inc):
// the twiddle table, the passes and their cells, the schedule, the gaps and
// the strips, built once per plan in scalar code. Ported from the PSFT's
// prototype (~/Developer/work/fft/ideal walk.hpp, Plan; 2026-11-23): the same
// passes, cells, schedule keys and gap rule, so the same values in the same
// order; the prototype's growable arrays, lambdas and stable sort are
// counted blocks, named functions and a sort on a total order here.
//
// Addresses. After t stages, row kappa of the state (its run of N / 2^t
// reals) starts at row(t, kappa): each bit of kappa below t goes to address
// bit m - 1 - bit while bit <= m - 1 - t (a plain stage's place), else to
// address bit `bit` (a crossed stage has exchanged it into its final place).
//
// Gaps. Runs a power of two apart share cache sets. The work buffers carry
// one cache line of gap after every stretch as long as one first-level way,
// and one more after every (way x sets); a logical address k is at
// place(k) = k + (k >> shift) gap + (k >> shift2) gap2. An address made of
// parts with no bits in common maps to the sum of the parts' places, so
// bases, quarter offsets and far offsets are mapped separately. The caller's
// arrays have no gaps.
//
// Schedule. Plain passes run parents before children, by block start;
// crossed passes children before parents, by block end. Blocks no larger
// than the granule are grouped per granule and run pass by pass inside it.
// The order moves independent cells only, so it never changes a value.

#include "plan_state.hpp"

#include "hosted/cache_backend.hpp"
#include "stxlib/math.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/types.hpp"

#include <algorithm>
#include <bit>

namespace stxlib::fft::detail {

namespace {

// The straight-line kernels' largest sizes (walk_small_f64.inc, _f32.inc).
constexpr i32 straight_line_limit64 = 128;
constexpr i32 straight_line_limit32 = 256;

// The second work buffer's displacement from the first, in bytes beyond a
// whole number of cache lines (the prototype's measured 2112), so the two
// buffers' streams do not share cache sets.
constexpr u32 stagger_bytes = 2112;

// Slack after the second buffer (the prototype's).
constexpr usize work_slack = 16;

// The most passes: m <= 30 stages, at least one stage each.
constexpr u32 pass_capacity = 32;

// A temporary block of `count` elements, released when the owner ends.
template <typename Element>
class Scratch final {
public:
    explicit Scratch(const usize wanted) noexcept {
        if (wanted == 0 || wanted > usize_max / sizeof(Element)) return;
        elements_ = static_cast<Element*>(memory::allocate(wanted * sizeof(Element), alignof(Element)));
        count_ = elements_ == nullptr ? 0 : wanted;
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    ~Scratch() {
        Element* const held = elements_;
        elements_ = nullptr;
        if (held != nullptr) memory::release(held);
    }
    [[nodiscard]] bool ready() const noexcept { return elements_ != nullptr; }
    [[nodiscard]] Element* data() const noexcept { return elements_; }
    [[nodiscard]] usize count() const noexcept { return count_; }

private:
    Element* elements_{nullptr};
    usize count_{0};
};

// The layout of one plan's work buffers.
struct Layout final {
    u32 shift{31};     // log2 of one gap-free stretch (a cache way), in reals
    u32 shift2{31};    // log2 of (way x sets), in reals
    u32 gap{0};        // reals per gap (one line), 0 without gaps
    u32 granule{0};    // 0: every pass one sweep
    usize stagger{0};  // reals
};

[[nodiscard]] u32 ceiling_log2(const usize value) noexcept {
    u32 bits = 0;
    while (bits < 40 && (usize{1} << bits) < value) bits = bits + 1;
    return bits;
}

// Clamps a host-reported size into [low, high].
[[nodiscard]] usize clamped(const usize value, const usize low, const usize high) noexcept {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

[[nodiscard]] Layout layout_for(const WalkOptions& options, const usize element) noexcept {
    cache_backend::DataCaches caches;
    cache_backend::read_data_caches(caches);
    const usize level1 = clamped(caches.level1, 4096, usize{1} << 26);
    const usize ways = clamped(caches.ways, 1, 64);
    const usize line = clamped(std::bit_floor(clamped(caches.line, 16, 1024)), 16, 1024);
    Layout layout;
    // Granule: the largest power-of-two block, in reals, whose working set
    // (a block and its mirror, in two buffers) stays under three quarters of
    // the first-level cache; at least 64.
    if (options.granule == walk_granule_automatic) {
        u32 granule = 64;
        while (8 * static_cast<usize>(granule) * element <= level1 * 3 / 4 && granule < (u32{1} << 30)) {
            granule = granule * 2;
        }
        layout.granule = granule;
    } else {
        layout.granule = options.granule;
    }
    const usize way = clamped(level1 / ways / element, 1, usize{1} << 26);
    const usize sets = clamped(level1 / ways / line, 1, usize{1} << 20);
    layout.shift = ceiling_log2(way);
    layout.shift2 = ceiling_log2(way * sets);
    if (layout.shift2 < layout.shift) layout.shift2 = layout.shift;
    layout.gap = options.gaps ? static_cast<u32>(line / element) : 0;
    layout.stagger = static_cast<usize>(stagger_bytes & ~63U) / element;
    return layout;
}

// row(t, kappa): see "Addresses".
[[nodiscard]] u32 row_address(const u32 m, const u32 t, const u32 kappa) noexcept {
    const u32 s = m - 1 - t;
    u32 address = 0;
    for (u32 bit = 0; bit < t; ++bit) {
        if (((kappa >> bit) & 1U) == 0) continue;
        const u32 place = bit <= s ? (m - 1 - bit) : bit;
        address = address | (u32{1} << place);
    }
    return address;
}

// A pass's shape before its cells are placed.
struct PassShape final {
    u32 radix{4};
    u32 t{0};
    bool crossed{false};
    WalkBuffer source{WalkBuffer::caller};
    WalkBuffer destination{WalkBuffer::first};
};

// The passes for 2^m (the prototype's build): the plain stages 0 .. m - h - 1
// (h = m / 2) begin with one radix-2 pass when their count is odd, then
// radix-4 passes; the crossed stages up to m - 3 likewise; the terminal
// covers the last two stages. The first pass reads the caller's array and
// writes the first buffer; later plain passes work in place, crossed passes
// into the other buffer. Returns the pass count; `terminal_t` and
// `terminal_source` describe the terminal.
void add_pass(PassShape (&shapes)[pass_capacity], u32& count, u32& t, WalkBuffer& current, const u32 radix,
              const bool crossed) noexcept {
    PassShape& shape = shapes[count];
    shape.radix = radix;
    shape.t = t;
    shape.crossed = crossed;
    shape.source = current;
    if (current == WalkBuffer::caller) {
        shape.destination = WalkBuffer::first;
    } else if (crossed) {
        shape.destination = current == WalkBuffer::first ? WalkBuffer::second : WalkBuffer::first;
    } else {
        shape.destination = current;
    }
    current = shape.destination;
    count = count + 1;
    t = t + (radix == 4 ? 2 : 1);
}

[[nodiscard]] u32 pass_shapes(const u32 m, PassShape (&shapes)[pass_capacity], u32& terminal_t,
                              WalkBuffer& terminal_source) noexcept {
    const u32 h = m / 2;
    const u32 plain = m - h;
    u32 t = 0;
    u32 count = 0;
    WalkBuffer current = WalkBuffer::caller;
    if ((plain & 1U) != 0) add_pass(shapes, count, t, current, 2, false);
    while (t < plain) add_pass(shapes, count, t, current, 4, false);
    if ((h & 1U) != 0) add_pass(shapes, count, t, current, 2, true);
    while (t < m - 2) add_pass(shapes, count, t, current, 4, true);
    terminal_t = t;
    terminal_source = current;
    return count;
}

// A cell as built, with its twiddle index (the cell's smaller angle is
// 2 pi tw / N); placed and stripped later.
struct BuiltCell final {
    u32 in_re{0};
    u32 in_im{0};
    u32 out[4]{};
    u32 tw{0};
};

// Orders cells by their real run's address (unique within a pass).
struct ByRealRun final {
    [[nodiscard]] bool operator()(const BuiltCell& a, const BuiltCell& b) const noexcept { return a.in_re < b.in_re; }
};

// Orders cells by the first place they write (unique within a pass): a
// crossed pass runs its cells in write order ("scattered reads, ordered
// writes"; the prototype's fill), a plain pass writes where it reads.
struct ByFirstWrite final {
    [[nodiscard]] bool operator()(const BuiltCell& a, const BuiltCell& b) const noexcept { return a.out[0] < b.out[0]; }
};

// One unit of work for the schedule: a pass's row 0 (part 0), ridge
// (part 1) or one cell (part 2, index idx). seq is the order of creation,
// the last key, so the sort is a total order and equals the prototype's
// stable sort.
struct Op final {
    u64 k1{0};
    u32 k2{0};
    u32 part{0};
    u32 addr{0};
    u32 pass{0};
    u32 idx{0};
    u32 seq{0};
};

struct ScheduleOrder final {
    [[nodiscard]] bool operator()(const Op& a, const Op& b) const noexcept {
        if (a.k1 != b.k1) return a.k1 < b.k1;
        if (a.k2 != b.k2) return a.k2 < b.k2;
        if (a.part != b.part) return a.part < b.part;
        if (a.addr != b.addr) return a.addr < b.addr;
        return a.seq < b.seq;
    }
};

// Everything one pass needs while it is built.
struct PassBuild final {
    PassShape shape{};
    u32 chunk{0};
    u32 offsets[4]{};
    u32 dc_in{0};
    u32 dc_out[4]{};
    bool ridge{false};
    u32 ridge_in{0};
    u32 ridge_out[4]{};
    BuiltCell* cells{nullptr};   // into the cell scratch
    u32 cell_count{0};
};

// The cell count of a pass at stage t: rows k = 1 .. 2^(t-1) - 1 (t >= 2).
[[nodiscard]] u32 cells_at(const u32 t) noexcept {
    if (t < 2) return 0;
    return (u32{1} << (t - 1)) - 1;
}

// Fills a pass's logical addresses and cells (the prototype's fill).
void fill_pass(PassBuild& p, const u32 m, const u32 n) noexcept {
    const u32 t = p.shape.t;
    if (p.shape.radix == 4) {
        p.chunk = n >> (t + 2);
        for (u32 c = 0; c < 4; ++c) p.offsets[c] = row_address(m, t + 2, c << t);
        p.dc_in = row_address(m, t, 0);
        p.dc_out[0] = row_address(m, t + 2, 0);
        p.dc_out[1] = row_address(m, t + 2, u32{1} << (t + 1));
        p.dc_out[2] = row_address(m, t + 2, u32{1} << t);
        p.dc_out[3] = row_address(m, t + 2, u32{3} << t);
        p.ridge = t >= 1;
        if (p.ridge) {
            const u32 k = u32{1} << (t - 1);
            p.ridge_in = row_address(m, t, k);
            p.ridge_out[0] = row_address(m, t + 2, k);
            p.ridge_out[1] = row_address(m, t + 2, 7 * k);
            p.ridge_out[2] = row_address(m, t + 2, 3 * k);
            p.ridge_out[3] = row_address(m, t + 2, 5 * k);
        }
        const u32 top = u32{1} << t;
        for (u32 k = 1; k < cells_at(t) + 1; ++k) {
            BuiltCell& cell = p.cells[k - 1];
            cell.in_re = row_address(m, t, k);
            cell.in_im = row_address(m, t, top - k);
            cell.out[0] = row_address(m, t + 2, k);
            cell.out[1] = row_address(m, t + 2, top - k);
            cell.tw = k * (n >> (t + 2));
        }
    } else {
        p.chunk = n >> (t + 1);
        p.dc_in = row_address(m, t, 0);
        p.dc_out[0] = row_address(m, t + 1, 0);
        p.dc_out[1] = row_address(m, t + 1, u32{1} << t);
        p.ridge = t >= 1;
        if (p.ridge) {
            const u32 k = u32{1} << (t - 1);
            p.ridge_in = row_address(m, t, k);
            p.ridge_out[0] = row_address(m, t + 1, k);
            p.ridge_out[1] = row_address(m, t + 1, 3 * k);
        }
        const u32 top = u32{1} << t;
        for (u32 k = 1; k < cells_at(t) + 1; ++k) {
            BuiltCell& cell = p.cells[k - 1];
            cell.in_re = row_address(m, t, k);
            cell.in_im = row_address(m, t, top - k);
            cell.out[0] = row_address(m, t + 1, k);
            cell.out[1] = row_address(m, t + 1, 2 * top - k);
            cell.out[2] = row_address(m, t + 1, top - k);
            cell.out[3] = row_address(m, t + 1, top + k);
            cell.tw = k * (n >> (t + 1));
        }
    }
    p.cell_count = cells_at(t);
    if (p.shape.crossed) {
        std::sort(p.cells, p.cells + p.cell_count, ByFirstWrite{});
    } else {
        std::sort(p.cells, p.cells + p.cell_count, ByRealRun{});
    }
}

// The schedule key of one unit whose representative address is rep.
[[nodiscard]] Op key_of(const PassBuild& p, const u32 pass, const u32 rep, const u32 granule) noexcept {
    constexpr u64 big = u64{1} << 40;
    const u32 stages = p.shape.radix == 4 ? 2 : 1;
    const u32 run = p.chunk * (p.shape.radix == 4 ? 4 : 2);
    const u32 block = p.shape.crossed ? (u32{1} << (p.shape.t + stages)) : run;
    const bool small = granule != 0 && block <= granule;
    Op op;
    op.k2 = pass;
    op.pass = pass;
    op.addr = rep;
    if (granule == 0) {
        op.k1 = p.shape.crossed ? big : 0;
    } else if (!p.shape.crossed) {
        op.k1 = small ? static_cast<u64>(rep / granule) * granule : rep;
    } else {
        const u64 end = small ? (static_cast<u64>(rep / granule) + 1) * granule
                              : (static_cast<u64>(rep / block) + 1) * block;
        op.k1 = big + end;
    }
    return op;
}

template <typename T>
[[nodiscard]] T cosine_at(const T* const table, const u32 eighth, const u32 a) noexcept {
    const T result = a <= eighth ? table[2 * a] : table[2 * (2 * eighth - a) + 1];
    return result;
}

template <typename T>
[[nodiscard]] T sine_at(const T* const table, const u32 eighth, const u32 a) noexcept {
    const T result = a <= eighth ? table[2 * a + 1] : table[2 * (2 * eighth - a)];
    return result;
}

// The twiddle table: table[2a] = cos(2 pi a / N), table[2a + 1] =
// sin(2 pi a / N) as cos(2 pi (N/4 - a) / N), a = 0 .. N/8, from sincospi
// (the argument 2a / N in half-turns is exact) rounded to T.
template <typename T>
void fill_twiddles(T* const table, const i32 n) noexcept {
    const i32 eighth = n / 8;
    for (i32 a = 0; a <= eighth; ++a) {
        double sine = 0.0;
        double cosine = 0.0;
        math::sincospi(2.0 * (static_cast<double>(a) / static_cast<double>(n)), sine, cosine);
        double complement_sine = 0.0;
        double complement_cosine = 0.0;
        math::sincospi(2.0 * (static_cast<double>(n / 4 - a) / static_cast<double>(n)), complement_sine,
                       complement_cosine);
        table[2 * a] = static_cast<T>(cosine);
        table[2 * a + 1] = static_cast<T>(complement_cosine);
    }
}

// The pass's place functions (plan_state.hpp, walk_in_place).
[[nodiscard]] u32 place(const u32 k, const u32 shift, const u32 shift2, const u32 gap) noexcept {
    return k + (k >> shift) * gap + (k >> shift2) * gap;
}

template <typename T>
[[nodiscard]] bool build_walk_of(PlanState& state, const i32 n, const WalkOptions& options) noexcept {
    WalkTables& tables = state.walk;
    const u32 un = static_cast<u32>(n);
    const u32 m = static_cast<u32>(std::countr_zero(un));
    const u32 eighth = un / 8;
    tables.n = n;
    tables.eighth = eighth;

    T* const table = static_cast<T*>(take_plan_block(state, 2 * (static_cast<usize>(eighth) + 1), sizeof(T)));
    if (table == nullptr) return false;
    fill_twiddles<T>(table, n);
    if constexpr (sizeof(T) == sizeof(double)) {
        tables.twiddles64 = table;
    } else {
        tables.twiddles32 = table;
    }

    const i32 limit = sizeof(T) == sizeof(double) ? straight_line_limit64 : straight_line_limit32;
    tables.straight_line = options.straight_line && n <= limit;
    if (tables.straight_line) {
        tables.work_count = 0;
        return true;
    }

    // Passes and cells.
    PassShape shapes[pass_capacity];
    u32 terminal_t = 0;
    WalkBuffer terminal_source = WalkBuffer::first;
    const u32 pass_count = pass_shapes(m, shapes, terminal_t, terminal_source);
    usize total_cells = 0;
    for (u32 index = 0; index < pass_count; ++index) total_cells = total_cells + cells_at(shapes[index].t);
    Scratch<BuiltCell> built(total_cells == 0 ? 1 : total_cells);
    if (!built.ready()) return false;
    PassBuild passes[pass_capacity];
    usize cell_offset = 0;
    usize op_count = 0;
    for (u32 index = 0; index < pass_count; ++index) {
        PassBuild& p = passes[index];
        p.shape = shapes[index];
        p.cells = built.data() + cell_offset;
        fill_pass(p, m, un);
        cell_offset = cell_offset + p.cell_count;
        op_count = op_count + 1 + (p.ridge ? 1 : 0) + p.cell_count;
    }

    // Layout.
    const Layout layout = layout_for(options, sizeof(T));
    const usize elements = static_cast<usize>(un) + (static_cast<usize>(un) >> layout.shift) * layout.gap +
                           (static_cast<usize>(un) >> layout.shift2) * layout.gap;
    tables.elements = elements;
    tables.stagger = layout.stagger;
    tables.work_count = 2 * elements + layout.stagger + work_slack;

    // Schedule.
    Scratch<Op> ops(op_count);
    if (!ops.ready()) return false;
    u32 seq = 0;
    for (u32 index = 0; index < pass_count; ++index) {
        const PassBuild& p = passes[index];
        Op dc = key_of(p, index, p.dc_in, layout.granule);
        dc.part = 0;
        dc.seq = seq;
        ops.data()[seq] = dc;
        seq = seq + 1;
        if (p.ridge) {
            Op ridge = key_of(p, index, p.ridge_in, layout.granule);
            ridge.part = 1;
            ridge.seq = seq;
            ops.data()[seq] = ridge;
            seq = seq + 1;
        }
        for (u32 c = 0; c < p.cell_count; ++c) {
            const BuiltCell& cell = p.cells[c];
            const u32 rep = cell.in_re < cell.in_im ? cell.in_re : cell.in_im;
            Op op = key_of(p, index, rep, layout.granule);
            // Within a block a crossed pass's cells keep their write order.
            if (p.shape.crossed) op.addr = cell.out[0];
            op.part = 2;
            op.idx = c;
            op.seq = seq;
            ops.data()[seq] = op;
            seq = seq + 1;
        }
    }
    std::sort(ops.data(), ops.data() + op_count, ScheduleOrder{});

    // Segments: consecutive cells of one pass merge.
    Scratch<WalkSegment> segments(op_count);
    Scratch<u32> order(total_cells == 0 ? 1 : total_cells);   // scheduled position -> built cell
    if (!segments.ready() || !order.ready()) return false;
    u32 placed[pass_capacity]{};
    usize pass_first[pass_capacity]{};
    usize first_cell = 0;
    for (u32 index = 0; index < pass_count; ++index) {
        pass_first[index] = first_cell;
        first_cell = first_cell + passes[index].cell_count;
    }
    u32 segment_count = 0;
    for (usize index = 0; index < op_count; ++index) {
        const Op& op = ops.data()[index];
        if (op.part != 2) {
            const WalkPart part = op.part == 0 ? WalkPart::dc : WalkPart::ridge;
            segments.data()[segment_count] = WalkSegment{.pass = op.pass, .part = part, .first = 0, .count = 0};
            segment_count = segment_count + 1;
            continue;
        }
        const u32 at = placed[op.pass];
        order.data()[pass_first[op.pass] + at] = op.idx;
        placed[op.pass] = at + 1;
        if (segment_count > 0) {
            WalkSegment& last = segments.data()[segment_count - 1];
            if (last.pass == op.pass && last.part == WalkPart::cells && last.first + last.count == at) {
                last.count = last.count + 1;
                continue;
            }
        }
        segments.data()[segment_count] = WalkSegment{.pass = op.pass, .part = WalkPart::cells, .first = at, .count = 1};
        segment_count = segment_count + 1;
    }

    // The plan's blocks.
    WalkPass* const final_passes = static_cast<WalkPass*>(take_plan_block(state, pass_count, sizeof(WalkPass)));
    WalkSegment* const final_segments =
        static_cast<WalkSegment*>(take_plan_block(state, segment_count, sizeof(WalkSegment)));
    const u32 row_count = terminal_t >= 1 ? (u32{1} << (terminal_t - 1)) - 1 : 0;
    WalkRow* const rows =
        static_cast<WalkRow*>(take_plan_block(state, row_count == 0 ? 1 : row_count, sizeof(WalkRow)));
    usize strip_total = 0;
    usize cell4_total = 0;
    usize cell2_total = 0;
    for (u32 index = 0; index < pass_count; ++index) {
        const PassBuild& p = passes[index];
        strip_total = strip_total + (p.shape.radix == 4 ? 4 : 2) * static_cast<usize>(p.cell_count);
        if (p.shape.radix == 4) {
            cell4_total = cell4_total + p.cell_count;
        } else {
            cell2_total = cell2_total + p.cell_count;
        }
    }
    T* const strips = static_cast<T*>(take_plan_block(state, strip_total == 0 ? 1 : strip_total, sizeof(T)));
    WalkCell4* const all_cells4 =
        static_cast<WalkCell4*>(take_plan_block(state, cell4_total == 0 ? 1 : cell4_total, sizeof(WalkCell4)));
    WalkCell2* const all_cells2 =
        static_cast<WalkCell2*>(take_plan_block(state, cell2_total == 0 ? 1 : cell2_total, sizeof(WalkCell2)));
    if (final_passes == nullptr || final_segments == nullptr || rows == nullptr || strips == nullptr ||
        all_cells4 == nullptr || all_cells2 == nullptr) {
        return false;
    }
    usize strip_at = 0;
    usize cell4_at = 0;
    usize cell2_at = 0;
    for (u32 index = 0; index < segment_count; ++index) final_segments[index] = segments.data()[index];

    for (u32 index = 0; index < pass_count; ++index) {
        const PassBuild& p = passes[index];
        WalkPass& w = final_passes[index];
        w.radix = p.shape.radix;
        w.source = p.shape.source;
        w.destination = p.shape.destination;
        w.chunk = p.chunk;
        w.shift = layout.shift;
        w.shift2 = layout.shift2;
        const u32 in_gap = p.shape.source != WalkBuffer::caller ? layout.gap : 0;
        w.in_gap = in_gap;
        w.in_gap2 = in_gap;
        w.out_gap = layout.gap;
        w.out_gap2 = layout.gap;
        const u32 way = u32{1} << layout.shift;
        w.stretch = p.chunk > way ? way : p.chunk;
        for (u32 c = 0; c < 4; ++c) w.quarters[c] = place(c * p.chunk, w.shift, w.shift2, in_gap);
        w.dc_in = place(p.dc_in, w.shift, w.shift2, in_gap);
        w.ridge = p.ridge;
        w.ridge_in = place(p.ridge_in, w.shift, w.shift2, in_gap);
        for (u32 c = 0; c < 4; ++c) {
            w.dc_out[c] = place(p.dc_out[c], w.shift, w.shift2, layout.gap);
            w.ridge_out[c] = place(p.ridge_out[c], w.shift, w.shift2, layout.gap);
            w.offsets[c] = place(p.offsets[c], w.shift, w.shift2, layout.gap);
        }
        w.cell_count = p.cell_count;
        const usize strip_width = p.shape.radix == 4 ? 4 : 2;
        T* const strip = strips + strip_at;
        strip_at = strip_at + strip_width * p.cell_count;
        if constexpr (sizeof(T) == sizeof(double)) {
            w.strip64 = strip;
        } else {
            w.strip32 = strip;
        }
        const u32* const scheduled = order.data() + pass_first[index];
        if (p.shape.radix == 4) {
            WalkCell4* const cells = all_cells4 + cell4_at;
            cell4_at = cell4_at + p.cell_count;
            for (u32 c = 0; c < p.cell_count; ++c) {
                const BuiltCell& cell = p.cells[scheduled[c]];
                strip[4 * c] = cosine_at(table, eighth, 2 * cell.tw);
                strip[4 * c + 1] = sine_at(table, eighth, 2 * cell.tw);
                strip[4 * c + 2] = cosine_at(table, eighth, cell.tw);
                strip[4 * c + 3] = sine_at(table, eighth, cell.tw);
                cells[c] = WalkCell4{.in_re = place(cell.in_re, w.shift, w.shift2, in_gap),
                                     .in_im = place(cell.in_im, w.shift, w.shift2, in_gap),
                                     .out_o = place(cell.out[0], w.shift, w.shift2, layout.gap),
                                     .out_b = place(cell.out[1], w.shift, w.shift2, layout.gap)};
            }
            w.cells4 = cells;
        } else {
            WalkCell2* const cells = all_cells2 + cell2_at;
            cell2_at = cell2_at + p.cell_count;
            for (u32 c = 0; c < p.cell_count; ++c) {
                const BuiltCell& cell = p.cells[scheduled[c]];
                strip[2 * c] = cosine_at(table, eighth, cell.tw);
                strip[2 * c + 1] = sine_at(table, eighth, cell.tw);
                cells[c] = WalkCell2{.in_re = place(cell.in_re, w.shift, w.shift2, in_gap),
                                     .in_im = place(cell.in_im, w.shift, w.shift2, in_gap),
                                     .out0 = place(cell.out[0], w.shift, w.shift2, layout.gap),
                                     .out1 = place(cell.out[1], w.shift, w.shift2, layout.gap),
                                     .out2 = place(cell.out[2], w.shift, w.shift2, layout.gap),
                                     .out3 = place(cell.out[3], w.shift, w.shift2, layout.gap)};
            }
            w.cells2 = cells;
        }
    }

    // The terminal: rows k = 1 .. N/8 - 1 in order (its source is a work
    // buffer, so its places have gaps).
    const u32 top = u32{1} << terminal_t;
    WalkTerminal& terminal = tables.terminal;
    terminal.source = terminal_source;
    terminal.dc_in = place(row_address(m, terminal_t, 0), layout.shift, layout.shift2, layout.gap);
    terminal.ridge_in = place(row_address(m, terminal_t, top >> 1), layout.shift, layout.shift2, layout.gap);
    for (u32 k = 1; k <= row_count; ++k) {
        const u32 in_re = place(row_address(m, terminal_t, k), layout.shift, layout.shift2, layout.gap);
        const u32 in_im = place(row_address(m, terminal_t, top - k), layout.shift, layout.shift2, layout.gap);
        rows[k - 1] = WalkRow{.in_re = in_re, .in_im = in_im, .k = k};
    }
    terminal.rows = rows;
    terminal.row_count = row_count;
    // Rows with k <= N/16 read their larger angle 2k straight from the table.
    const u32 direct = un / 16;
    terminal.direct_rows = direct < row_count ? direct : row_count;

    tables.passes = final_passes;
    tables.pass_count = pass_count;
    tables.segments = final_segments;
    tables.segment_count = segment_count;

    return true;
}

} // namespace

bool build_walk(PlanState& state, const i32 n, const bool float64, const WalkOptions& options) noexcept {
    const bool result = float64 ? build_walk_of<double>(state, n, options) : build_walk_of<float>(state, n, options);
    return result;
}

WalkTier walk_tier_for(const simd::Level level) noexcept {
    if (level == simd::Level::scalar) return WalkTier::exact_portable;
#if defined(STXLIB_FFT_WALK_FUSED128)
    if (simd::fma128_at(level)) return WalkTier::fused128;
#endif
#if defined(STXLIB_FFT_WALK_EXACT128)
    return WalkTier::exact128;
#else
    return WalkTier::exact_portable;
#endif
}

bool walk_tier_runs(const WalkTier tier) noexcept {
    switch (tier) {
    case WalkTier::exact_portable:
        return true;
    case WalkTier::exact128:
#if defined(STXLIB_FFT_WALK_EXACT128)
        return simd::hardware_level() != simd::Level::scalar;
#else
        return false;
#endif
    case WalkTier::fused128:
#if defined(STXLIB_FFT_WALK_FUSED128)
        return simd::fma128_at(simd::hardware_level());
#else
        return false;
#endif
    }
    return false;
}

const WalkKernels& walk_kernels_for(const WalkTier tier) noexcept {
#if defined(STXLIB_FFT_WALK_FUSED128)
    if (tier == WalkTier::fused128) return walk_fused128::kernels;
#endif
#if defined(STXLIB_FFT_WALK_EXACT128)
    if (tier == WalkTier::exact128) return walk_exact128::kernels;
#endif
    static_cast<void>(tier);
    return walk_portable::kernels;
}

} // namespace stxlib::fft::detail
