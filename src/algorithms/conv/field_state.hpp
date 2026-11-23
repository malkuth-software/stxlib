#pragma once

// The state behind stxlib::conv::Field and Workspace (include/stxlib/conv.hpp), shared by
// field.cpp and transfer.cpp.
//
// A field keeps the straight RGBA8 source (packed, width * 4 bytes a row) and nothing else. A
// row cell's admission is a pure function of its eight source samples, so the transfers compute
// the cells they need on demand (row_segment), with compile's arithmetic: an interior cell is
// its raw currents plus e = admitted - raw where the contest acts, an end cell its admitted
// currents. (Until 2026-11-23 the corrections were stored, 48 bytes per contested cell and
// channel, about 1 GB for a 2049 x 2049 source; the bytes of every output are unchanged.)

#include "kernels.hpp"
#include "line.hpp"

#include "stxlib/conv.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::conv::detail {

struct FieldState final {
    // The stored orientation. When the source's height is over 150% of its width the field is
    // stored transposed (owner, 2026-11-23: the 1D form along the longer axis first), so the rows,
    // the first admitted axis, run along the source's longer side; width and height here are then
    // the source's height and width, and every entry point swaps coordinates.
    usize width{0};
    usize height{0};
    bool transposed{false};
    bool ready{false};
    const Kernels* kernels{nullptr};
    Vector<u8> source;
};

// Scratch for row_segment over up to `capacity` cells: the decoded planes (four channels, the
// segment's samples i - 3 .. i + 4), K2's decoded pixels, K1's raw currents, list, gathered
// windows, admitted currents and idle bytes. Pointers into caller storage, so a single cell can
// use stack arrays (SmallSegment) and a block can use a workspace's vectors.
struct SegmentBuffers final {
    usize capacity{0};
    double* planes{nullptr};   // channel_count * (capacity + 7)
    double* decoded{nullptr};  // (capacity + 5) * channel_count
    double* raw{nullptr};      // 5 * capacity
    u32* list{nullptr};        // capacity
    double* gathered{nullptr}; // 4 * capacity
    double* admitted{nullptr}; // 5 * capacity
    double* compact{nullptr};  // 5 * capacity
    u8* idle{nullptr};         // capacity
};

// Stack scratch for up to 8 cells.
struct SmallSegment final {
    static constexpr usize cells = 8;
    double planes[channel_count * (cells + 7)];
    double decoded[(cells + 5) * channel_count];
    double raw[5 * cells];
    u32 list[cells];
    double gathered[4 * cells];
    double admitted[5 * cells];
    double compact[5 * cells];
    u8 idle[cells];
    SegmentBuffers buffers() noexcept {
        SegmentBuffers b;
        b.capacity = cells;
        b.planes = planes;
        b.decoded = decoded;
        b.raw = raw;
        b.list = list;
        b.gathered = gathered;
        b.admitted = admitted;
        b.compact = compact;
        b.idle = idle;
        return b;
    }
};

// Workspace scratch for row_segment over up to `capacity` cells.
struct SegmentScratch final {
    Vector<double> planes;
    Vector<double> decoded;
    Vector<double> raw;
    Vector<u32> list;
    Vector<double> gathered;
    Vector<double> admitted;
    Vector<double> compact;
    Vector<u8> idle;
};
[[nodiscard]] OperationStatus prepare_segment(SegmentScratch& scratch, usize capacity) noexcept;
SegmentBuffers segment_buffers(SegmentScratch& scratch) noexcept;

// Row r's cells imin .. imax (imax - imin < buffers.capacity), 24 doubles each (anchor[4], then
// five currents, channel innermost; row_cell's layout): an interior cell's K2 raw currents plus
// the contest's e = admitted - raw on the channels where it acts (K1, as compile did), an end
// cell's admitted currents. Lanes are independent, so the bytes do not depend on the segment.
void row_segment(const FieldState& field, usize r, usize imin, usize imax, double* cells, const SegmentBuffers& buffers) noexcept;

