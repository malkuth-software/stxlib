// stxlib::conv transfers and Workspace (include/stxlib/conv.hpp).
//
// Preview reads the field at each destination pixel's pulled-back centre. Canon on an axis
// map is Fubini with combined weights (work/conv FFT_MATH.md Part III): each destination
// pixel's footprint is cut at source cells along each axis, three-point Gauss on each piece
// (exact for the quintic pieces), the row values computed once per u node for every source
// row, and each vertical cell settled once per u node and shared by every destination row
// that reads it, its nodes' weights combined.
//
// Every transfer validates everything before it writes: on failure no pixel of the region
// has been written.

#include "canon.hpp"
#include "field_state.hpp"

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

// Gauss-Legendre, three points on [-1, 1].
constexpr double gauss_x[3]{-0.7745966692414834, 0.0, 0.7745966692414834};
constexpr double gauss_w[3]{5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};

// A quadrature node of a footprint: its source cell, phase and weight.
struct FootprintNode final {
    usize cell{0};
    double t{0.0};
    double weight{0.0};
};

// Writes the nodes of target pixel `index`'s footprint [index s - 1/2, (index + 1) s - 1/2]
// (s = source / target), cut at source cells, into nodes (footprint_room); returns their count.
usize footprint(const usize index, const usize source, const usize target, FootprintNode* nodes) noexcept {
    const double scale = static_cast<double>(source) / static_cast<double>(target);
    const double left = static_cast<double>(index) * scale - 0.5;
    const double right = static_cast<double>(index + 1) * scale - 0.5;
    const double length = right - left;
    usize count = 0;
    double a = left;
    while (a < right) {
        // floor(a): a >= -1/2, so -1 below zero and truncation above.
        const double whole = a < 0.0 ? -1.0 : static_cast<double>(static_cast<u64>(a));
        double b = whole + 1.0;
        if (b > right) b = right;
        const double half = 0.5 * (b - a);
        const double mid = 0.5 * (a + b);
        for (usize k = 0; k < 3; ++k) {
            FootprintNode node;
            locate(mid + half * gauss_x[k], source - 1, node.cell, node.t);
            node.weight = gauss_w[k] * half / length;
            nodes[count] = node;
            count = count + 1;
        }
        a = b;
    }
    return count;
}

usize footprint_room(const usize source, const usize target) noexcept {
    const usize pieces = source / target + 3;
    return 3 * pieces;
}

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

// Settles vertical cell j of the current column in every channel: simple (the linear
// kernel) or its admitted cell (anchor and five currents).
void settle(const FieldState& field, const usize j, const double* column, u8* simple, double* cells) noexcept {
    const usize rows = field.height;
    const CellShape shape = cell_shape(j, rows);
    for (usize c = 0; c < channel_count; ++c) {
        double w[8];
        for (usize k = 0; k < 8; ++k) {
            const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
            const usize clamped = row < 0 ? 0 : static_cast<usize>(row) >= rows ? rows - 1 : static_cast<usize>(row);
            w[k] = column[clamped * channel_count + c];
        }
        double raw[5];
        double admitted[5];
        raw_for_shape(w, shape, raw);
        double* const cell = cells + (j * channel_count + c) * 6;
        simple[j * channel_count + c] = 0;
        if (interior_cell(j, rows)) {
            if (admit_cell(w, shape, raw, admitted, true)) {
                simple[j * channel_count + c] = 1;
                continue;
            }
        } else {
            (void)admit_cell(w, shape, raw, admitted, false);
        }
        cell[0] = w[3];
        for (usize m = 0; m < 5; ++m) cell[m + 1] = admitted[m];
    }
}

