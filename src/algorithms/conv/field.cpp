// stxlib::conv::Field (include/stxlib/conv.hpp): the source copy and evaluation.
//
// A field stores its source and nothing else. row_segment computes a row's cells on demand with
// the arithmetic compile used to store: a segment's interior cells go through K1 per channel
// (pass A lists the cells whose raw currents are not one-signed; pass B projects the listed ones
// and reports which stayed inside their box), and their K2 raw currents get e = admitted - raw
// where the contest acts; end cells are admitted whole, as stx_conv's compile_line_cell does.
// Evaluation is the separated field (work/conv FFT_MATH.md Part II): row values at the eight rows
// around v, then the vertical cell, admitted on the spot.

#include "field_state.hpp"

#include "stxlib/memory.hpp"
#include "stxlib/simd.hpp"

#include <new>

namespace stxlib::conv::detail {

OperationStatus prepare_segment(SegmentScratch& scratch, const usize capacity) noexcept {
    OperationStatus status = scratch.planes.resize(channel_count * (capacity + 7));
    if (status.succeeded()) status = scratch.decoded.resize((capacity + 5) * channel_count);
    if (status.succeeded()) status = scratch.raw.resize(5 * capacity);
    if (status.succeeded()) status = scratch.list.resize(capacity);
    if (status.succeeded()) status = scratch.gathered.resize(4 * capacity);
    if (status.succeeded()) status = scratch.admitted.resize(5 * capacity);
    if (status.succeeded()) status = scratch.compact.resize(5 * capacity);
    if (status.succeeded()) status = scratch.idle.resize(capacity);
    return status;
}

SegmentBuffers segment_buffers(SegmentScratch& scratch) noexcept {
    SegmentBuffers b;
    b.capacity = scratch.list.count();
    b.planes = scratch.planes.mutable_view().data();
    b.decoded = scratch.decoded.mutable_view().data();
    b.raw = scratch.raw.mutable_view().data();
    b.list = scratch.list.mutable_view().data();
    b.gathered = scratch.gathered.mutable_view().data();
    b.admitted = scratch.admitted.mutable_view().data();
    b.compact = scratch.compact.mutable_view().data();
    b.idle = scratch.idle.mutable_view().data();
    return b;
}

void row_segment(const FieldState& field, const usize r, const usize imin, const usize imax, double* const cells,
                 const SegmentBuffers& b) noexcept {
    const usize width = field.width;
    const usize n = imax - imin + 1;
    const usize stride = n + 7;
    const u8* const row = field.source.view().data() + r * width * 4;
    // Plane c, sample k: x = imin - 3 + k, clamped to the row (compile's planes repeated the end
    // samples once, and no window reaches further).
    for (usize k = 0; k < stride; ++k) {
        const i64 x = static_cast<i64>(imin) - 3 + static_cast<i64>(k);
        const usize clamped = x < 0 ? 0 : static_cast<usize>(x) >= width ? width - 1 : static_cast<usize>(x);
        double value[channel_count];
        decode(row + clamped * 4, value);
        for (usize c = 0; c < channel_count; ++c) b.planes[c * stride + k] = value[c];
    }
    const Kernels& kernels = *field.kernels;
    // Interior cells 2 .. width - 4 within the segment.
    const usize lo = imin > 2 ? imin : 2;
    const usize hi = width >= 6 ? (imax < width - 4 ? imax : width - 4) : 0;
    if (width >= 6 && lo <= hi) {
        const usize lanes = hi - lo + 1;
        // K2: pixels lo - 2 .. hi + 3, channel innermost: plane index k = x - (imin - 3).
        for (usize x = lo - 2; x <= hi + 3; ++x) {
            for (usize c = 0; c < channel_count; ++c) b.decoded[(x - (lo - 2)) * channel_count + c] = b.planes[c * stride + (x + 3 - imin)];
        }
        kernels.row_cells(b.decoded, lanes, cells + (lo - imin) * 24);
        for (usize c = 0; c < channel_count; ++c) {
            // Lane l is cell lo + l; window[k][l] = y[cell - 3 + k] = plane index (lo - imin) + l + k.
            const double* window[8];
            for (usize k = 0; k < 8; ++k) window[k] = b.planes + c * stride + (lo - imin) + k;
            double* raw[5];
            for (usize m = 0; m < 5; ++m) raw[m] = b.raw + m * lanes;
            const usize count = kernels.pass_a(window, raw, b.list, lanes);
            if (count == 0) continue;
            // Pass B reads samples 2 .. 5 of each window and the raw currents, gathered compact.
            const double* gathered_window[8];
            for (usize k = 0; k < 4; ++k) gathered_window[k + 2] = b.gathered + k * count;
            gathered_window[0] = gathered_window[2];
            gathered_window[1] = gathered_window[2];
            gathered_window[6] = gathered_window[5];
            gathered_window[7] = gathered_window[5];
            const double* compact_raw[5];
            double* admitted[5];
            for (usize m = 0; m < 5; ++m) {
                double* const plane = b.compact + m * count;
                compact_raw[m] = plane;
                admitted[m] = b.admitted + m * count;
            }
            for (usize q = 0; q < count; ++q) {
                const usize lane = b.list[q];
                for (usize k = 0; k < 4; ++k) b.gathered[k * count + q] = window[k + 2][lane];
                for (usize m = 0; m < 5; ++m) b.compact[m * count + q] = raw[m][lane];
            }
            kernels.pass_b(gathered_window, compact_raw, admitted, b.idle, count);
            for (usize q = 0; q < count; ++q) {
                if (b.idle[q] != 0) continue;
                double* const cell = cells + (lo + b.list[q] - imin) * 24;
                for (usize m = 0; m < 5; ++m) {
                    const double e = admitted[m][q] - compact_raw[m][q];
                    cell[(m + 1) * channel_count + c] = cell[(m + 1) * channel_count + c] + e;
                }
            }
        }
    }
    // End cells: the anchor and the admitted currents of the clamped window.
    for (usize i = imin; i <= imax; ++i) {
        if (width >= 6 && i >= lo && i <= hi) continue;
        double* const cell = cells + (i - imin) * 24;
        const CellShape shape = cell_shape(i, width);
        for (usize c = 0; c < channel_count; ++c) {
            double window[8];
            for (usize k = 0; k < 8; ++k) window[k] = b.planes[c * stride + (i - imin) + k];
            double raw[5];
            double admitted[5];
            raw_for_shape(window, shape, raw);
            (void)admit_cell(window, shape, raw, admitted, false);
            cell[c] = window[3];
            for (usize m = 0; m < 5; ++m) cell[(m + 1) * channel_count + c] = admitted[m];
        }
    }
}

void row_cell(const FieldState& field, const usize y, const usize i, double* cell) noexcept {
    SmallSegment small;
    row_segment(field, y, i, i, cell, small.buffers());
}

void row_value(const FieldState& field, const usize y, const usize i, const double* s, const double* tau, double* out) noexcept {
    (void)s;
    double cell[6 * channel_count];
    row_cell(field, y, i, cell);
    for (usize c = 0; c < channel_count; ++c) {
        double value = cell[c] + cell[1 * channel_count + c] * tau[0];
        value = value + cell[2 * channel_count + c] * tau[1];
        value = value + cell[3 * channel_count + c] * tau[2];
        value = value + cell[4 * channel_count + c] * tau[3];
        value = value + cell[5 * channel_count + c] * tau[4];
        out[c] = value;
    }
}

double vertical_value(const FieldState& field, const usize j, const double* w, const double* sv, const double* tauv) noexcept {
    const usize height = field.height;
    const CellShape shape = cell_shape(j, height);
    double raw[5];
    double admitted[5];
    raw_for_shape(w, shape, raw);
    if (interior_cell(j, height)) {
        if (admit_cell(w, shape, raw, admitted, true)) {
            double value = 0.0;
            for (usize r = 0; r < 6; ++r) value = value + sv[r] * w[r + 1];
            return value;
        }
        return evaluate_cell(w[3], admitted, tauv);
    }
    (void)admit_cell(w, shape, raw, admitted, false);
    return evaluate_cell(w[3], admitted, tauv);
}

void field_value(const FieldState& field, const double u, const double v, double* out) noexcept {
    usize i = 0;
    usize j = 0;
    double tu = 0.0;
    double tv = 0.0;
    locate(u, field.width - 1, i, tu);
    locate(v, field.height - 1, j, tv);
    double su[6];
    double tauu[5];
    double sv[6];
    double tauv[5];
    kernel_weights(tu, su);
    tail_weights(tu, tauu);
    kernel_weights(tv, sv);
    tail_weights(tv, tauv);
    double h[8][channel_count];
    for (usize k = 0; k < 8; ++k) {
        for (usize c = 0; c < channel_count; ++c) h[k][c] = 0.0;
        const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
        if (row < 0 || static_cast<usize>(row) >= field.height) continue;
        row_value(field, static_cast<usize>(row), i, su, tauu, h[k]);
    }
    for (usize c = 0; c < channel_count; ++c) {
        double w[8];
        for (usize k = 0; k < 8; ++k) w[k] = h[k][c];
        out[c] = vertical_value(field, j, w, sv, tauv);
    }
}

// Copies the source pixels of `rect` (source coordinates) into the stored orientation.
void copy_source(FieldState& field, const Rgba8Image& source, const Region& rect) noexcept {
    u8* const copy = field.source.mutable_view().data();
    const u8* const from = source.pixels.data();
    const usize stored_row = field.width * 4;
    for (usize y = rect.y0; y < rect.y1; ++y) {
        for (usize x = rect.x0; x < rect.x1; ++x) {
            const usize sx = field.transposed ? y : x;
            const usize sy = field.transposed ? x : y;
            for (usize k = 0; k < 4; ++k) copy[sy * stored_row + sx * 4 + k] = from[y * source.row_stride + x * 4 + k];
        }
    }
}

} // namespace stxlib::conv::detail

