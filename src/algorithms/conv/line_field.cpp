// stxlib::conv::LineField (include/stxlib/conv.hpp): the 1D field and its transfers.
//
// The arithmetic of a 2D field's rows (field.cpp row_value): anchor plus currents . tau, the
// currents being raw (admission idle), raw + e (contested; e = admitted - raw), or an end
// cell's admitted currents (stx_conv's compile_line_cell). Cells are settled once, at compile, and kept densely: a line is small
// next to an image, and dense storage makes evaluation one lookup.

#include "field_state.hpp"
#include "footprint.hpp"

#include "stxlib/memory.hpp"

#include <new>

namespace stxlib::conv::detail {

namespace {

bool finite_value(const double x) noexcept { return x == x && x - x == 0.0; }

// The value of cell i at tails tau, lane l.
double line_value(const LineFieldState& field, const usize i, const double* tau, const usize l) noexcept {
    const usize slot = i * field.lanes + l;
    const double* const k = field.values.view().data() + slot * 5;
    double value = field.samples.view().data()[slot] + k[0] * tau[0];
    value = value + k[1] * tau[1];
    value = value + k[2] * tau[2];
    value = value + k[3] * tau[3];
    value = value + k[4] * tau[4];
    return value;
}

void line_at(const LineFieldState& field, const double x, double* out) noexcept {
    usize i = 0;
    double t = 0.0;
    locate(x, field.nodes - 1, i, t);
    double tau[5];
    tail_weights(t, tau);
    for (usize l = 0; l < field.lanes; ++l) out[l] = line_value(field, i, tau, l);
}

OperationStatus refused(const Operation operation, const TextView message) noexcept {
    return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, operation, ErrorDetail{.message = message}));
}

bool target_holds(const MutableLineSamples& target) noexcept {
    if (target.nodes == 0 || target.lanes == 0 || target.node_stride < target.lanes) return false;
    if (target.nodes > max_line_nodes) return false;
    return target.values.count() >= (target.nodes - 1) * target.node_stride + target.lanes;
}

} // namespace

} // namespace stxlib::conv::detail

