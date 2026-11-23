// stxlib::conv transfers and Workspace (include/stxlib/conv.hpp).
//
// Preview reads the field at each destination pixel's pulled-back centre. Canon on an axis
// map is Fubini with combined weights (work/conv FFT_MATH.md Part III): each destination
// pixel's footprint is cut at source cells along each axis, three-point Gauss on each piece
// (exact for the quintic pieces), the row values computed once per u node for every source
// row (cached per source cell), and the vertical cells settled once per u node in one K1 batch
// and shared by every destination row
// that reads it, its nodes' weights combined.
//
// Every transfer validates everything before it writes: on failure no pixel of the region
// has been written.

#include "canon.hpp"
#include "field_state.hpp"
#include "footprint.hpp"

#include "stxlib/memory.hpp"

#include <new>

namespace stxlib::conv::detail {

namespace {

bool finite(const double x) noexcept { return x == x && x - x == 0.0; }

OperationStatus refused(const Operation operation, const TextView message) noexcept {
    return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, operation, ErrorDetail{.message = message}));
}

OperationStatus exhausted(const Operation operation) noexcept {
    return OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, operation));
}

// The checks every transfer shares: a compiled field, a workspace, a destination whose view
// holds its rows, and a region inside it.
OperationStatus check_common(const Operation operation, const Field& field, const MutableRgba8Image& destination, const Region& region,
                             Workspace& workspace) noexcept {
    if (!field.ready()) return refused(operation, "the field is not compiled");
    if (FieldAccess::state(workspace) == nullptr) return refused(operation, "empty workspace");
    if (destination.width == 0 || destination.height == 0 || destination.width > max_side || destination.height > max_side) {
        return refused(operation, "each destination side must be in [1, max_side]");
    }
    if (destination.row_stride < destination.width * 4 ||
        destination.pixels.count() < (destination.height - 1) * destination.row_stride + destination.width * 4) {
        return refused(operation, "the destination view does not hold its rows");
    }
    if (region.x0 > region.x1 || region.y0 > region.y1 || region.x1 > destination.width || region.y1 > destination.height) {
        return refused(operation, "the region is not inside the destination");
    }
    return OperationStatus::success();
}

u8* pixel_at(const MutableRgba8Image& destination, const usize x, const usize y) noexcept {
    u8* const result = destination.pixels.data() + y * destination.row_stride + x * 4;
    return result;
}

// ---------------------------------------------------------------- maps for preview

// Preview axis map: nodes aligned at both ends.
void axis_at(const double width, const double height, const double target_width, const double target_height, const usize x, const usize y,
             double& u, double& v) noexcept {
    u = target_width > 1.0 ? static_cast<double>(x) * (width - 1.0) / (target_width - 1.0) : 0.0;
    v = target_height > 1.0 ? static_cast<double>(y) * (height - 1.0) / (target_height - 1.0) : 0.0;
}

void mesh_at(const MeshMap& mesh, const double x, const double y, double& u, double& v) noexcept {
    const double* const uv = mesh.source_uv.data();
    const double gxf = x / mesh.spacing;
    const double gyf = y / mesh.spacing;
    usize cx = static_cast<usize>(gxf);
    usize cy = static_cast<usize>(gyf);
    if (cx + 1 >= mesh.nodes_x) cx = mesh.nodes_x - 2;
    if (cy + 1 >= mesh.nodes_y) cy = mesh.nodes_y - 2;
    const double fx = gxf - static_cast<double>(cx);
    const double fy = gyf - static_cast<double>(cy);
    const usize n00 = cy * mesh.nodes_x + cx;
    const usize n10 = n00 + 1;
    const usize n01 = n00 + mesh.nodes_x;
    const usize n11 = n01 + 1;
    if (fx + fy <= 1.0) {
        u = uv[2 * n00] + fx * (uv[2 * n10] - uv[2 * n00]) + fy * (uv[2 * n01] - uv[2 * n00]);
        v = uv[2 * n00 + 1] + fx * (uv[2 * n10 + 1] - uv[2 * n00 + 1]) + fy * (uv[2 * n01 + 1] - uv[2 * n00 + 1]);
    } else {
        u = uv[2 * n11] + (1.0 - fx) * (uv[2 * n01] - uv[2 * n11]) + (1.0 - fy) * (uv[2 * n10] - uv[2 * n11]);
        v = uv[2 * n11 + 1] + (1.0 - fx) * (uv[2 * n01 + 1] - uv[2 * n11 + 1]) + (1.0 - fy) * (uv[2 * n10 + 1] - uv[2 * n11 + 1]);
    }
}

// ---------------------------------------------------------------- axis canon