namespace stxlib::conv {

using detail::FieldState;

Field::~Field() { release(); }

Field::Field(Field&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Field& Field::operator=(Field&& other) noexcept {
    if (this != &other) {
        release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void Field::release() noexcept {
    // The owner lets go before it releases (AGENTS.md, termination rule 1).
    FieldState* const state = state_;
    state_ = nullptr;
    if (state == nullptr) return;
    (*state).~FieldState();
    memory::release(state);
}

OperationResult<Field> Field::make(const usize width, const usize height, const FieldOptions& options) noexcept {
    if (options.bubble) {
        return OperationResult<Field>::failure(ErrorRecord::make(
            ErrorKind::unsupported, "stxlib.conv.field.make",
            ErrorDetail{.message = "the bounded odd bubble is not implemented in the separated field"}));
    }
    if (width < 2 || height < 2 || width > max_side || height > max_side) {
        return OperationResult<Field>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.make",
                                                                 ErrorDetail{.message = "each side must be in [2, max_side]"}));
    }
    const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.field.make");
    void* const block = memory::allocate(sizeof(FieldState), alignof(FieldState));
    if (block == nullptr) return OperationResult<Field>::failure(exhausted);
    FieldState* const state = ::new (block) FieldState();
    Field field(state);
    // The longer axis first: a source more than 150% taller than wide is stored transposed.
    const bool transposed = height * 2 > width * 3;
    (*state).transposed = transposed;
    (*state).width = transposed ? height : width;
    (*state).height = transposed ? width : height;
    (*state).kernels = &detail::kernels_for(simd::selected_level());
    // width, height <= 2^15, so width * height * 4 <= 2^32 fits usize on every target.
    const OperationStatus source = (*state).source.resize(width * height * 4);
    if (source.failed()) return OperationResult<Field>::failure(exhausted);
    return OperationResult<Field>::success(static_cast<Field&&>(field));
}

OperationStatus Field::compile(const Rgba8Image& source) noexcept {
    if (state_ == nullptr) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.compile",
                                                          ErrorDetail{.message = "empty field"}));
    }
    FieldState& field = *state_;
    const usize sw = field.transposed ? field.height : field.width;
    const usize sh = field.transposed ? field.width : field.height;
    if (source.width != sw || source.height != sh || source.row_stride < sw * 4 || source.pixels.count() < (sh - 1) * source.row_stride + sw * 4) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.compile",
                                                          ErrorDetail{.message = "the image does not have the field's size"}));
    }
    detail::copy_source(field, source, Region{0, 0, sw, sh});
    field.ready = true;
    return OperationStatus::success();
}