OperationStatus axis_canon(const FieldState& field, const AxisMap& map, const MutableRgba8Image& destination, const Region& region,
                           WorkspaceState& workspace) noexcept {
    const usize rows = field.height;
    const usize region_rows = region.y1 - region.y0;
    AxisCanonScratch scratch;
    const OperationStatus combined = combine_rows(rows, map.target_height, region, scratch);
    if (combined.failed()) return combined;
    Vector<FootprintNode> u_nodes;
    OperationStatus status = u_nodes.resize(footprint_room(field.width, map.target_width));
    if (status.succeeded()) status = workspace.column.resize(rows * channel_count);
    if (status.succeeded()) status = workspace.settled.resize(rows);
    if (status.succeeded()) status = workspace.simple.resize(rows * channel_count);
    if (status.succeeded()) status = workspace.cells.resize(rows * channel_count * 6);
    if (status.succeeded()) status = workspace.accumulator.resize(region_rows * channel_count);
    if (status.failed()) return exhausted("stxlib.conv.transfer");
    double* const column = workspace.column.mutable_view().data();
    u32* const settled = workspace.settled.mutable_view().data();
    u8* const simple = workspace.simple.mutable_view().data();
    double* const cells = workspace.cells.mutable_view().data();
    double* const accumulator = workspace.accumulator.mutable_view().data();
    for (usize j = 0; j < rows; ++j) settled[j] = 0;
    u32 stamp = 0;
    for (usize x = region.x0; x < region.x1; ++x) {
        for (usize k = 0; k < region_rows * channel_count; ++k) accumulator[k] = 0.0;
        FootprintNode* const nodes = u_nodes.mutable_view().data();
        const usize node_count = footprint(x, field.width, map.target_width, nodes);
        for (usize a = 0; a < node_count; ++a) {
            double su[6];
            double tauu[5];
            kernel_weights(nodes[a].t, su);
            tail_weights(nodes[a].t, tauu);
            for (usize r = 0; r < rows; ++r) row_value(field, r, nodes[a].cell, su, tauu, column + r * channel_count);
            if (stamp == 0xffffffffu) {
                for (usize j = 0; j < rows; ++j) settled[j] = 0;
                stamp = 0;
            }
            stamp = stamp + 1;
            for (usize y = 0; y < region_rows; ++y) {
                double sum[channel_count]{0.0, 0.0, 0.0, 0.0};
                for (usize e = scratch.row_offset[y]; e < scratch.row_offset[y + 1]; ++e) {
                    const RowCellWeights& entry = scratch.rows[e];
                    const usize j = entry.cell;
                    if (settled[j] != stamp) {
                        settled[j] = stamp;
                        settle(field, j, column, simple, cells);
                    }
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
                    accumulator[y * channel_count + c] = accumulator[y * channel_count + c] + nodes[a].weight * sum[c];
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
        axis_at(static_cast<double>(field.width), static_cast<double>(field.height), static_cast<double>((*map.axis).target_width),
                static_cast<double>((*map.axis).target_height), x, y, u, v);
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

OperationStatus preview(const FieldState& field, const AnyMap& map, const MutableRgba8Image& destination, const Region& region) noexcept {
    for (usize y = region.y0; y < region.y1; ++y) {
        for (usize x = region.x0; x < region.x1; ++x) {
            double u = 0.0;
            double v = 0.0;
            pull_back(map, field, x, y, u, v);
            double value[channel_count];
            field_value(field, u, v, value);
            encode(value, pixel_at(destination, x, y));
        }
    }
    return OperationStatus::success();
}

OperationStatus canon_not_yet(const Operation operation) noexcept {
    return OperationStatus::failure(ErrorRecord::make(
        ErrorKind::unsupported, operation,
        ErrorDetail{.message = "projective canon is held: option (b) measured wrong near the horizon (work/conv PERSPECTIVE_OPTIONS.md)"}));
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
    const usize total = sizeof(WorkspaceState) + (w.values.capacity() + w.column.capacity() + w.cells.capacity() + w.accumulator.capacity()) * sizeof(double) +
                        w.settled.capacity() * sizeof(u32) + w.simple.capacity();
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
    if (mode == Mode::canon) return detail::axis_canon(state, map, destination, region, *FieldAccess::state(workspace));
    detail::AnyMap any;
    any.kind = detail::MapKind::axis;
    any.axis = &map;
    return detail::preview(state, any, destination, region);
}

OperationStatus transfer(const Field& field, const AffineMap& map, const Mode mode, const MutableRgba8Image& destination,
                         const Region& region, Workspace& workspace) noexcept {
    const OperationStatus common = detail::check_common("stxlib.conv.transfer.affine", field, destination, region, workspace);
    if (common.failed()) return common;
    const bool valid = detail::finite(map.a11) && detail::finite(map.a12) && detail::finite(map.a21) && detail::finite(map.a22) &&
                       detail::finite(map.ou) && detail::finite(map.ov);
    if (!valid) return detail::refused("stxlib.conv.transfer.affine", "the map's coefficients must be finite");
    if (mode == Mode::canon) {
        return detail::affine_canon(*FieldAccess::state(field), map, destination, region, *FieldAccess::state(workspace));
    }
    detail::AnyMap any;
    any.kind = detail::MapKind::affine;
    any.affine = &map;
    return detail::preview(*FieldAccess::state(field), any, destination, region);
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
    if (mode == Mode::canon) {
        return detail::mesh_canon(*FieldAccess::state(field), map, destination, region, *FieldAccess::state(workspace));
    }
    detail::AnyMap any;
    any.kind = detail::MapKind::mesh;
    any.mesh = &map;
    return detail::preview(*FieldAccess::state(field), any, destination, region);
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
    if (mode == Mode::canon) return detail::canon_not_yet("stxlib.conv.transfer.projective");
    detail::AnyMap any;
    any.kind = detail::MapKind::projective;
    any.projective = &map;
    return detail::preview(*FieldAccess::state(field), any, destination, region);
}

} // namespace stxlib::conv