// Per destination row: the v-cells its nodes fall in, with combined weights.
struct RowCellWeights final {
    usize cell{0};
    double linear[6]{0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double tail[5]{0.0, 0.0, 0.0, 0.0, 0.0};
    double total{0.0};
};

struct AxisCanonScratch final {
    Vector<FootprintNode> nodes;
    Vector<usize> row_offset;
    Vector<RowCellWeights> rows;
};

OperationStatus combine_rows(const usize height, const usize target_height, const Region& region, AxisCanonScratch& scratch) noexcept {
    const usize room = footprint_room(height, target_height);
    if (scratch.nodes.resize(room).failed()) return exhausted("stxlib.conv.transfer");
    scratch.row_offset.clear();
    scratch.rows.clear();
    for (usize y = region.y0; y < region.y1; ++y) {
        if (scratch.row_offset.push_copy(scratch.rows.count()).failed()) return exhausted("stxlib.conv.transfer");
        FootprintNode* const nodes = scratch.nodes.mutable_view().data();
        const usize count = footprint(y, height, target_height, nodes);
        for (usize b = 0; b < count; ++b) {
            const usize first = scratch.row_offset[scratch.row_offset.count() - 1];
            if (scratch.rows.count() == first || scratch.rows[scratch.rows.count() - 1].cell != nodes[b].cell) {
                RowCellWeights fresh;
                fresh.cell = nodes[b].cell;
                if (scratch.rows.push(static_cast<RowCellWeights&&>(fresh)).failed()) return exhausted("stxlib.conv.transfer");
            }
            RowCellWeights& e = scratch.rows[scratch.rows.count() - 1];
            double s[6];
            double tau[5];
            kernel_weights(nodes[b].t, s);
            tail_weights(nodes[b].t, tau);
            for (usize r = 0; r < 6; ++r) e.linear[r] = e.linear[r] + nodes[b].weight * s[r];
            for (usize k = 0; k < 5; ++k) e.tail[k] = e.tail[k] + nodes[b].weight * tau[k];
            e.total = e.total + nodes[b].weight;
        }
    }
    if (scratch.row_offset.push_copy(scratch.rows.count()).failed()) return exhausted("stxlib.conv.transfer");
    return OperationStatus::success();
}

// One u node's vertical cells j in [jlo, jhi], every channel, in one batch: simple (the linear
// kernel) or the admitted cell (anchor and five currents). Interior cells go through K1 over lanes
// (j - first) * 4 + c: pass A lists the windows whose raw currents are not one-signed, pass B
// projects the listed ones; a listed window inside its box stays simple. End cells are compiled
// whole. The same arithmetic as compile and Field::evaluate's vertical_value. `column` holds the
// column's row values with four padding rows before and after (column[-16 ..]).
void settle_range(const FieldState& field, const usize jlo, const usize jhi, const double* column, u8* simple, double* cells,
                  WorkspaceState& w) noexcept {
    const usize rows = field.height;
    const usize first = jlo > 2 ? jlo : 2;
    const usize last = rows >= 6 ? (jhi < rows - 4 ? jhi : rows - 4) : 0;
    for (usize j = jlo; j <= jhi; ++j) {
        if (rows >= 6 && j >= first && j <= last) continue;
        const CellShape shape = cell_shape(j, rows);
        for (usize c = 0; c < channel_count; ++c) {
            double win[8];
            for (usize k = 0; k < 8; ++k) {
                const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
                const usize clamped = row < 0 ? 0 : static_cast<usize>(row) >= rows ? rows - 1 : static_cast<usize>(row);
                win[k] = column[clamped * channel_count + c];
            }
            double raw[5];
            double admitted[5];
            raw_for_shape(win, shape, raw);
            (void)admit_cell(win, shape, raw, admitted, false);
            simple[j * channel_count + c] = 0;
            double* const cell = cells + (j * channel_count + c) * 6;
            cell[0] = win[3];
            for (usize m = 0; m < 5; ++m) cell[m + 1] = admitted[m];
        }
    }
    if (!(rows >= 6 && first <= last)) return;
    const usize n = (last - first + 1) * channel_count;
    const double* window[8];
    for (usize k = 0; k < 8; ++k) window[k] = column + (static_cast<i64>(first) - 3 + static_cast<i64>(k)) * static_cast<i64>(channel_count);
    double* const raw_base = w.axis_raw.mutable_view().data();
    double* raw[5];
    for (usize r = 0; r < 5; ++r) raw[r] = raw_base + r * n;
    u32* const list = w.axis_list.mutable_view().data();
    const Kernels& kernels = *field.kernels;
    const usize m = kernels.pass_a(window, raw, list, n);
    for (usize lane = 0; lane < n; ++lane) simple[first * channel_count + lane] = 1;
    if (m == 0) return;
    double* const gather = w.axis_gather.mutable_view().data();
    double* const admitted_base = w.axis_admitted.mutable_view().data();
    const usize stride = m;
    double* cw[4];
    double* cr[5];
    double* ca[5];
    for (usize k = 0; k < 4; ++k) cw[k] = gather + k * stride;
    for (usize r = 0; r < 5; ++r) {
        cr[r] = gather + (4 + r) * stride;
        ca[r] = admitted_base + r * stride;
    }
    for (usize g = 0; g < m; ++g) {
        const usize lane = list[g];
        for (usize k = 0; k < 4; ++k) cw[k][g] = window[k + 2][lane];
        for (usize r = 0; r < 5; ++r) cr[r][g] = raw[r][lane];
    }
    const double* const cwc[8]{cw[0], cw[0], cw[0], cw[1], cw[2], cw[3], cw[3], cw[3]};
    const double* const crc[5]{cr[0], cr[1], cr[2], cr[3], cr[4]};
    u8* const idle = w.axis_idle.mutable_view().data();
    kernels.pass_b(cwc, crc, ca, idle, m);
    for (usize g = 0; g < m; ++g) {
        if (idle[g] != 0) continue;
        const usize lane = list[g];
        const usize slot = first * channel_count + lane;
        simple[slot] = 0;
        double* const cell = cells + slot * 6;
        cell[0] = window[3][lane];
        for (usize r = 0; r < 5; ++r) cell[r + 1] = ca[r][g];
    }
}

OperationStatus axis_canon(const FieldState& field, const AxisMap& map, const MutableRgba8Image& destination, const Region& region,
                           WorkspaceState& workspace, const bool fused) noexcept {
    const usize rows = field.height;
    const usize region_rows = region.y1 - region.y0;
    if (region_rows == 0 || region.x0 == region.x1) return OperationStatus::success();
    AxisCanonScratch scratch;
    const OperationStatus combined = combine_rows(rows, map.target_height, region, scratch);
    if (combined.failed()) return combined;
    // The v-cells the region's rows read: one contiguous range, settled per u node.
    usize jlo = rows;
    usize jhi = 0;
    for (usize e = 0; e < scratch.rows.count(); ++e) {
        const usize j = scratch.rows[e].cell;
        jlo = j < jlo ? j : jlo;
        jhi = j > jhi ? j : jhi;
    }
    constexpr usize pad = 4 * channel_count;
    const usize lanes = rows * channel_count;
    Vector<FootprintNode> u_nodes;
    OperationStatus status = u_nodes.resize(footprint_room(field.width, map.target_width));
    if (status.succeeded()) status = workspace.padded_column.resize(lanes + 2 * pad);
    if (status.succeeded()) status = workspace.simple.resize(lanes);
    if (status.succeeded()) status = workspace.cells.resize(lanes * 6);
    if (status.succeeded()) status = workspace.accumulator.resize(region_rows * channel_count);
    if (status.succeeded()) status = workspace.row_cells.resize(rows * 6 * channel_count);
    if (status.succeeded()) status = workspace.axis_raw.resize(5 * lanes);
    if (status.succeeded()) status = workspace.axis_list.resize(lanes);
    if (status.succeeded()) status = workspace.axis_gather.resize(9 * lanes);
    if (status.succeeded()) status = workspace.axis_admitted.resize(5 * lanes);
    if (status.succeeded()) status = workspace.axis_idle.resize(lanes);
    if (status.failed()) return exhausted("stxlib.conv.transfer");
    double* const padded = workspace.padded_column.mutable_view().data();
    for (usize k = 0; k < lanes + 2 * pad; ++k) padded[k] = 0.0;
    double* const column = padded + pad;
    u8* const simple = workspace.simple.mutable_view().data();
    double* const cells = workspace.cells.mutable_view().data();
    double* const accumulator = workspace.accumulator.mutable_view().data();
    double* const row_cells = workspace.row_cells.mutable_view().data();
    // The cell whose row cells are cached (none yet).
    usize cached = field.width;
    for (usize x = region.x0; x < region.x1; ++x) {
        for (usize k = 0; k < region_rows * channel_count; ++k) accumulator[k] = 0.0;
        FootprintNode* const nodes = u_nodes.mutable_view().data();
        const usize node_count = footprint(x, field.width, map.target_width, nodes);
        // Fused (experiment, owner 2026-11-23): the column of row values integrated across the
        // footprint first (exact: the rows are quintic per cell, their 2D corrections stored),
        // then one vertical contest per destination column on those averages, as the 1D method
        // orders it; exact canon contests at every u node instead.
        if (fused) {
            for (usize k = 0; k < lanes; ++k) column[k] = 0.0;
        }
        for (usize a = 0; a < node_count; ++a) {
            const usize i = nodes[a].cell;
            if (i != cached) {
                for (usize r = 0; r < rows; ++r) row_cell(field, r, i, row_cells + r * 6 * channel_count);
                cached = i;
            }
            double tauu[5];
            tail_weights(nodes[a].t, tauu);
            for (usize r = 0; r < rows; ++r) {
                const double* const cell = row_cells + r * 6 * channel_count;
                for (usize c = 0; c < channel_count; ++c) {
                    double value = cell[c] + cell[1 * channel_count + c] * tauu[0];
                    value = value + cell[2 * channel_count + c] * tauu[1];
                    value = value + cell[3 * channel_count + c] * tauu[2];
                    value = value + cell[4 * channel_count + c] * tauu[3];
                    value = value + cell[5 * channel_count + c] * tauu[4];
                    if (fused) {
                        column[r * channel_count + c] = column[r * channel_count + c] + nodes[a].weight * value;
                    } else {
                        column[r * channel_count + c] = value;
                    }
                }
            }
            if (fused && a + 1 < node_count) continue;
            const double node_weight = fused ? 1.0 : nodes[a].weight;
            settle_range(field, jlo, jhi, column, simple, cells, workspace);
            for (usize y = 0; y < region_rows; ++y) {
                double sum[channel_count]{0.0, 0.0, 0.0, 0.0};
                for (usize e = scratch.row_offset[y]; e < scratch.row_offset[y + 1]; ++e) {
                    const RowCellWeights& entry = scratch.rows[e];
                    const usize j = entry.cell;
                    for (usize c = 0; c < channel_count; ++c) {
                        double value = 0.0;
                        if (simple[j * channel_count + c] != 0) {
                            for (usize r = 0; r < 6; ++r) value = value + entry.linear[r] * column[(j + r - 2) * channel_count + c];
                        } else {
                            const double* const cell = cells + (j * channel_count + c) * 6;
                            value = entry.total * cell[0];
                            for (usize k = 0; k < 5; ++k) value = value + entry.tail[k] * cell[k + 1];
                        }
                        sum[c] = sum[c] + value;
                    }
                }
                for (usize c = 0; c < channel_count; ++c) {
                    accumulator[y * channel_count + c] = accumulator[y * channel_count + c] + node_weight * sum[c];
                }
            }
        }
        for (usize y = 0; y < region_rows; ++y) encode(accumulator + y * channel_count, pixel_at(destination, x, region.y0 + y));
    }
    return OperationStatus::success();
}

// ---------------------------------------------------------------- preview driver

enum class MapKind : u8 {
    axis = 0,
    affine = 1,
    mesh = 2,
    projective = 3,
};

struct AnyMap final {
    MapKind kind{MapKind::axis};
    const AxisMap* axis{nullptr};
    const AffineMap* affine{nullptr};
    const MeshMap* mesh{nullptr};
    const ProjectiveMap* projective{nullptr};
};

void pull_back(const AnyMap& map, const FieldState& field, const usize x, const usize y, double& u, double& v) noexcept {
    const double fx = static_cast<double>(x);
    const double fy = static_cast<double>(y);
    switch (map.kind) {
    case MapKind::axis: {
        // In the source's coordinates, then into the stored orientation.
        const double sw = static_cast<double>(field.transposed ? field.height : field.width);
        const double sh = static_cast<double>(field.transposed ? field.width : field.height);
        axis_at(sw, sh, static_cast<double>((*map.axis).target_width), static_cast<double>((*map.axis).target_height), x, y, u, v);
        if (field.transposed) {
            const double t = u;
            u = v;
            v = t;
        }
        return;
    }
    case MapKind::affine: {
        const AffineMap& a = *map.affine;
        u = a.ou + a.a11 * fx + a.a12 * fy;
        v = a.ov + a.a21 * fx + a.a22 * fy;
        return;
    }
    case MapKind::mesh:
        mesh_at(*map.mesh, fx, fy, u, v);
        return;
    case MapKind::projective: {
        const double* const h = (*map.projective).h;
        const double w = h[6] * fx + h[7] * fy + h[8];
        u = (h[0] * fx + h[1] * fy + h[2]) / w;
        v = (h[3] * fx + h[4] * fy + h[5]) / w;
        return;
    }
    }
    u = fx;
    v = fy;
}

// ---------------------------------------------------------------- tiled preview (K2)
//
// work/conv SIMD_DESIGN.md Stage 2. Per 32 x 32 tile: the pulled-back points; the tile's
// local row cells (anchor plus currents, 24 doubles: channel innermost) for the source rows
// and cells its points reach, built once by K2 row_cells plus the rows' run lists; every
// query's eight row values by K2 tile_windows; K1 pass A over all vertical windows, the linear
// value everywhere, pass B on the listed windows only and the admitted value where admission
// acts; end vertical cells compiled whole. Every step has Field::evaluate's operation order,
// so the bytes are encode(evaluate(pull_back(x, y))). A tile whose points reach more than
// limit_cells cells or limit_rows rows is evaluated point by point (the same bits).

constexpr usize tile = 32;
constexpr usize limit_cells = 96;
constexpr usize limit_rows = 104;
constexpr usize tile_queries = tile * tile;
constexpr usize tile_lanes = tile_queries * channel_count;
constexpr double zero_cell[24]{};

OperationStatus prepare_tiled(WorkspaceState& w) noexcept {
    OperationStatus status = w.us.resize(tile_queries);
    if (status.succeeded()) status = w.vs.resize(tile_queries);
    if (status.succeeded()) status = w.local.resize(limit_rows * limit_cells * 24);
    if (status.succeeded()) status = w.decoded.resize((limit_cells + 8) * channel_count);
    if (status.succeeded()) status = w.planes.resize((8 + 5 + 1) * tile_lanes);
    if (status.succeeded()) status = w.sv.resize(6 * tile_queries);
    if (status.succeeded()) status = w.tau_u.resize(5 * tile_queries);
    if (status.succeeded()) status = w.tau_v.resize(5 * tile_queries);
    if (status.succeeded()) status = w.row_pointers.resize(8 * tile_queries);
    if (status.succeeded()) status = w.list.resize(tile_lanes);
    if (status.succeeded()) status = w.compact.resize((8 + 5 + 5 + 1 + 5) * (tile_lanes + 1));
    if (status.succeeded()) status = w.idle.resize(tile_lanes);
    if (status.succeeded()) status = w.end_queries.resize(tile_queries);
    if (status.succeeded()) status = w.query_cell_v.resize(tile_queries);
    return status;
}

// Builds the local cells of rows r0..r1, cells imin..imax.
void build_local(const FieldState& field, const usize r0, const usize r1, const usize imin, const usize imax, WorkspaceState& w) noexcept {
    const usize width = field.width;
    const usize ncells = imax - imin + 1;
    double* const local = w.local.mutable_view().data();
    double* const decoded = w.decoded.mutable_view().data();
    const usize inner_first = imin > 2 ? imin : 2;
    const usize inner_last = width >= 4 ? (imax < width - 4 ? imax : width - 4) : 0;
    for (usize r = r0; r <= r1; ++r) {
        double* const row_local = local + (r - r0) * ncells * 24;
        const u8* const row = field.source.view().data() + r * width * 4;
        if (width >= 6 && inner_first <= inner_last) {
            for (usize x = inner_first - 2; x <= inner_last + 3; ++x) decode(row + x * 4, decoded + (x - (inner_first - 2)) * channel_count);
            (*field.kernels).row_cells(decoded, inner_last - inner_first + 1, row_local + (inner_first - imin) * 24);
        }
        // End cells: anchor, then their stored currents from the run list below.
        for (usize i = imin; i <= imax; ++i) {
            if (interior_cell(i, width)) continue;
            double* const cell = row_local + (i - imin) * 24;
            double anchor[channel_count];
            decode(row + i * 4, anchor);
            for (usize c = 0; c < channel_count; ++c) cell[c] = anchor[c];
        }
        const View<Correction> row_list = row_corrections(field, r);
        for (usize k = 0; k < row_list.count(); ++k) {
            const Correction& entry = row_list.data()[k];
            if (entry.cell < imin || entry.cell > imax) continue;
            double* const cell = row_local + (entry.cell - imin) * 24;
            const usize c = entry.channel;
            if (interior_cell(entry.cell, width)) {
                for (usize m = 0; m < 5; ++m) cell[(m + 1) * channel_count + c] = cell[(m + 1) * channel_count + c] + entry.e[m];
            } else {
                for (usize m = 0; m < 5; ++m) cell[(m + 1) * channel_count + c] = entry.e[m];
            }
        }
    }
}

void preview_tile(const FieldState& field, const AnyMap& map, const MutableRgba8Image& destination, const usize tx0, const usize ty0,
                  const usize tx1, const usize ty1, WorkspaceState& w) noexcept {
    const usize width = field.width;
    const usize height = field.height;
    const usize cells = width - 1;
    const usize tw = tx1 - tx0;
    const usize count = (ty1 - ty0) * tw;
    double* const us = w.us.mutable_view().data();
    double* const vs = w.vs.mutable_view().data();
    usize imin = cells;
    usize imax = 0;
    usize jmin = height;
    usize jmax = 0;
    for (usize y = ty0; y < ty1; ++y) {
        for (usize x = tx0; x < tx1; ++x) {
            const usize q = (y - ty0) * tw + (x - tx0);
            pull_back(map, field, x, y, us[q], vs[q]);
            usize i = 0;
            usize j = 0;
            double t = 0.0;
            locate(us[q], cells, i, t);
            locate(vs[q], height - 1, j, t);
            imin = i < imin ? i : imin;
            imax = i > imax ? i : imax;
            jmin = j < jmin ? j : jmin;
            jmax = j > jmax ? j : jmax;
        }
    }
    const usize r0 = jmin >= 3 ? jmin - 3 : 0;
    const usize r1 = jmax + 4 < height ? jmax + 4 : height - 1;
    const usize ncells = imax - imin + 1;
    if (ncells > limit_cells || r1 - r0 + 1 > limit_rows) {
        for (usize q = 0; q < count; ++q) {
            double value[channel_count];
            field_value(field, us[q], vs[q], value);
            encode(value, pixel_at(destination, tx0 + q % tw, ty0 + q / tw));
        }
        return;
    }
    build_local(field, r0, r1, imin, imax, w);
    const Kernels& kernels = *field.kernels;
    const double* const local = w.local.view().data();
    double* const sv = w.sv.mutable_view().data();
    double* const tau_u = w.tau_u.mutable_view().data();
    double* const tau_v = w.tau_v.mutable_view().data();
    const double** const rows = w.row_pointers.mutable_view().data();
    usize* const end_queries = w.end_queries.mutable_view().data();
    usize* const query_cell_v = w.query_cell_v.mutable_view().data();
    usize ends = 0;
    for (usize q = 0; q < count; ++q) {
        usize i = 0;
        usize j = 0;
        double tu = 0.0;
        double tv = 0.0;
        locate(us[q], cells, i, tu);
        locate(vs[q], height - 1, j, tv);
        tail_weights(tu, tau_u + 5 * q);
        kernel_weights(tv, sv + 6 * q);
        tail_weights(tv, tau_v + 5 * q);
        const bool interior = interior_cell(j, height);
        for (usize k = 0; k < 8; ++k) {
            const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
            const bool skipped = (interior && (k == 0 || k == 7)) || row < 0 || static_cast<usize>(row) >= height;
            rows[8 * q + k] = skipped ? zero_cell : local + ((static_cast<usize>(row) - r0) * ncells + (i - imin)) * 24;
        }
        query_cell_v[q] = j;
        if (!interior) {
            end_queries[ends] = q;
            ends = ends + 1;
        }
    }
    double* const planes = w.planes.mutable_view().data();
    double* window[8];
    double* raw[5];
    for (usize k = 0; k < 8; ++k) window[k] = planes + k * tile_lanes;
    for (usize k = 0; k < 5; ++k) raw[k] = planes + (8 + k) * tile_lanes;
    double* const value = planes + 13 * tile_lanes;
    kernels.tile_windows(count, rows, tau_u, window);
    const usize n = count * channel_count;
    const double* const wc[8]{window[0], window[1], window[2], window[3], window[4], window[5], window[6], window[7]};
    u32* const list = w.list.mutable_view().data();
    const usize m = kernels.pass_a(wc, raw, list, n);
    kernels.vertical_linear(count, wc, sv, value);
    // Pass B on the listed windows: gather samples 2..5, the raw currents and the tails.
    double* const compact = w.compact.mutable_view().data();
    const usize stride = m + 1;
    double* cw[8];
    double* cr[5];
    double* ca[5];
    for (usize k = 0; k < 8; ++k) cw[k] = compact + k * stride;
    for (usize k = 0; k < 5; ++k) {
        cr[k] = compact + (8 + k) * stride;
        ca[k] = compact + (13 + k) * stride;
    }
    double* const cv = compact + 18 * stride;
    double* const ct = compact + 19 * stride;
    for (usize g = 0; g < m; ++g) {
        const usize lane = list[g];
        for (usize k = 2; k <= 5; ++k) cw[k][g] = window[k][lane];
        for (usize k = 0; k < 5; ++k) {
            cr[k][g] = raw[k][lane];
            ct[5 * g + k] = tau_v[5 * (lane / channel_count) + k];
        }
    }
    const double* const cwc[8]{cw[2], cw[2], cw[2], cw[3], cw[4], cw[5], cw[5], cw[5]};
    const double* const crc[5]{cr[0], cr[1], cr[2], cr[3], cr[4]};
    u8* const idle = w.idle.mutable_view().data();
    kernels.pass_b(cwc, crc, ca, idle, m);
    const double* const cac[5]{ca[0], ca[1], ca[2], ca[3], ca[4]};
    kernels.vertical_admitted(cw[3], ct, cac, cv, m);
    // Where the box holds the raw currents the value stays the linear one (Field::evaluate).
    for (usize g = 0; g < m; ++g) {
        if (idle[g] == 0) value[list[g]] = cv[g];
    }
    // End vertical cells (whole windows, end interior lanes overwritten): compiled whole.
    for (usize e = 0; e < ends; ++e) {
        const usize q = end_queries[e];
        const usize j = query_cell_v[q];
        const CellShape shape = cell_shape(j, height);
        for (usize c = 0; c < channel_count; ++c) {
            const usize lane = q * channel_count + c;
            double win[8];
            for (usize k = 0; k < 8; ++k) win[k] = window[k][lane];
            double r[5];
            double admitted[5];
            raw_for_shape(win, shape, r);
            (void)admit_cell(win, shape, r, admitted, false);
            value[lane] = evaluate_cell(win[3], admitted, tau_v + 5 * q);
        }
    }
    for (usize q = 0; q < count; ++q) encode(value + q * channel_count, pixel_at(destination, tx0 + q % tw, ty0 + q / tw));
}

OperationStatus preview(const FieldState& field, const AnyMap& map, const MutableRgba8Image& destination, const Region& region,
                        WorkspaceState& workspace) noexcept {
    if (region.x0 == region.x1 || region.y0 == region.y1) return OperationStatus::success();
    if (prepare_tiled(workspace).failed()) return exhausted("stxlib.conv.transfer");
    for (usize ty0 = region.y0; ty0 < region.y1; ty0 += tile) {
        const usize ty1 = ty0 + tile < region.y1 ? ty0 + tile : region.y1;
        for (usize tx0 = region.x0; tx0 < region.x1; tx0 += tile) {
            const usize tx1 = tx0 + tile < region.x1 ? tx0 + tile : region.x1;
            preview_tile(field, map, destination, tx0, ty0, tx1, ty1, workspace);
        }
    }
    return OperationStatus::success();
}


} // namespace

} // namespace stxlib::conv::detail