OperationResult<NodeBox> Field::recompile(const Rgba8Image& source, const Region& changed) noexcept {
    if (!ready()) {
        return OperationResult<NodeBox>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.recompile",
                                                                   ErrorDetail{.message = "the field is not compiled"}));
    }
    FieldState& field = *state_;
    const usize sw = field.transposed ? field.height : field.width;
    const usize sh = field.transposed ? field.width : field.height;
    if (source.width != sw || source.height != sh || source.row_stride < sw * 4 || source.pixels.count() < (sh - 1) * source.row_stride + sw * 4) {
        return OperationResult<NodeBox>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.recompile",
                                                                   ErrorDetail{.message = "the image does not have the field's size"}));
    }
    if (changed.x0 > changed.x1 || changed.y0 > changed.y1 || changed.x1 > sw || changed.y1 > sh) {
        return OperationResult<NodeBox>::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.recompile",
                                                                   ErrorDetail{.message = "the changed region is not inside the source"}));
    }
    NodeBox box;
    if (changed.x0 == changed.x1 || changed.y0 == changed.y1) return OperationResult<NodeBox>::success(box);
    detail::copy_source(field, source, changed);
    // A pixel x reaches the cells i with i - 3 <= x <= i + 4 (the window y[i-3 .. i+4]), so the
    // values of u in [x - 4, x + 4]; likewise along v through the vertical windows. In the
    // source's own coordinates either way.
    box.u0 = changed.x0 >= 4 ? changed.x0 - 4 : 0;
    box.v0 = changed.y0 >= 4 ? changed.y0 - 4 : 0;
    box.u1 = changed.x1 + 3 < sw ? changed.x1 + 3 : sw - 1;
    box.v1 = changed.y1 + 3 < sh ? changed.y1 + 3 : sh - 1;
    box.empty = false;
    return OperationResult<NodeBox>::success(box);
}

bool Field::ready() const noexcept { return state_ != nullptr && (*state_).ready; }
usize Field::width() const noexcept { return state_ == nullptr ? 0 : (*state_).transposed ? (*state_).height : (*state_).width; }
usize Field::height() const noexcept { return state_ == nullptr ? 0 : (*state_).transposed ? (*state_).width : (*state_).height; }

usize Field::storage_bytes() const noexcept {
    if (state_ == nullptr) return 0;
    const FieldState& field = *state_;
    const usize total = sizeof(FieldState) + field.source.capacity();
    return total;
}

OperationStatus Field::evaluate(const double u, const double v, const MutableView<double> values) const noexcept {
    if (!ready()) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.evaluate",
                                                          ErrorDetail{.message = "the field is not compiled"}));
    }
    if (values.count() < channels || !(u == u) || !(v == v)) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.evaluate",
                                                          ErrorDetail{.message = "four values and finite coordinates are needed"}));
    }
    if ((*state_).transposed) {
        detail::field_value(*state_, v, u, values.data());
    } else {
        detail::field_value(*state_, u, v, values.data());
    }
    return OperationStatus::success();
}

} // namespace stxlib::conv