struct WorkspaceState final {
    // Field values at the destination pixels of one region row (channel innermost).
    Vector<double> values;
    // Axis canon: the per-v-cell idle flags and admitted cells.
    Vector<u8> simple;
    // Transposed fields: a mesh's swapped node positions, and axis canon's transposed scratch.
    Vector<double> swapped_uv;
    Vector<u8> transposed_pixels;
    Vector<double> cells;
    Vector<double> accumulator;
    // Tiled preview (transfer.cpp): per tile the pulled-back points, the local row cells, the
    // vertical windows (structure of arrays), per-query weights and row pointers, and K1's
    // list, compact batch and idle bytes.
    Vector<double> us;
    Vector<double> vs;
    Vector<double> local;
    Vector<double> decoded;
    Vector<double> planes;
    Vector<double> sv;
    Vector<double> tau_u;
    Vector<double> tau_v;
    Vector<const double*> row_pointers;
    Vector<u32> list;
    Vector<double> compact;
    Vector<u8> idle;
    Vector<usize> end_queries;
    Vector<usize> query_cell_v;
    // Axis canon: per source row its cached row cell (24 doubles) for the column's cell, and the
    // vertical contest batch (lanes = v-cell x channel).
    Vector<double> row_cells;
    Vector<double> padded_column;
    Vector<double> axis_raw;
    Vector<u32> axis_list;
    Vector<double> axis_gather;
    Vector<double> axis_admitted;
    Vector<u8> axis_idle;
    // Axis preview: per region row its vertical cell and weights (sv[6], tau_v[5]).
    Vector<usize> axis_cells;
    Vector<double> axis_weights;
    // Axis preview: the distinct interior vertical cells the region reads, and their gathered
    // windows (8 planes, lanes = listed cell x channel).
    Vector<usize> axis_listed;
    Vector<double> axis_windows;
    // Composited affine transfer (experiment): a strip of lines' row-pass values (line-major,
    // rows, channel innermost) and per line its source rows and vertical cells.
    Vector<double> composite_values;
    Vector<usize> composite_ranges;
    // row_segment's scratch (tiled preview: limit_cells; axis paths: a block of cells).
    SegmentScratch segment;
};

// A line field: the samples (node-major, lanes innermost, packed) and per cell and lane its
// five currents, formed as a 2D field's rows form them (raw, raw + e, or the end cell's
// admitted currents); a value is the anchor plus currents . tau.
struct LineFieldState final {
    usize nodes{0};
    usize lanes{0};
    bool ready{false};
    Vector<double> samples;
    Vector<double> values;
};

struct FieldAccess final {
    static const FieldState* state(const Field& field) noexcept { return field.state_; }
    static const LineFieldState* state(const LineField& field) noexcept { return field.state_; }
    static WorkspaceState* state(Workspace& workspace) noexcept { return workspace.state_; }
};

// Row y's value at column cell i (tails tau; s is unused), all channels: anchor plus
// (raw + e) . tau for interior cells (e on listed channels only), anchor plus c . tau for end
// cells.
void row_value(const FieldState& field, usize y, usize i, const double* s, const double* tau, double* out) noexcept;

// Row y's cell i: anchor[4] then five currents (channel innermost, 24 doubles), as row_value
// uses them; a value is cell[c] + cell[4 + c] tau0 + ... + cell[20 + c] tau4.
void row_cell(const FieldState& field, usize y, usize i, double* cell) noexcept;

// The vertical cell j's value in channel c from the window w[0..7] (rows j-3 .. j+4).
double vertical_value(const FieldState& field, usize j, const double* w, const double* sv, const double* tauv) noexcept;

// The field at (u, v), all channels (Field::evaluate's arithmetic).
void field_value(const FieldState& field, double u, double v, double* out) noexcept;

// The fused axis canon (experiment, transfer.cpp): horizontal integration first, then one
// vertical contest per destination column. Tests and measurements only.
OperationStatus axis_canon_fused(const Field& field, const AxisMap& map, const MutableRgba8Image& destination, const Region& region,
                                 Workspace& workspace) noexcept;

// The composited affine transfer (experiment, transfer.cpp): a row pass along the stored rows,
// then a column pass along destination lines, each the 1D field. Tests and measurements only.
// Refuses (unsupported) a map whose passes would be singular or reduce by more than 32.
OperationStatus affine_composited(const Field& field, const AffineMap& map, Mode mode, const MutableRgba8Image& destination,
                                  const Region& region, Workspace& workspace) noexcept;

} // namespace stxlib::conv::detail