namespace stxlib::conv {

using detail::FieldAccess;
using detail::FieldState;
using detail::WorkspaceState;

Workspace::~Workspace() { release(); }

Workspace::Workspace(Workspace&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Workspace& Workspace::operator=(Workspace&& other) noexcept {
    if (this != &other) {
        release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void Workspace::release() noexcept {
    WorkspaceState* const state = state_;
    state_ = nullptr;
    if (state == nullptr) return;
    (*state).~WorkspaceState();
    memory::release(state);
}

OperationResult<Workspace> Workspace::make() noexcept {
    void* const block = memory::allocate(sizeof(WorkspaceState), alignof(WorkspaceState));
    if (block == nullptr) {
        return OperationResult<Workspace>::failure(ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.workspace.make"));
    }
    WorkspaceState* const state = ::new (block) WorkspaceState();
    return OperationResult<Workspace>::success(Workspace(state));
}

usize Workspace::storage_bytes() const noexcept {
    if (state_ == nullptr) return 0;
    const WorkspaceState& w = *state_;
    const usize doubles = w.values.capacity() + w.cells.capacity() + w.accumulator.capacity() + w.us.capacity() + w.vs.capacity() +
                          w.local.capacity() + w.decoded.capacity() + w.planes.capacity() + w.sv.capacity() + w.tau_u.capacity() +
                          w.tau_v.capacity() + w.compact.capacity() + w.row_cells.capacity() + w.padded_column.capacity() +
                          w.axis_raw.capacity() + w.axis_gather.capacity() + w.axis_admitted.capacity();
    const usize total = sizeof(WorkspaceState) + doubles * sizeof(double) + w.simple.capacity() + w.idle.capacity() + w.axis_idle.capacity() +
                        (w.list.capacity() + w.axis_list.capacity()) * sizeof(u32) + w.row_pointers.capacity() * sizeof(const double*) +
                        (w.end_queries.capacity() + w.query_cell_v.capacity()) * sizeof(usize);
    return total;
}

OperationStatus transfer(const Field& field, const AxisMap& map, const Mode mode, const MutableRgba8Image& destination, const Region& region,
                         Workspace& workspace) noexcept {
    const OperationStatus common = detail::check_common("stxlib.conv.transfer.axis", field, destination, region, workspace);
    if (common.failed()) return common;
    if (map.target_width != destination.width || map.target_height != destination.height) {
        return detail::refused("stxlib.conv.transfer.axis", "the destination must have the map's target size");
    }
    const FieldState& state = *FieldAccess::state(field);
    if (mode == Mode::canon && state.transposed) {
        // The stored field's rows run along the destination's columns: render the transposed
        // destination and copy it back, so a tall source gives the transpose of its transpose's
        // resize, bit for bit.
        detail::WorkspaceState& w = *FieldAccess::state(workspace);
        if (w.transposed_pixels.resize(destination.width * destination.height * 4).failed()) return detail::exhausted("stxlib.conv.transfer");
        AxisMap swapped;
        swapped.target_width = map.target_height;
        swapped.target_height = map.target_width;
        MutableRgba8Image scratch;
        scratch.pixels = w.transposed_pixels.mutable_view();
        scratch.width = destination.height;
        scratch.height = destination.width;
        scratch.row_stride = destination.height * 4;
        const Region turned{region.y0, region.x0, region.y1, region.x1};
        const OperationStatus done = detail::axis_canon(state, swapped, scratch, turned, w, false);
        if (done.failed()) return done;
        const u8* const t = w.transposed_pixels.view().data();
        for (usize y = region.y0; y < region.y1; ++y) {
            for (usize x = region.x0; x < region.x1; ++x) {
                for (usize k = 0; k < 4; ++k) destination.pixels.data()[y * destination.row_stride + x * 4 + k] = t[(x * scratch.width + y) * 4 + k];
            }
        }
        return OperationStatus::success();
    }
    if (mode == Mode::canon) return detail::axis_canon(state, map, destination, region, *FieldAccess::state(workspace), false);
    detail::AnyMap any;
    any.kind = detail::MapKind::axis;
    any.axis = &map;
    return detail::preview(state, any, destination, region, *FieldAccess::state(workspace));
}

OperationStatus transfer(const Field& field, const AffineMap& map, const Mode mode, const MutableRgba8Image& destination,
                         const Region& region, Workspace& workspace) noexcept {
    const OperationStatus common = detail::check_common("stxlib.conv.transfer.affine", field, destination, region, workspace);
    if (common.failed()) return common;
    const bool valid = detail::finite(map.a11) && detail::finite(map.a12) && detail::finite(map.a21) && detail::finite(map.a22) &&
                       detail::finite(map.ou) && detail::finite(map.ov);
    if (!valid) return detail::refused("stxlib.conv.transfer.affine", "the map's coefficients must be finite");
    // A transposed field reads (v, u): the map's rows swapped.
    AffineMap stored = map;
    if ((*FieldAccess::state(field)).transposed) {
        stored.a11 = map.a21;
        stored.a12 = map.a22;
        stored.a21 = map.a11;
        stored.a22 = map.a12;
        stored.ou = map.ov;
        stored.ov = map.ou;
    }
    if (mode == Mode::canon) {
        return detail::affine_canon(*FieldAccess::state(field), stored, destination, region, *FieldAccess::state(workspace));
    }
    detail::AnyMap any;
    any.kind = detail::MapKind::affine;
    any.affine = &stored;
    return detail::preview(*FieldAccess::state(field), any, destination, region, *FieldAccess::state(workspace));
}

OperationStatus transfer(const Field& field, const MeshMap& map, const Mode mode, const MutableRgba8Image& destination, const Region& region,
                         Workspace& workspace) noexcept {
    const OperationStatus common = detail::check_common("stxlib.conv.transfer.mesh", field, destination, region, workspace);
    if (common.failed()) return common;
    if (map.nodes_x < 2 || map.nodes_y < 2 || map.nodes_x > max_side || map.nodes_y > max_side || !detail::finite(map.spacing) ||
        !(map.spacing > 0.0) || map.source_uv.count() / 2 / map.nodes_x < map.nodes_y) {
        return detail::refused("stxlib.conv.transfer.mesh", "the mesh needs at least 2 x 2 nodes, a positive spacing and its positions");
    }
    const double* const uv = map.source_uv.data();
    for (usize k = 0; k < 2 * map.nodes_x * map.nodes_y; ++k) {
        if (!detail::finite(uv[k])) return detail::refused("stxlib.conv.transfer.mesh", "every node position must be finite");
    }
    // A transposed field reads (v, u): the node positions swapped, in the workspace.
    MeshMap stored = map;
    detail::WorkspaceState& ws = *FieldAccess::state(workspace);
    if ((*FieldAccess::state(field)).transposed) {
        const usize count = 2 * map.nodes_x * map.nodes_y;
        if (ws.swapped_uv.resize(count).failed()) return detail::exhausted("stxlib.conv.transfer");
        double* const swapped = ws.swapped_uv.mutable_view().data();
        for (usize k = 0; k < count; k += 2) {
            swapped[k] = uv[k + 1];
            swapped[k + 1] = uv[k];
        }
        stored.source_uv = View<double>(swapped, count);
    }
    if (mode == Mode::canon) {
        return detail::mesh_canon(*FieldAccess::state(field), stored, destination, region, ws);
    }
    detail::AnyMap any;
    any.kind = detail::MapKind::mesh;
    any.mesh = &stored;
    return detail::preview(*FieldAccess::state(field), any, destination, region, *FieldAccess::state(workspace));
}

OperationStatus transfer(const Field& field, const ProjectiveMap& map, const Mode mode, const MutableRgba8Image& destination,
                         const Region& region, Workspace& workspace) noexcept {
    const OperationStatus common = detail::check_common("stxlib.conv.transfer.projective", field, destination, region, workspace);
    if (common.failed()) return common;
    for (usize k = 0; k < 9; ++k) {
        if (!detail::finite(map.h[k])) return detail::refused("stxlib.conv.transfer.projective", "the map's coefficients must be finite");
    }
    if (region.x0 < region.x1 && region.y0 < region.y1) {
        // w is linear, so positive at the region's four corner centres means positive on it.
        const double xs[2]{static_cast<double>(region.x0), static_cast<double>(region.x1 - 1)};
        const double ys[2]{static_cast<double>(region.y0), static_cast<double>(region.y1 - 1)};
        for (usize a = 0; a < 2; ++a) {
            for (usize b = 0; b < 2; ++b) {
                const double w = map.h[6] * xs[a] + map.h[7] * ys[b] + map.h[8];
                if (!(w > 0.0)) return detail::refused("stxlib.conv.transfer.projective", "w must be positive on the region");
            }
        }
    }
    // A transposed field reads (v, u): the numerators' rows swapped.
    ProjectiveMap stored = map;
    if ((*FieldAccess::state(field)).transposed) {
        for (usize k = 0; k < 3; ++k) {
            stored.h[k] = map.h[3 + k];
            stored.h[3 + k] = map.h[k];
        }
    }
    if (mode == Mode::canon) {
        return detail::projective_canon(*FieldAccess::state(field), stored, destination, region, *FieldAccess::state(workspace));
    }
    detail::AnyMap any;
    any.kind = detail::MapKind::projective;
    any.projective = &stored;
    return detail::preview(*FieldAccess::state(field), any, destination, region, *FieldAccess::state(workspace));
}


namespace {

usize clamp_index(const double x, const usize limit) noexcept {
    if (!(x > 0.0)) return 0;
    if (x >= static_cast<double>(limit)) return limit;
    return static_cast<usize>(x);
}

} // namespace

Region affected_region(const NodeBox& box, const AxisMap& map, const usize source_width, const usize source_height) noexcept {
    Region region;
    if (box.empty || map.target_width == 0 || map.target_height == 0 || source_width < 2 || source_height < 2) return region;
    const double W = static_cast<double>(map.target_width);
    const double H = static_cast<double>(map.target_height);
    const double w = static_cast<double>(source_width);
    const double h = static_cast<double>(source_height);
    // Preview: x reads u = x (w - 1) / (W - 1). Canon: footprint [x s - 1/2, (x + 1) s - 1/2],
    // s = w / W. The union of both pixel ranges, grown by one.
    const double u0 = static_cast<double>(box.u0);
    const double u1 = static_cast<double>(box.u1);
    const double v0 = static_cast<double>(box.v0);
    const double v1 = static_cast<double>(box.v1);
    const double px0 = W > 1.0 ? u0 * (W - 1.0) / (w - 1.0) : 0.0;
    const double px1 = W > 1.0 ? u1 * (W - 1.0) / (w - 1.0) : 0.0;
    const double py0 = H > 1.0 ? v0 * (H - 1.0) / (h - 1.0) : 0.0;
    const double py1 = H > 1.0 ? v1 * (H - 1.0) / (h - 1.0) : 0.0;
    const double cx0 = (u0 + 0.5) * W / w - 1.0;
    const double cx1 = (u1 + 0.5) * W / w;
    const double cy0 = (v0 + 0.5) * H / h - 1.0;
    const double cy1 = (v1 + 0.5) * H / h;
    const double x0 = (px0 < cx0 ? px0 : cx0) - 1.0;
    const double x1 = (px1 > cx1 ? px1 : cx1) + 2.0;
    const double y0 = (py0 < cy0 ? py0 : cy0) - 1.0;
    const double y1 = (py1 > cy1 ? py1 : cy1) + 2.0;
    region.x0 = clamp_index(x0, map.target_width);
    region.x1 = clamp_index(x1, map.target_width);
    region.y0 = clamp_index(y0, map.target_height);
    region.y1 = clamp_index(y1, map.target_height);
    return region;
}

Region affected_region(const NodeBox& box, const AffineMap& map, const usize source_width, const usize source_height, const usize destination_width,
                       const usize destination_height) noexcept {
    Region region;
    if (box.empty || destination_width == 0 || destination_height == 0) return region;
    const double det = map.a11 * map.a22 - map.a12 * map.a21;
    const Region whole{0, 0, destination_width, destination_height};
    if (!(det == det) || det == 0.0) return whole;
    // Destination of a source point: p = A^-1 (q - o). The box grown by the canon tile box's
    // one-region margin plus one, its corners mapped, the bounding box grown by a pixel (the
    // half-pixel footprint, and rounding), then widened to whole 16-pixel canon tiles: canon
    // carries its primitive Q along a tile row, so an edit can change the rounding of every
    // pixel of a tile whose cell box it reaches (canon.cpp; tiles sit on the destination grid).
    // Coordinates outside the source clamp to its border, so a box reaching a border reaches
    // without limit beyond it (reach: a distance past any destination).
    const double reach = 1e15;
    const double us[2]{box.u0 == 0 ? -reach : static_cast<double>(box.u0) - 2.0,
                       box.u1 + 1 >= source_width ? reach : static_cast<double>(box.u1) + 2.0};
    const double vs[2]{box.v0 == 0 ? -reach : static_cast<double>(box.v0) - 2.0,
                       box.v1 + 1 >= source_height ? reach : static_cast<double>(box.v1) + 2.0};
    double xmin = 1e300;
    double xmax = -1e300;
    double ymin = 1e300;
    double ymax = -1e300;
    for (usize a = 0; a < 2; ++a) {
        for (usize b = 0; b < 2; ++b) {
            const double du = us[a] - map.ou;
            const double dv = vs[b] - map.ov;
            const double x = (map.a22 * du - map.a12 * dv) / det;
            const double y = (map.a11 * dv - map.a21 * du) / det;
            xmin = x < xmin ? x : xmin;
            xmax = x > xmax ? x : xmax;
            ymin = y < ymin ? y : ymin;
            ymax = y > ymax ? y : ymax;
        }
    }
    if (!(xmin == xmin) || !(ymin == ymin) || !(xmax == xmax) || !(ymax == ymax)) return whole;
    constexpr usize canon_tile = 16;
    const usize x0 = clamp_index(xmin - 1.5, destination_width);
    const usize x1 = clamp_index(xmax + 2.5, destination_width);
    const usize y0 = clamp_index(ymin - 1.5, destination_height);
    const usize y1 = clamp_index(ymax + 2.5, destination_height);
    region.x0 = x0 / canon_tile * canon_tile;
    region.y0 = y0 / canon_tile * canon_tile;
    const usize xr = (x1 + canon_tile - 1) / canon_tile * canon_tile;
    const usize yr = (y1 + canon_tile - 1) / canon_tile * canon_tile;
    region.x1 = xr < destination_width ? xr : destination_width;
    region.y1 = yr < destination_height ? yr : destination_height;
    if (region.x0 >= region.x1 || region.y0 >= region.y1) {
        const Region nothing;
        return nothing;
    }
    return region;
}


namespace detail {
// The fused axis canon (experiment): tests and measurements only, not part of the API yet.
OperationStatus axis_canon_fused(const Field& field, const AxisMap& map, const MutableRgba8Image& destination, const Region& region,
                                 Workspace& workspace) noexcept {
    const OperationStatus common = check_common("stxlib.conv.transfer.axis", field, destination, region, workspace);
    if (common.failed()) return common;
    if (map.target_width != destination.width || map.target_height != destination.height) {
        return refused("stxlib.conv.transfer.axis", "the destination must have the map's target size");
    }
    return axis_canon(*FieldAccess::state(field), map, destination, region, *FieldAccess::state(workspace), true);
}
} // namespace detail

} // namespace stxlib::conv
