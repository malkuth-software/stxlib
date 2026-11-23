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

// Preview axis map: each destination pixel reads the centre of the footprint canon averages,
// (x + 1/2) w / W - 1/2, so swapping preview for canon never shifts the image (owner,
// 2026-11-23: one lattice for both modes).
void axis_at(const double width, const double height, const double target_width, const double target_height, const usize x, const usize y,
             double& u, double& v) noexcept {
    u = (static_cast<double>(x) + 0.5) * width / target_width - 0.5;
    v = (static_cast<double>(y) + 0.5) * height / target_height - 0.5;
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

// settle_range's interior settle for a list of interior cells js[0 .. count) instead of a range:
// their windows gathered from the column, the same K1 calls, the same results per (j, channel).
void settle_list(const FieldState& field, const usize* const js, const usize count, const double* column, u8* simple, double* cells,
                 WorkspaceState& w) noexcept {
    if (count == 0) return;
    const usize n = count * channel_count;
    double* const planes = w.axis_windows.mutable_view().data();
    const double* window[8];
    for (usize k = 0; k < 8; ++k) {
        double* const plane = planes + k * n;
        for (usize l = 0; l < count; ++l) {
            const double* const source = column + (static_cast<i64>(js[l]) - 3 + static_cast<i64>(k)) * static_cast<i64>(channel_count);
            for (usize c = 0; c < channel_count; ++c) plane[l * channel_count + c] = source[c];
        }
        window[k] = plane;
    }
    double* const raw_base = w.axis_raw.mutable_view().data();
    double* raw[5];
    for (usize r = 0; r < 5; ++r) raw[r] = raw_base + r * n;
    u32* const list = w.axis_list.mutable_view().data();
    const Kernels& kernels = *field.kernels;
    const usize m = kernels.pass_a(window, raw, list, n);
    for (usize l = 0; l < count; ++l) {
        for (usize c = 0; c < channel_count; ++c) simple[js[l] * channel_count + c] = 1;
    }
    if (m == 0) return;
    double* const gather = w.axis_gather.mutable_view().data();
    double* const admitted_base = w.axis_admitted.mutable_view().data();
    double* cw[4];
    double* cr[5];
    double* ca[5];
    for (usize k = 0; k < 4; ++k) cw[k] = gather + k * m;
    for (usize r = 0; r < 5; ++r) {
        cr[r] = gather + (4 + r) * m;
        ca[r] = admitted_base + r * m;
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
        const usize slot = js[lane / channel_count] * channel_count + lane % channel_count;
        simple[slot] = 0;
        double* const cell = cells + slot * 6;
        cell[0] = window[3][lane];
        for (usize r = 0; r < 5; ++r) cell[r + 1] = ca[r][g];
    }
}

// Cells per row_segment call on the axis paths: the cache holds this many cells of every band row.
constexpr usize axis_block = 16;

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
    // The source rows those cells read: each cell's window is rows j - 3 .. j + 4, clamped to the
    // field (settle_range), and the simple kernel reads j - 2 .. j + 3. Only these rows are
    // evaluated, so a region costs in proportion to its own rows, not the field's (a display
    // reduction split into strips does the work once in all, not once per strip). The bytes are
    // the same: the rows outside the band are never read.
    const usize rlo = jlo > 3 ? jlo - 3 : 0;
    const usize rhi = jhi + 4 < rows - 1 ? jhi + 4 : rows - 1;
    constexpr usize pad = 4 * channel_count;
    const usize lanes = rows * channel_count;
    Vector<FootprintNode> u_nodes;
    OperationStatus status = u_nodes.resize(footprint_room(field.width, map.target_width));
    if (status.succeeded()) status = workspace.padded_column.resize(lanes + 2 * pad);
    if (status.succeeded()) status = workspace.simple.resize(lanes);
    if (status.succeeded()) status = workspace.cells.resize(lanes * 6);
    if (status.succeeded()) status = workspace.accumulator.resize(region_rows * channel_count);
    if (status.succeeded()) status = workspace.row_cells.resize(rows * axis_block * 24);
    if (status.succeeded()) status = prepare_segment(workspace.segment, axis_block);
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
    const SegmentBuffers buffers = segment_buffers(workspace.segment);
    // The block of cells [cached, cached + axis_block) whose row cells are cached (none yet).
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
            for (usize k = rlo * channel_count; k < (rhi + 1) * channel_count; ++k) column[k] = 0.0;
        }
        for (usize a = 0; a < node_count; ++a) {
            const usize i = nodes[a].cell;
            if (cached == field.width || i < cached || i >= cached + axis_block) {
                cached = i;
                const usize last = cached + axis_block - 1 < field.width - 2 ? cached + axis_block - 1 : field.width - 2;
                for (usize r = rlo; r <= rhi; ++r) row_segment(field, r, cached, last, row_cells + r * axis_block * 24, buffers);
            }
            double tauu[5];
            tail_weights(nodes[a].t, tauu);
            for (usize r = rlo; r <= rhi; ++r) {
                const double* const cell = row_cells + (r * axis_block + (i - cached)) * 24;
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
    if (status.succeeded()) status = prepare_segment(w.segment, limit_cells);
    return status;
}

// Builds the local cells of rows r0..r1, cells imin..imax (row_segment, compile's arithmetic).
void build_local(const FieldState& field, const usize r0, const usize r1, const usize imin, const usize imax, WorkspaceState& w) noexcept {
    const usize ncells = imax - imin + 1;
    double* const local = w.local.mutable_view().data();
    const SegmentBuffers buffers = segment_buffers(w.segment);
    for (usize r = r0; r <= r1; ++r) row_segment(field, r, imin, imax, local + (r - r0) * ncells * 24, buffers);
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

// ---------------------------------------------------------------- separable axis preview
//
// An axis map's u depends on x alone and v on y alone, so the preview separates exactly: per
// destination column, the row values at u for the band of source rows the region reads (from
// row_segment blocks, cached across columns), then one vertical settle per cell j (K1, as axis
// canon's settle_range), shared by every destination row in that cell; per pixel only the cell's
// value at v. Every value is Field::evaluate's: the row values have row_value's operation order,
// interior vertical cells settle_range's (vertical_value's arithmetic), and end vertical cells go
// through vertical_value itself on field_value's window (rows outside the field read 0).

OperationStatus axis_preview(const FieldState& field, const AxisMap& map, const MutableRgba8Image& destination, const Region& region,
                             WorkspaceState& w) noexcept {
    const usize rows = field.height;
    const usize width = field.width;
    const usize region_rows = region.y1 - region.y0;
    if (region_rows == 0 || region.x0 == region.x1) return OperationStatus::success();
    constexpr usize pad = 4 * channel_count;
    const usize lanes = rows * channel_count;
    OperationStatus status = w.axis_cells.resize(region_rows);
    if (status.succeeded()) status = w.axis_listed.resize(region_rows);
    if (status.succeeded()) status = w.axis_windows.resize(8 * region_rows * channel_count);
    if (status.succeeded()) status = w.axis_weights.resize(region_rows * 11);
    if (status.succeeded()) status = w.padded_column.resize(lanes + 2 * pad);
    if (status.succeeded()) status = w.simple.resize(lanes);
    if (status.succeeded()) status = w.cells.resize(lanes * 6);
    if (status.succeeded()) status = w.row_cells.resize(rows * axis_block * 24);
    if (status.succeeded()) status = w.axis_raw.resize(5 * lanes);
    if (status.succeeded()) status = w.axis_list.resize(lanes);
    if (status.succeeded()) status = w.axis_gather.resize(9 * lanes);
    if (status.succeeded()) status = w.axis_admitted.resize(5 * lanes);
    if (status.succeeded()) status = w.axis_idle.resize(lanes);
    if (status.succeeded()) status = prepare_segment(w.segment, axis_block);
    if (status.failed()) return exhausted("stxlib.conv.transfer");
    const double sw = static_cast<double>(width);
    const double sh = static_cast<double>(rows);
    const double tw = static_cast<double>(map.target_width);
    const double th = static_cast<double>(map.target_height);
    // Per region row: the vertical cell and its weights.
    usize* const cell_of = w.axis_cells.mutable_view().data();
    double* const weights = w.axis_weights.mutable_view().data();
    usize jlo = rows;
    usize jhi = 0;
    for (usize y = region.y0; y < region.y1; ++y) {
        double u = 0.0;
        double v = 0.0;
        axis_at(sw, sh, tw, th, region.x0, y, u, v);
        usize j = 0;
        double tv = 0.0;
        locate(v, rows - 1, j, tv);
        cell_of[y - region.y0] = j;
        kernel_weights(tv, weights + (y - region.y0) * 11);
        tail_weights(tv, weights + (y - region.y0) * 11 + 6);
        jlo = j < jlo ? j : jlo;
        jhi = j > jhi ? j : jhi;
    }
    const usize rlo = jlo > 3 ? jlo - 3 : 0;
    const usize rhi = jhi + 4 < rows - 1 ? jhi + 4 : rows - 1;
    // The distinct interior cells the rows read (v grows with y, so equal cells are adjacent).
    // When they are most of the range, settle the range; otherwise only the listed cells.
    usize* const listed = w.axis_listed.mutable_view().data();
    usize listed_count = 0;
    for (usize y = 0; y < region_rows; ++y) {
        const usize j = cell_of[y];
        if (!interior_cell(j, rows)) continue;
        if (listed_count > 0 && listed[listed_count - 1] == j) continue;
        listed[listed_count] = j;
        listed_count = listed_count + 1;
    }
    const bool dense = listed_count * 4 >= (jhi - jlo + 1) * 3;
    double* const padded = w.padded_column.mutable_view().data();
    for (usize k = 0; k < lanes + 2 * pad; ++k) padded[k] = 0.0;
    double* const column = padded + pad;
    u8* const simple = w.simple.mutable_view().data();
    double* const cells = w.cells.mutable_view().data();
    double* const row_cells = w.row_cells.mutable_view().data();
    const SegmentBuffers buffers = segment_buffers(w.segment);
    usize cached = width;
    for (usize x = region.x0; x < region.x1; ++x) {
        double u = 0.0;
        double v = 0.0;
        axis_at(sw, sh, tw, th, x, region.y0, u, v);
        usize i = 0;
        double tu = 0.0;
        locate(u, width - 1, i, tu);
        double tauu[5];
        tail_weights(tu, tauu);
        if (cached == width || i < cached || i >= cached + axis_block) {
            cached = i;
            const usize last = cached + axis_block - 1 < width - 2 ? cached + axis_block - 1 : width - 2;
            for (usize r = rlo; r <= rhi; ++r) row_segment(field, r, cached, last, row_cells + r * axis_block * 24, buffers);
        }
        for (usize r = rlo; r <= rhi; ++r) {
            const double* const cell = row_cells + (r * axis_block + (i - cached)) * 24;
            for (usize c = 0; c < channel_count; ++c) {
                double value = cell[c] + cell[1 * channel_count + c] * tauu[0];
                value = value + cell[2 * channel_count + c] * tauu[1];
                value = value + cell[3 * channel_count + c] * tauu[2];
                value = value + cell[4 * channel_count + c] * tauu[3];
                value = value + cell[5 * channel_count + c] * tauu[4];
                column[r * channel_count + c] = value;
            }
        }
        if (dense) {
            settle_range(field, jlo, jhi, column, simple, cells, w);
        } else {
            settle_list(field, listed, listed_count, column, simple, cells, w);
        }
        for (usize y = 0; y < region_rows; ++y) {
            const usize j = cell_of[y];
            const double* const sv = weights + y * 11;
            const double* const tauv = sv + 6;
            double value[channel_count];
            if (!interior_cell(j, rows)) {
                for (usize c = 0; c < channel_count; ++c) {
                    double window[8];
                    for (usize k = 0; k < 8; ++k) {
                        const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
                        window[k] = row < 0 || static_cast<usize>(row) >= rows ? 0.0 : column[static_cast<usize>(row) * channel_count + c];
                    }
                    value[c] = vertical_value(field, j, window, sv, tauv);
                }
            } else {
                for (usize c = 0; c < channel_count; ++c) {
                    if (simple[j * channel_count + c] != 0) {
                        double linear = 0.0;
                        for (usize r = 0; r < 6; ++r) linear = linear + sv[r] * column[(j - 2 + r) * channel_count + c];
                        value[c] = linear;
                    } else {
                        const double* const cell = cells + (j * channel_count + c) * 6;
                        value[c] = evaluate_cell(cell[0], cell + 1, tauv);
                    }
                }
            }
            encode(value, pixel_at(destination, x, region.y0 + y));
        }
    }
    return OperationStatus::success();
}

// ---------------------------------------------------------------- composited affine (experiment)
//
// An affine map as two 1D passes (Catmull-Smith), each the 1D CONV field. Destination lines p
// (columns when |a22| >= |a21|, rows otherwise) and positions q along them. Pass 2 reads each
// line's column of intermediate values at v = delta p + eps q + zeta; pass 1 makes those values
// from the stored rows: the value of line p at source row r is row r's 1D field at
// u = alpha p + beta r + gamma (eliminating q). Each row's cells come from one row_segment over
// the strip's cells, each line's vertical cells from one settle_range. Canon averages each pass
// over its pixel's extent (|alpha| along u, |eps| along v; exact for the quintic cells), so the
// footprint is the destination pixel sheared along the line, area preserved. Outside the source
// the passes clamp separately: a row is clamped along u at its own u, not at the point's.

// Weights of the interval [left, right] (right > left) of the 1D field on `nodes` nodes, per cell
// touched: the parts outside [0, nodes - 1] read the border value, the rest is cut at cells with
// three-point Gauss per piece. Returns the entry count (at most room), or room + 1 if more.
usize interval_weights(const double left, const double right, const usize nodes, RowCellWeights* out, const usize room) noexcept {
    const double extent = static_cast<double>(nodes - 1);
    const double length = right - left;
    usize count = 0;
    double outside_left = 0.0;
    double outside_right = 0.0;
    double a = left;
    double b_end = right;
    if (a < 0.0) {
        outside_left = (b_end < 0.0 ? b_end : 0.0) - a;
        a = 0.0;
    }
    if (b_end > extent) {
        outside_right = b_end - (a > extent ? a : extent);
        b_end = extent;
    }
    if (outside_left > 0.0) {
        RowCellWeights e;
        e.cell = 0;
        double s[6];
        double tau[5];
        kernel_weights(0.0, s);
        tail_weights(0.0, tau);
        const double weight = outside_left / length;
        for (usize r = 0; r < 6; ++r) e.linear[r] = weight * s[r];
        for (usize k = 0; k < 5; ++k) e.tail[k] = weight * tau[k];
        e.total = weight;
        out[0] = e;
        count = 1;
    }
    while (a < b_end) {
        // a is in [0, extent), so truncation is floor.
        double whole = static_cast<double>(static_cast<u64>(a));
        if (whole >= extent) whole = extent - 1.0;
        double b = whole + 1.0;
        if (b > b_end) b = b_end;
        const usize cell = static_cast<usize>(whole);
        if (count == 0 || out[count - 1].cell != cell) {
            if (count == room) return room + 1;
            out[count] = RowCellWeights{};
            out[count].cell = cell;
            count = count + 1;
        }
        RowCellWeights& e = out[count - 1];
        const double half = 0.5 * (b - a);
        const double mid = 0.5 * (a + b);
        for (usize k = 0; k < 3; ++k) {
            const double t = mid + half * gauss_x[k] - whole;
            const double weight = gauss_w[k] * half / length;
            double s[6];
            double tau[5];
            kernel_weights(t, s);
            tail_weights(t, tau);
            for (usize r = 0; r < 6; ++r) e.linear[r] = e.linear[r] + weight * s[r];
            for (usize m = 0; m < 5; ++m) e.tail[m] = e.tail[m] + weight * tau[m];
            e.total = e.total + weight;
        }
        a = b;
    }
    if (outside_right > 0.0) {
        const usize cell = nodes - 2;
        if (count == 0 || out[count - 1].cell != cell) {
            if (count == room) return room + 1;
            out[count] = RowCellWeights{};
            out[count].cell = cell;
            count = count + 1;
        }
        RowCellWeights& e = out[count - 1];
        double s[6];
        double tau[5];
        kernel_weights(1.0, s);
        tail_weights(1.0, tau);
        const double weight = outside_right / length;
        for (usize r = 0; r < 6; ++r) e.linear[r] = e.linear[r] + weight * s[r];
        for (usize k = 0; k < 5; ++k) e.tail[k] = e.tail[k] + weight * tau[k];
        e.total = e.total + weight;
    }
    return count;
}

// The largest reduction a composited pass takes, and so the interval entries it can need.
constexpr double composite_reach = 32.0;
constexpr usize composite_room = 40;
// Destination lines per strip: the rows' cells are made once per strip.
constexpr usize composite_strip = 32;

OperationStatus affine_composited_state(const FieldState& field, const AffineMap& map, const Mode mode,
                                        const MutableRgba8Image& destination, const Region& region, WorkspaceState& w) noexcept {
    const usize width = field.width;
    const usize rows = field.height;
    const bool canon = mode == Mode::canon;
    const double det = map.a11 * map.a22 - map.a12 * map.a21;
    const double m21 = map.a21 < 0.0 ? 0.0 - map.a21 : map.a21;
    const double m22 = map.a22 < 0.0 ? 0.0 - map.a22 : map.a22;
    // On a tie the line direction follows the stored orientation, so a transposed field still
    // gives the transpose of its transpose's bytes.
    const bool by_columns = m22 > m21 || (m22 == m21 && !field.transposed);
    if ((by_columns ? m22 : m21) == 0.0 || det == 0.0) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::unsupported, "stxlib.conv.transfer.affine_composited",
                                                          ErrorDetail{.message = "the map does not separate into two passes"}));
    }
    double alpha = 0.0;
    double beta = 0.0;
    double gamma = 0.0;
    double delta = 0.0;
    double eps = 0.0;
    if (by_columns) {
        alpha = map.a11 - map.a12 * map.a21 / map.a22;
        beta = map.a12 / map.a22;
        gamma = map.ou - map.a12 * map.ov / map.a22;
        delta = map.a21;
        eps = map.a22;
    } else {
        alpha = map.a12 - map.a11 * map.a22 / map.a21;
        beta = map.a11 / map.a21;
        gamma = map.ou - map.a11 * map.ov / map.a21;
        delta = map.a22;
        eps = map.a21;
    }
    const double zeta = map.ov;
    const double reach_u = alpha < 0.0 ? 0.0 - alpha : alpha;
    const double reach_v = eps < 0.0 ? 0.0 - eps : eps;
    if (!finite(alpha) || !finite(beta) || !finite(gamma) || reach_u > composite_reach || reach_v > composite_reach) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::unsupported, "stxlib.conv.transfer.affine_composited",
                                                          ErrorDetail{.message = "a pass reduces by more than the composited reach"}));
    }
    const double hu = canon ? 0.5 * reach_u : 0.0;
    const double hv = canon ? 0.5 * reach_v : 0.0;
    const usize p0 = by_columns ? region.x0 : region.y0;
    const usize p1 = by_columns ? region.x1 : region.y1;
    const usize q0 = by_columns ? region.y0 : region.x0;
    const usize q1 = by_columns ? region.y1 : region.x1;
    if (p0 == p1 || q0 == q1) return OperationStatus::success();
    constexpr usize pad = 4 * channel_count;
    const usize lanes = rows * channel_count;
    // Cells per row segment: a strip's lines move u by |alpha| each, plus the canon reach.
    const double span = static_cast<double>(composite_strip) * reach_u + 2.0 * hu + 4.0;
    const usize capacity = span >= static_cast<double>(width) ? width : static_cast<usize>(span) + 1;
    OperationStatus status = w.composite_values.resize(composite_strip * lanes);
    if (status.succeeded()) status = w.composite_ranges.resize(composite_strip * 4);
    if (status.succeeded()) status = w.row_cells.resize(capacity * 24);
    if (status.succeeded()) status = prepare_segment(w.segment, capacity);
    if (status.succeeded()) status = w.padded_column.resize(lanes + 2 * pad);
    if (status.succeeded()) status = w.simple.resize(lanes);
    if (status.succeeded()) status = w.cells.resize(lanes * 6);
    if (status.succeeded()) status = w.axis_raw.resize(5 * lanes);
    if (status.succeeded()) status = w.axis_list.resize(lanes);
    if (status.succeeded()) status = w.axis_gather.resize(9 * lanes);
    if (status.succeeded()) status = w.axis_admitted.resize(5 * lanes);
    if (status.succeeded()) status = w.axis_idle.resize(lanes);
    if (status.failed()) return exhausted("stxlib.conv.transfer");
    double* const values = w.composite_values.mutable_view().data();
    usize* const ranges = w.composite_ranges.mutable_view().data();
    double* const row_cells = w.row_cells.mutable_view().data();
    const SegmentBuffers buffers = segment_buffers(w.segment);
    double* const padded = w.padded_column.mutable_view().data();
    for (usize k = 0; k < lanes + 2 * pad; ++k) padded[k] = 0.0;
    double* const column = padded + pad;
    u8* const simple = w.simple.mutable_view().data();
    double* const cells = w.cells.mutable_view().data();
    RowCellWeights entries[composite_room];
    const double fq0 = static_cast<double>(q0);
    const double fq1 = static_cast<double>(q1 - 1);
    for (usize s0 = p0; s0 < p1; s0 += composite_strip) {
        const usize s1 = s0 + composite_strip < p1 ? s0 + composite_strip : p1;
        // Per line: its vertical cells and the source rows their windows read.
        usize srlo = rows;
        usize srhi = 0;
        for (usize p = s0; p < s1; ++p) {
            const double base = delta * static_cast<double>(p) + zeta;
            const double va = base + eps * fq0;
            const double vb = base + eps * fq1;
            const double vmin = (va < vb ? va : vb) - hv;
            const double vmax = (va < vb ? vb : va) + hv;
            usize jlo = 0;
            usize jhi = 0;
            double t = 0.0;
            locate(vmin, rows - 1, jlo, t);
            locate(vmax, rows - 1, jhi, t);
            const usize rlo = jlo > 3 ? jlo - 3 : 0;
            const usize rhi = jhi + 4 < rows - 1 ? jhi + 4 : rows - 1;
            usize* const range = ranges + (p - s0) * 4;
            range[0] = jlo;
            range[1] = jhi;
            range[2] = rlo;
            range[3] = rhi;
            srlo = rlo < srlo ? rlo : srlo;
            srhi = rhi > srhi ? rhi : srhi;
        }
        // Pass 1: per source row, its cells over the strip's lines once, then each line's value.
        for (usize r = srlo; r <= srhi; ++r) {
            const double row_base = beta * static_cast<double>(r) + gamma;
            usize ilo = width;
            usize ihi = 0;
            for (usize p = s0; p < s1; ++p) {
                const usize* const range = ranges + (p - s0) * 4;
                if (r < range[2] || r > range[3]) continue;
                const double u = alpha * static_cast<double>(p) + row_base;
                usize ia = 0;
                usize ib = 0;
                double t = 0.0;
                locate(u - hu, width - 1, ia, t);
                locate(u + hu, width - 1, ib, t);
                ilo = ia < ilo ? ia : ilo;
                ihi = ib > ihi ? ib : ihi;
            }
            if (ilo > ihi) continue;
            row_segment(field, r, ilo, ihi, row_cells, buffers);
            for (usize p = s0; p < s1; ++p) {
                const usize* const range = ranges + (p - s0) * 4;
                if (r < range[2] || r > range[3]) continue;
                const double u = alpha * static_cast<double>(p) + row_base;
                double* const out = values + ((p - s0) * rows + r) * channel_count;
                if (!canon) {
                    usize i = 0;
                    double t = 0.0;
                    locate(u, width - 1, i, t);
                    double tau[5];
                    tail_weights(t, tau);
                    const double* const cell = row_cells + (i - ilo) * 24;
                    for (usize c = 0; c < channel_count; ++c) {
                        double value = cell[c] + cell[1 * channel_count + c] * tau[0];
                        value = value + cell[2 * channel_count + c] * tau[1];
                        value = value + cell[3 * channel_count + c] * tau[2];
                        value = value + cell[4 * channel_count + c] * tau[3];
                        value = value + cell[5 * channel_count + c] * tau[4];
                        out[c] = value;
                    }
                } else {
                    const usize count = interval_weights(u - hu, u + hu, width, entries, composite_room);
                    double sum[channel_count]{0.0, 0.0, 0.0, 0.0};
                    for (usize e = 0; e < count && e < composite_room; ++e) {
                        const double* const cell = row_cells + (entries[e].cell - ilo) * 24;
                        for (usize c = 0; c < channel_count; ++c) {
                            double value = entries[e].total * cell[c];
                            for (usize k = 0; k < 5; ++k) value = value + entries[e].tail[k] * cell[(k + 1) * channel_count + c];
                            sum[c] = sum[c] + value;
                        }
                    }
                    for (usize c = 0; c < channel_count; ++c) out[c] = sum[c];
                }
            }
        }
        // Pass 2: per line, its column settled once, then each position's value.
        for (usize p = s0; p < s1; ++p) {
            const usize* const range = ranges + (p - s0) * 4;
            const usize jlo = range[0];
            const usize jhi = range[1];
            const double* const line_values = values + (p - s0) * rows * channel_count;
            for (usize k = range[2] * channel_count; k < (range[3] + 1) * channel_count; ++k) column[k] = line_values[k];
            settle_range(field, jlo, jhi, column, simple, cells, w);
            const double base = delta * static_cast<double>(p) + zeta;
            for (usize q = q0; q < q1; ++q) {
                const double v = base + eps * static_cast<double>(q);
                double value[channel_count]{0.0, 0.0, 0.0, 0.0};
                usize count = 1;
                if (!canon) {
                    double t = 0.0;
                    entries[0] = RowCellWeights{};
                    locate(v, rows - 1, entries[0].cell, t);
                    kernel_weights(t, entries[0].linear);
                    tail_weights(t, entries[0].tail);
                    entries[0].total = 1.0;
                } else {
                    count = interval_weights(v - hv, v + hv, rows, entries, composite_room);
                    if (count > composite_room) count = composite_room;
                }
                if (!canon && !interior_cell(entries[0].cell, rows)) {
                    // As Field::evaluate: an end cell through vertical_value, rows outside read 0.
                    const usize j = entries[0].cell;
                    for (usize c = 0; c < channel_count; ++c) {
                        double window[8];
                        for (usize k = 0; k < 8; ++k) {
                            const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
                            window[k] = row < 0 || static_cast<usize>(row) >= rows ? 0.0 : column[static_cast<usize>(row) * channel_count + c];
                        }
                        value[c] = vertical_value(field, j, window, entries[0].linear, entries[0].tail);
                    }
                    count = 0;
                }
                for (usize e = 0; e < count; ++e) {
                    const RowCellWeights& entry = entries[e];
                    const usize j = entry.cell;
                    for (usize c = 0; c < channel_count; ++c) {
                        double part = 0.0;
                        if (simple[j * channel_count + c] != 0) {
                            for (usize r = 0; r < 6; ++r) part = part + entry.linear[r] * column[(j + r - 2) * channel_count + c];
                        } else {
                            const double* const cell = cells + (j * channel_count + c) * 6;
                            part = entry.total * cell[0];
                            for (usize k = 0; k < 5; ++k) part = part + entry.tail[k] * cell[k + 1];
                        }
                        value[c] = value[c] + part;
                    }
                }
                const usize x = by_columns ? p : q;
                const usize y = by_columns ? q : p;
                encode(value, pixel_at(destination, x, y));
            }
        }
    }
    return OperationStatus::success();
}