namespace stxlib::conv {

using detail::FieldAccess;
using detail::LineFieldState;

LineField::~LineField() { release(); }

LineField::LineField(LineField&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

LineField& LineField::operator=(LineField&& other) noexcept {
    if (this != &other) {
        release();
        state_ = other.state_;
        other.state_ = nullptr;
    }
    return *this;
}

void LineField::release() noexcept {
    LineFieldState* const state = state_;
    state_ = nullptr;
    if (state == nullptr) return;
    (*state).~LineFieldState();
    memory::release(state);
}

OperationResult<LineField> LineField::make(const usize nodes, const usize lanes, const FieldOptions& options) noexcept {
    if (options.bubble) {
        return OperationResult<LineField>::failure(ErrorRecord::make(
            ErrorKind::unsupported, "stxlib.conv.line.make", ErrorDetail{.message = "the bounded odd bubble is not implemented"}));
    }
    if (nodes < 2 || nodes > max_line_nodes || lanes == 0 || lanes > max_line_lanes) {
        return OperationResult<LineField>::failure(ErrorRecord::make(
            ErrorKind::invalid_argument, "stxlib.conv.line.make",
            ErrorDetail{.message = "nodes must be in [2, max_line_nodes] and lanes in [1, max_line_lanes]"}));
    }
    const ErrorRecord exhausted = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.line.make");
    void* const block = memory::allocate(sizeof(LineFieldState), alignof(LineFieldState));
    if (block == nullptr) return OperationResult<LineField>::failure(exhausted);
    LineFieldState* const state = ::new (block) LineFieldState();
    LineField field(state);
    (*state).nodes = nodes;
    (*state).lanes = lanes;
    // nodes <= 2^28 and lanes <= 2^6: every product below fits 64 bits; on a 32-bit target the
    // allocation itself refuses what does not fit.
    OperationStatus status = (*state).samples.resize(nodes * lanes);
    if (status.succeeded()) status = (*state).values.resize((nodes - 1) * lanes * 5);
    if (status.failed()) return OperationResult<LineField>::failure(exhausted);
    return OperationResult<LineField>::success(static_cast<LineField&&>(field));
}

OperationStatus LineField::compile(const LineSamples& samples) noexcept {
    if (state_ == nullptr) return detail::refused("stxlib.conv.line.compile", "empty field");
    LineFieldState& field = *state_;
    const usize n = field.nodes;
    const usize lanes = field.lanes;
    if (samples.nodes != n || samples.lanes != lanes || samples.node_stride < lanes ||
        samples.values.count() < (n - 1) * samples.node_stride + lanes) {
        return detail::refused("stxlib.conv.line.compile", "the samples do not have the field's nodes and lanes");
    }
    const double* const from = samples.values.data();
    for (usize k = 0; k < n; ++k) {
        for (usize l = 0; l < lanes; ++l) {
            if (!detail::finite_value(from[k * samples.node_stride + l])) {
                return detail::refused("stxlib.conv.line.compile", "every sample must be finite");
            }
        }
    }
    field.ready = false;
    double* const y = field.samples.mutable_view().data();
    for (usize k = 0; k < n; ++k) {
        for (usize l = 0; l < lanes; ++l) y[k * lanes + l] = from[k * samples.node_stride + l];
    }
    double* const values = field.values.mutable_view().data();
    for (usize i = 0; i + 1 < n; ++i) {
        const detail::CellShape shape = detail::cell_shape(i, n);
        const bool interior = detail::interior_cell(i, n);
        for (usize l = 0; l < lanes; ++l) {
            double window[8];
            for (usize k = 0; k < 8; ++k) {
                const i64 x = static_cast<i64>(i) - 3 + static_cast<i64>(k);
                const usize clamped = x < 0 ? 0 : static_cast<usize>(x) >= n ? n - 1 : static_cast<usize>(x);
                window[k] = y[clamped * lanes + l];
            }
            double raw[5];
            double admitted[5];
            detail::raw_for_shape(window, shape, raw);
            const usize slot = i * lanes + l;
            double* const k = values + slot * 5;
            if (interior) {
                if (detail::admit_cell(window, shape, raw, admitted, true)) {
                    for (usize m = 0; m < 5; ++m) k[m] = raw[m];
                    continue;
                }
                // raw + e, e = admitted - raw: the 2D field's rows add the stored e to raw.
                for (usize m = 0; m < 5; ++m) k[m] = raw[m] + (admitted[m] - raw[m]);
                continue;
            }
            (void)detail::admit_cell(window, shape, raw, admitted, false);
            for (usize m = 0; m < 5; ++m) k[m] = admitted[m];
        }
    }
    field.ready = true;
    return OperationStatus::success();
}

bool LineField::ready() const noexcept { return state_ != nullptr && (*state_).ready; }
usize LineField::nodes() const noexcept { return state_ == nullptr ? 0 : (*state_).nodes; }
usize LineField::lanes() const noexcept { return state_ == nullptr ? 0 : (*state_).lanes; }

usize LineField::storage_bytes() const noexcept {
    if (state_ == nullptr) return 0;
    const LineFieldState& field = *state_;
    const usize total = sizeof(LineFieldState) + (field.samples.capacity() + field.values.capacity()) * sizeof(double);
    return total;
}

OperationStatus LineField::evaluate(const double x, const MutableView<double> values) const noexcept {
    if (!ready()) return detail::refused("stxlib.conv.line.evaluate", "the field is not compiled");
    if (values.count() < (*state_).lanes || !(x == x)) {
        return detail::refused("stxlib.conv.line.evaluate", "a value per lane and a finite coordinate are needed");
    }
    detail::line_at(*state_, x, values.data());
    return OperationStatus::success();
}

OperationStatus transfer(const LineField& field, const Mode mode, const MutableLineSamples& target) noexcept {
    if (!field.ready()) return detail::refused("stxlib.conv.line.transfer", "the field is not compiled");
    const LineFieldState& state = *FieldAccess::state(field);
    if (!detail::target_holds(target) || target.lanes != state.lanes) {
        return detail::refused("stxlib.conv.line.transfer", "the target does not hold its samples, or its lanes differ");
    }
    const usize n = state.nodes;
    const usize lanes = state.lanes;
    double* const out = target.values.data();
    if (mode == Mode::preview) {
        double value[max_line_lanes];
        for (usize k = 0; k < target.nodes; ++k) {
            const double x = (static_cast<double>(k) + 0.5) * static_cast<double>(n) / static_cast<double>(target.nodes) - 0.5;
            detail::line_at(state, x, value);
            for (usize l = 0; l < lanes; ++l) out[k * target.node_stride + l] = value[l];
        }
        return OperationStatus::success();
    }
    // Canon: the footprint nodes live on the stack (at most 3 (n / N + 3) nodes; refused past
    // the room below, which a shrink of more than 340 : 1 would need).
    constexpr usize room = 1024;
    if (detail::footprint_room(n, target.nodes) > room) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.conv.line.transfer",
                                                          ErrorDetail{.message = "canon shrinks a line by at most 340 : 1"}));
    }
    detail::FootprintNode nodes[room];
    for (usize k = 0; k < target.nodes; ++k) {
        const usize count = detail::footprint(k, n, target.nodes, nodes);
        double sum[max_line_lanes];
        for (usize l = 0; l < lanes; ++l) sum[l] = 0.0;
        for (usize a = 0; a < count; ++a) {
            double tau[5];
            detail::tail_weights(nodes[a].t, tau);
            for (usize l = 0; l < lanes; ++l) sum[l] = sum[l] + nodes[a].weight * detail::line_value(state, nodes[a].cell, tau, l);
        }
        for (usize l = 0; l < lanes; ++l) out[k * target.node_stride + l] = sum[l];
    }
    return OperationStatus::success();
}

OperationStatus transfer_points(const LineField& field, const View<double> positions, const MutableLineSamples& target) noexcept {
    if (!field.ready()) return detail::refused("stxlib.conv.line.points", "the field is not compiled");
    const LineFieldState& state = *FieldAccess::state(field);
    if (!detail::target_holds(target) || target.lanes != state.lanes || positions.count() != target.nodes) {
        return detail::refused("stxlib.conv.line.points", "one position per target sample, and matching lanes, are needed");
    }
    const double* const x = positions.data();
    for (usize k = 0; k < positions.count(); ++k) {
        if (!detail::finite_value(x[k])) return detail::refused("stxlib.conv.line.points", "every position must be finite");
    }
    double* const out = target.values.data();
    double value[max_line_lanes];
    for (usize k = 0; k < target.nodes; ++k) {
        detail::line_at(state, x[k], value);
        for (usize l = 0; l < state.lanes; ++l) out[k * target.node_stride + l] = value[l];
    }
    return OperationStatus::success();
}

} // namespace stxlib::conv
