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
    // corrections[row_start[y] .. row_start[y + 1]) are row y's.
    Vector<usize> row_start;
    Vector<Correction> corrections;
};

struct WorkspaceState final {
    // Field values at the destination pixels of one region row (channel innermost).
    Vector<double> values;
    // Axis canon: per source row, one column of row values (channel innermost); the
    // per-v-cell settle stamps, idle flags and admitted cells.
    Vector<double> column;
    Vector<u32> settled;
    Vector<u8> simple;
    Vector<double> cells;
    Vector<double> accumulator;
};

struct FieldAccess final {
    static const FieldState* state(const Field& field) noexcept { return field.state_; }
    static WorkspaceState* state(Workspace& workspace) noexcept { return workspace.state_; }
};

// Row y's value at column cell i (weights s for the phase, tails tau), all channels: the
// linear kernel plus tau . e on listed channels (end cells: anchor plus tau . c).
void row_value(const FieldState& field, usize y, usize i, const double* s, const double* tau, double* out) noexcept;

// The vertical cell j's value in channel c from the window w[0..7] (rows j-3 .. j+4).
double vertical_value(const FieldState& field, usize j, const double* w, const double* sv, const double* tauv) noexcept;

// The field at (u, v), all channels (Field::evaluate's arithmetic).
void field_value(const FieldState& field, double u, double v, double* out) noexcept;

} // namespace stxlib::conv::detail