// The 2D field's affine transfer: the field at each pulled-back centre (preview) or its exact
// average over the pulled-back pixel (canon.cpp).
OperationStatus affine_field_state(const FieldState& field, const AffineMap& stored, const Mode mode, const MutableRgba8Image& destination,
                                   const Region& region, WorkspaceState& w) noexcept;

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

OperationStatus affine_field_state(const FieldState& field, const AffineMap& stored, const Mode mode, const MutableRgba8Image& destination,
                                   const Region& region, WorkspaceState& w) noexcept {
    if (mode == Mode::canon) return affine_canon(field, stored, destination, region, w);
    AnyMap any;
    any.kind = MapKind::affine;
    any.affine = &stored;
    return preview(field, any, destination, region, w);
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
    if (state.transposed && (mode == Mode::canon || (state.width >= 8 && state.height >= 8))) {
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
        const OperationStatus done = mode == Mode::canon ? detail::axis_canon(state, swapped, scratch, turned, w, false)
                                                         : detail::axis_preview(state, swapped, scratch, turned, w);
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
    if (state.width >= 8 && state.height >= 8) return detail::axis_preview(state, map, destination, region, *FieldAccess::state(workspace));
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
    // The composited transfer (two 1D passes); the 2D field's path for maps it does not take.
    const OperationStatus composited =
        detail::affine_composited_state(*FieldAccess::state(field), stored, mode, destination, region, *FieldAccess::state(workspace));
    if (composited.succeeded() || (*composited.error()).kind() != ErrorKind::unsupported) return composited;
    return detail::affine_field_state(*FieldAccess::state(field), stored, mode, destination, region, *FieldAccess::state(workspace));
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
    // Preview: x reads u = (x + 1/2) s - 1/2, the centre of canon's footprint
    // [x s - 1/2, (x + 1) s - 1/2], s = w / W. The union of both pixel ranges, grown by one.
    const double u0 = static_cast<double>(box.u0);
    const double u1 = static_cast<double>(box.u1);
    const double v0 = static_cast<double>(box.v0);
    const double v1 = static_cast<double>(box.v1);
    const double px0 = (u0 + 0.5) * W / w - 0.5;
    const double px1 = (u1 + 0.5) * W / w - 0.5;
    const double py0 = (v0 + 0.5) * H / h - 0.5;
    const double py1 = (v1 + 0.5) * H / h - 0.5;
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
    // The composited transfer reads, for a pixel at (u, v) in the stored orientation, the rows
    // within 4 nodes (plus the canon half-extent) of v, each at u + beta (r - v), widened by the
    // canon half-extent along u: so the box grows by that much more along the stored u.
    const bool transposed = source_height * 2 > source_width * 3;
    const double s11 = transposed ? map.a21 : map.a11;
    const double s12 = transposed ? map.a22 : map.a12;
    const double s21 = transposed ? map.a11 : map.a21;
    const double s22 = transposed ? map.a12 : map.a22;
    const double m21 = s21 < 0.0 ? 0.0 - s21 : s21;
    const double m22 = s22 < 0.0 ? 0.0 - s22 : s22;
    const bool by_columns = m22 > m21 || (m22 == m21 && !transposed);
    const double divisor = by_columns ? s22 : s21;
    double grow_u = 0.0;
    double grow_v = 0.0;
    if (divisor != 0.0) {
        const double beta = (by_columns ? s12 : s11) / divisor;
        const double alpha = by_columns ? s11 - s12 * s21 / s22 : s12 - s11 * s22 / s21;
        const double eps = by_columns ? m22 : m21;
        const double mb = beta < 0.0 ? 0.0 - beta : beta;
        const double ma = alpha < 0.0 ? 0.0 - alpha : alpha;
        grow_v = 0.5 * eps + 1.0;
        grow_u = mb * (5.0 + 0.5 * eps) + 0.5 * ma + 1.0;
        if (!(grow_u == grow_u) || grow_u > reach) grow_u = reach;
    }
    const double gx = transposed ? grow_v : grow_u;
    const double gy = transposed ? grow_u : grow_v;
    const double us[2]{box.u0 == 0 ? -reach : static_cast<double>(box.u0) - 2.0 - gx,
                       box.u1 + 1 >= source_width ? reach : static_cast<double>(box.u1) + 2.0 + gx};
    const double vs[2]{box.v0 == 0 ? -reach : static_cast<double>(box.v0) - 2.0 - gy,
                       box.v1 + 1 >= source_height ? reach : static_cast<double>(box.v1) + 2.0 + gy};
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

// The 2D field's affine transfer, which the public transfer falls back to: tests and measurements.
OperationStatus affine_field(const Field& field, const AffineMap& map, const Mode mode, const MutableRgba8Image& destination,
                             const Region& region, Workspace& workspace) noexcept {
    const OperationStatus common = check_common("stxlib.conv.transfer.affine", field, destination, region, workspace);
    if (common.failed()) return common;
    const bool valid = finite(map.a11) && finite(map.a12) && finite(map.a21) && finite(map.a22) && finite(map.ou) && finite(map.ov);
    if (!valid) return refused("stxlib.conv.transfer.affine", "the map's coefficients must be finite");
    AffineMap stored = map;
    if ((*FieldAccess::state(field)).transposed) {
        stored.a11 = map.a21;
        stored.a12 = map.a22;
        stored.a21 = map.a11;
        stored.a22 = map.a12;
        stored.ou = map.ov;
        stored.ov = map.ou;
    }
    return affine_field_state(*FieldAccess::state(field), stored, mode, destination, region, *FieldAccess::state(workspace));
}

// The composited affine transfer alone (no fallback): tests and measurements.
OperationStatus affine_composited(const Field& field, const AffineMap& map, const Mode mode, const MutableRgba8Image& destination,
                                  const Region& region, Workspace& workspace) noexcept {
    const OperationStatus common = check_common("stxlib.conv.transfer.affine_composited", field, destination, region, workspace);
    if (common.failed()) return common;
    const bool valid = finite(map.a11) && finite(map.a12) && finite(map.a21) && finite(map.a22) && finite(map.ou) && finite(map.ov);
    if (!valid) return refused("stxlib.conv.transfer.affine_composited", "the map's coefficients must be finite");
    AffineMap stored = map;
    if ((*FieldAccess::state(field)).transposed) {
        stored.a11 = map.a21;
        stored.a12 = map.a22;
        stored.a21 = map.a11;
        stored.a22 = map.a12;
        stored.ou = map.ov;
        stored.ov = map.ou;
    }
    return affine_composited_state(*FieldAccess::state(field), stored, mode, destination, region, *FieldAccess::state(workspace));
}
} // namespace detail

} // namespace stxlib::conv
