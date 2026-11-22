#pragma once

// stxlib::conv: the canonical CONV field and its transfers (owner, 2026-11-22: one canonical
// CONV for every program, a tight standard function in STXlib). Design and evidence:
// work/conv/API_DESIGN.md, FFT_MATH.md (the separation, the exact kernel, the canon),
// SIMD_DESIGN.md (lanes and tiers).
//
// A Field is compiled once from an RGBA8 image (straight alpha). It keeps a copy of the
// source and the sparse admission corrections of its rows, nothing else: the separated form
// of the paper's ordered CONV surface (rows first, then columns; paper closure; no bubble).
// Transfers read a field through a map, in one of two modes:
//
//   preview  the field at each destination pixel's pulled-back centre (fast);
//   canon    the field's average over each destination pixel's pulled-back footprint
//            (exact for the piecewise-polynomial canon field on axis and affine maps and
//            meshes; perspective by subdivision with a stated density bound).
//
// Every transfer writes only inside its destination region, so a caller updates the part of
// a destination that a moved mesh node changed, and splits a destination into regions to run
// them on separate threads (one workspace per thread).
//
// Floating point (docs/CODE_STYLE_APPENDIX.md, A1): binary64, no contraction, no fused
// multiply-add; every SIMD tier (stxlib/simd.hpp) gives the scalar tier's bits. Inputs are
// bytes, so no subnormal is read; a subnormal intermediate is not produced on the tested
// fixtures, and the proof obligation is recorded in proofs/REPORT.md (open).

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::conv {

inline constexpr usize channels = 4;
// The largest side a field or destination takes.
inline constexpr usize max_side = usize{1} << 15;

enum class Mode : u8 {
    preview = 0,
    canon = 1,
};

// An RGBA8 image: four bytes per pixel, straight alpha, rows row_stride bytes apart.
struct Rgba8Image final {
    View<u8> pixels;
    usize width{0};
    usize height{0};
    usize row_stride{0};
};
struct MutableRgba8Image final {
    MutableView<u8> pixels;
    usize width{0};
    usize height{0};
    usize row_stride{0};
};

// Destination pixels [x0, x1) x [y0, y1).
struct Region final {
    usize x0{0};
    usize y0{0};
    usize x1{0};
    usize y1{0};
};

// Separable resampling to target_width x target_height. Preview: nodes aligned at both ends
// (target pixel x reads source node x (w - 1) / (W - 1)). Canon: each target pixel's
// footprint (x + [0, 1)) w / W - 1/2.
struct AxisMap final {
    usize target_width{0};
    usize target_height{0};
};

// Destination pixel centre (x, y) reads source node coordinates
// (ou + a11 x + a12 y, ov + a21 x + a22 y).
struct AffineMap final {
    double a11{1.0};
    double a12{0.0};
    double a21{0.0};
    double a22{1.0};
    double ou{0.0};
    double ov{0.0};
};

// A regular destination grid of nodes_x by nodes_y nodes, spacing pixels apart, each node
// carrying its source position: source_uv[2 (row nodes_x + column)] = u, [+ 1] = v. Each grid
// square is two triangles, mapped affinely. Any finite positions are valid (folds, collapsed
// triangles, positions outside the source).
struct MeshMap final {
    usize nodes_x{0};
    usize nodes_y{0};
    double spacing{1.0};
    View<double> source_uv;
};

// u = (h0 x + h1 y + h2) / w, v = (h3 x + h4 y + h5) / w, w = h6 x + h7 y + h8, required
// positive on the destination region.
struct ProjectiveMap final {
    double h[9]{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
};

namespace detail {
struct FieldState;
struct WorkspaceState;
struct FieldAccess;
} // namespace detail

// Options fixed at construction.
struct FieldOptions final {
    // The bounded odd bubble (owner, 2026-11-22: off by default, behind a flag). Not yet
    // implemented in the separated form: make refuses true with ErrorKind::unsupported.
    bool bubble{false};
};

// A compiled field. Move-only; the default field is empty and every operation refuses it.
class Field final {
public:
    Field() noexcept = default;
    ~Field();
    Field(const Field&) = delete;
    Field& operator=(const Field&) = delete;
    Field(Field&& other) noexcept;
    Field& operator=(Field&& other) noexcept;

    // A field for a width x height source (2 <= width, 2 <= height, both <= max_side).
    [[nodiscard]] static OperationResult<Field> make(usize width, usize height, const FieldOptions& options) noexcept;

    // Copies the source and settles every row's admission. The image must have the field's
    // size. On failure the field is left empty-of-content (not ready) and its storage is kept.
    [[nodiscard]] OperationStatus compile(const Rgba8Image& source) noexcept;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] usize width() const noexcept;
    [[nodiscard]] usize height() const noexcept;
    // The bytes this field owns (source copy, corrections, tables).
    [[nodiscard]] usize storage_bytes() const noexcept;

    // The premultiplied field value at source node coordinates (u, v), clamped to the source:
    // values[0..3] = premultiplied red, green, blue, alpha (unclamped). The Surface's value,
    // bit for bit (bubble off).
    [[nodiscard]] OperationStatus evaluate(double u, double v, MutableView<double> values) const noexcept;

private:
    explicit Field(detail::FieldState* state) noexcept : state_(state) {}
    void release() noexcept;
    detail::FieldState* state_{nullptr};
    friend struct detail::FieldAccess;
};

// Per-thread scratch for transfers (tile local fields, contest batches, canon cell pools).
// Move-only; one workspace serves one call at a time.
class Workspace final {
public:
    Workspace() noexcept = default;
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;
    Workspace(Workspace&& other) noexcept;
    Workspace& operator=(Workspace&& other) noexcept;

    [[nodiscard]] static OperationResult<Workspace> make() noexcept;
    [[nodiscard]] usize storage_bytes() const noexcept;

private:
    explicit Workspace(detail::WorkspaceState* state) noexcept : state_(state) {}
    void release() noexcept;
    detail::WorkspaceState* state_{nullptr};
    friend struct detail::FieldAccess;
};

// ---------------------------------------------------------------- lines (1D)
//
// A line field: `nodes` samples of `lanes` doubles each (node k, lane l at
// values[k node_stride + l]), compiled with the same arithmetic as a 2D field's rows (paper
// closure, no bubble). The owner, 2026-11-23: one API for 1D and 2D, the line as its own field.

inline constexpr usize max_line_lanes = 64;
// The largest line a field takes.
inline constexpr usize max_line_nodes = usize{1} << 28;

struct LineSamples final {
    View<double> values;
    usize nodes{0};
    usize lanes{0};
    usize node_stride{0};
};
struct MutableLineSamples final {
    MutableView<double> values;
    usize nodes{0};
    usize lanes{0};
    usize node_stride{0};
};

namespace detail {
struct LineFieldState;
} // namespace detail

class LineField final {
public:
    LineField() noexcept = default;
    ~LineField();
    LineField(const LineField&) = delete;
    LineField& operator=(const LineField&) = delete;
    LineField(LineField&& other) noexcept;
    LineField& operator=(LineField&& other) noexcept;

    // A field for 2 <= nodes <= max_line_nodes samples of 1 <= lanes <= max_line_lanes.
    [[nodiscard]] static OperationResult<LineField> make(usize nodes, usize lanes, const FieldOptions& options) noexcept;
    // Settles every cell. Samples must be finite and have the field's nodes and lanes. On
    // failure the field is not ready and keeps its storage.
    [[nodiscard]] OperationStatus compile(const LineSamples& samples) noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] usize nodes() const noexcept;
    [[nodiscard]] usize lanes() const noexcept;
    [[nodiscard]] usize storage_bytes() const noexcept;
    // The field at node coordinate x (clamped to [0, nodes - 1]), every lane.
    [[nodiscard]] OperationStatus evaluate(double x, MutableView<double> values) const noexcept;

private:
    explicit LineField(detail::LineFieldState* state) noexcept : state_(state) {}
    void release() noexcept;
    detail::LineFieldState* state_{nullptr};
    friend struct detail::FieldAccess;
};

