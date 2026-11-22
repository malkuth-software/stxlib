#pragma once

// The state behind stxlib::conv::Field and Workspace (include/stxlib/conv.hpp), shared by
// field.cpp and transfer.cpp.
//
// A field keeps the straight RGBA8 source (packed, width * 4 bytes a row) and, per source row,
// the row's contested cells as a run list sorted by (cell, channel) (work/conv
// PERFORMANCE_REVIEW.md R6): an interior cell keeps its correction e = c - a (admitted minus
// raw currents; the linear kernel supplies a), an end cell its admitted currents. Every
// end cell is listed in every channel; an interior cell whose admission is idle is not listed.

#include "kernels.hpp"
#include "line.hpp"

#include "stxlib/conv.hpp"
#include "stxlib/types.hpp"
#include "stxlib/vector.hpp"

namespace stxlib::conv::detail {

struct Correction final {
    u32 cell{0};
    u32 channel{0};
    double e[5]{0.0, 0.0, 0.0, 0.0, 0.0};
};

struct FieldState final {
    usize width{0};
    usize height{0};
    bool ready{false};
    const Kernels* kernels{nullptr};
    Vector<u8> source;
    // Row y's run list, sorted by (cell, channel); one list per row, so a recompile replaces
    // only the rows an edit reaches.
    Vector<Vector<Correction>> rows;
};

inline View<Correction> row_corrections(const FieldState& field, const usize y) noexcept { return field.rows[y].view(); }

struct WorkspaceState final {
    // Field values at the destination pixels of one region row (channel innermost).
    Vector<double> values;
    // Axis canon: the per-v-cell idle flags and admitted cells.
    Vector<u8> simple;
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

} // namespace stxlib::conv::detail