// Resamples to target.nodes samples. Preview: nodes aligned at both ends (target k reads x =
// k (n - 1) / (N - 1)). Canon: the field's average over each target sample's footprint
// (k + [0, 1)) n / N - 1/2, exact for the quintic cells. Lanes must match. On failure nothing
// has been written.
[[nodiscard]] OperationStatus transfer(const LineField& field, Mode mode, const MutableLineSamples& target) noexcept;
// target sample k = the field at positions[k] (finite; clamped). On failure nothing has been
// written.
[[nodiscard]] OperationStatus transfer_points(const LineField& field, View<double> positions, const MutableLineSamples& target) noexcept;

// Transfers. The destination region must lie inside the destination image; the destination
// must not overlap the field's storage. Output is straight-alpha RGBA8 (premultiplied value
// clamped to [0, alpha], alpha to [0, 1], then divided). On failure no pixel of the region has
// been written.
[[nodiscard]] OperationStatus transfer(const Field& field, const AxisMap& map, Mode mode, const MutableRgba8Image& destination,
                                       const Region& region, Workspace& workspace) noexcept;
[[nodiscard]] OperationStatus transfer(const Field& field, const AffineMap& map, Mode mode, const MutableRgba8Image& destination,
                                       const Region& region, Workspace& workspace) noexcept;
[[nodiscard]] OperationStatus transfer(const Field& field, const MeshMap& map, Mode mode, const MutableRgba8Image& destination,
                                       const Region& region, Workspace& workspace) noexcept;
[[nodiscard]] OperationStatus transfer(const Field& field, const ProjectiveMap& map, Mode mode, const MutableRgba8Image& destination,
                                       const Region& region, Workspace& workspace) noexcept;

} // namespace stxlib::conv
