// stxlib::conv::Field (include/stxlib/conv.hpp): compile and evaluation.
//
// Compile settles each source row once. A row's interior cells go through K1 per channel
// (pass A lists the cells whose raw currents are not one-signed; pass B projects the listed
// ones and reports which stayed inside their box); its end cells are compiled whole, as
// stx_conv's compile_line_cell does. Evaluation is the separated field (work/conv FFT_MATH.md
// Part II): row values at the eight rows around v (linear kernel plus the stored
// corrections), then the vertical cell, admitted on the spot.

#include "field_state.hpp"

#include "stxlib/memory.hpp"
#include "stxlib/simd.hpp"

#include <new>

namespace stxlib::conv::detail {

namespace {

// Compile scratch, released when compile returns: four decoded row planes (one padding
// sample at each end), K1's raw currents, list, gathered windows, admitted currents and idle
// bytes, and per channel the listed interior cells with their corrections.
struct CompileScratch final {
    Vector<double> planes;
    Vector<double> raw;
    Vector<u32> list;
    Vector<double> gathered;
    Vector<double> admitted;
    Vector<u8> idle;
    Vector<u32> listed_cells;
    Vector<double> listed_e;
    usize listed_count[channel_count]{0, 0, 0, 0};
};

OperationStatus sized(Vector<double>& vector, const usize count) noexcept { return vector.resize(count); }

OperationStatus prepare(CompileScratch& scratch, const usize width) noexcept {
    const usize interior = width >= 6 ? width - 5 : 0;
    const usize lanes = interior > 0 ? interior : 1;
    OperationStatus status = sized(scratch.planes, channel_count * (width + 2));
    if (status.succeeded()) status = sized(scratch.raw, 5 * lanes);
    if (status.succeeded()) status = scratch.list.resize(lanes);
    if (status.succeeded()) status = sized(scratch.gathered, 4 * lanes);
    if (status.succeeded()) status = sized(scratch.admitted, 5 * lanes);
    if (status.succeeded()) status = scratch.idle.resize(lanes);
    if (status.succeeded()) status = scratch.listed_cells.resize(channel_count * lanes);
    if (status.succeeded()) status = sized(scratch.listed_e, channel_count * 5 * lanes);
    return status;
}

// Decodes row y into the four planes: plane c holds x = -1 .. width (ends repeated).
void decode_row(const FieldState& field, const usize y, double* planes) noexcept {
    const usize width = field.width;
    const usize stride = width + 2;
    const u8* const row = field.source.view().data() + y * width * 4;
    for (usize x = 0; x < width; ++x) {
        double value[channel_count];
        decode(row + x * 4, value);
        for (usize c = 0; c < channel_count; ++c) planes[c * stride + x + 1] = value[c];
    }
    for (usize c = 0; c < channel_count; ++c) {
        planes[c * stride] = planes[c * stride + 1];
        planes[c * stride + width + 1] = planes[c * stride + width];
    }
}

// Lists channel c's contested interior cells of the decoded row (cells 2 .. width - 4).
void contest_interior(const Kernels& kernels, const usize width, const double* plane, CompileScratch& scratch, const usize c) noexcept {
    const usize interior = width >= 6 ? width - 5 : 0;
    scratch.listed_count[c] = 0;
    if (interior == 0) return;
    const usize lanes = interior;
    // Lane l is cell 2 + l; window[k][l] = y[cell - 3 + k] = plane[cell - 3 + k + 1].
    const double* window[8];
    for (usize k = 0; k < 8; ++k) window[k] = plane + k;
    double* const raw_base = scratch.raw.mutable_view().data();
    double* raw[5];
    for (usize r = 0; r < 5; ++r) raw[r] = raw_base + r * lanes;
    u32* const list = scratch.list.mutable_view().data();
    const usize count = kernels.pass_a(window, raw, list, interior);
    if (count == 0) return;
    // Pass B reads samples 2 .. 5 of each window and the raw currents, gathered compact.
    double* const gathered = scratch.gathered.mutable_view().data();
    double* const admitted_base = scratch.admitted.mutable_view().data();
    const double* gathered_window[8];
    for (usize k = 0; k < 4; ++k) gathered_window[k + 2] = gathered + k * lanes;
    gathered_window[0] = gathered_window[2];
    gathered_window[1] = gathered_window[2];
    gathered_window[6] = gathered_window[5];
    gathered_window[7] = gathered_window[5];
    // The raw currents of the listed lanes are gathered into the admitted planes' room first
    // and then moved into the raw planes' front, which pass A no longer needs past count.
    for (usize q = 0; q < count; ++q) {
        const usize lane = list[q];
        for (usize k = 0; k < 4; ++k) gathered[k * lanes + q] = window[k + 2][lane];
        for (usize r = 0; r < 5; ++r) admitted_base[r * lanes + q] = raw[r][lane];
    }
    for (usize r = 0; r < 5; ++r) {
        for (usize q = 0; q < count; ++q) raw[r][q] = admitted_base[r * lanes + q];
    }
    const double* compact_raw[5];
    double* admitted[5];
    for (usize r = 0; r < 5; ++r) {
        compact_raw[r] = raw[r];
        admitted[r] = admitted_base + r * lanes;
    }
    u8* const idle = scratch.idle.mutable_view().data();
    kernels.pass_b(gathered_window, compact_raw, admitted, idle, count);
    u32* const cells = scratch.listed_cells.mutable_view().data() + c * lanes;
    double* const e = scratch.listed_e.mutable_view().data() + c * 5 * lanes;
    usize kept = 0;
    for (usize q = 0; q < count; ++q) {
        if (idle[q] != 0) continue;
        cells[kept] = static_cast<u32>(list[q] + 2);
        for (usize r = 0; r < 5; ++r) e[kept * 5 + r] = admitted[r][q] - compact_raw[r][q];
        kept = kept + 1;
    }
    scratch.listed_count[c] = kept;
}

OperationStatus push_end_cell(FieldState& field, const double* planes, const usize cell) noexcept {
    const usize width = field.width;
    const usize stride = width + 2;
    const CellShape shape = cell_shape(cell, width);
    for (usize c = 0; c < channel_count; ++c) {
        double window[8];
        for (usize k = 0; k < 8; ++k) {
            const i64 x = static_cast<i64>(cell) - 3 + static_cast<i64>(k);
            const usize clamped = x < 0 ? 0 : static_cast<usize>(x) >= width ? width - 1 : static_cast<usize>(x);
            window[k] = planes[c * stride + clamped + 1];
        }
        double raw[5];
        Correction entry;
        entry.cell = static_cast<u32>(cell);
        entry.channel = static_cast<u32>(c);
        raw_for_shape(window, shape, raw);
        (void)admit_cell(window, shape, raw, entry.e, false);
        const OperationStatus pushed = field.corrections.push(static_cast<Correction&&>(entry));
        if (pushed.failed()) return pushed;
    }
    return OperationStatus::success();
}

// Appends row y's run list: end cells, then the interior cells merged across channels in
// (cell, channel) order, then the far end cells.
OperationStatus compile_row(FieldState& field, CompileScratch& scratch, const usize y) noexcept {
    const usize width = field.width;
    const usize cells = width - 1;
    const usize stride = width + 2;
    double* const planes = scratch.planes.mutable_view().data();
    decode_row(field, y, planes);
    for (usize c = 0; c < channel_count; ++c) contest_interior(*field.kernels, width, planes + c * stride, scratch, c);
    const usize interior = width >= 6 ? width - 5 : 0;
    const usize lanes = interior > 0 ? interior : 1;
    // Cells before the interior: 0 and 1 (fewer when the line is short).
    const usize first_interior = interior > 0 ? 2 : cells;
    for (usize cell = 0; cell < first_interior && cell < cells; ++cell) {
        const OperationStatus pushed = push_end_cell(field, planes, cell);
        if (pushed.failed()) return pushed;
    }
    usize next[channel_count]{0, 0, 0, 0};
    for (;;) {
        u32 lowest = 0xffffffffu;
        for (usize c = 0; c < channel_count; ++c) {
            if (next[c] < scratch.listed_count[c]) {
                const u32 cell = scratch.listed_cells.view().data()[c * lanes + next[c]];
                lowest = cell < lowest ? cell : lowest;
            }
        }
        if (lowest == 0xffffffffu) break;
        for (usize c = 0; c < channel_count; ++c) {
            if (next[c] >= scratch.listed_count[c]) continue;
            const u32 cell = scratch.listed_cells.view().data()[c * lanes + next[c]];
            if (cell != lowest) continue;
            Correction entry;
            entry.cell = cell;
            entry.channel = static_cast<u32>(c);
            const double* const e = scratch.listed_e.view().data() + (c * lanes + next[c]) * 5;
            for (usize r = 0; r < 5; ++r) entry.e[r] = e[r];
            const OperationStatus pushed = field.corrections.push(static_cast<Correction&&>(entry));
            if (pushed.failed()) return pushed;
            next[c] = next[c] + 1;
        }
    }
    for (usize cell = interior > 0 ? width - 3 : cells; cell < cells; ++cell) {
        const OperationStatus pushed = push_end_cell(field, planes, cell);
        if (pushed.failed()) return pushed;
    }
    return OperationStatus::success();
}

// The listed correction of (y, i, c), or null.
const Correction* find(const FieldState& field, const usize y, const usize i, const usize c) noexcept {
    const Correction* const all = field.corrections.view().data();
    usize low = field.row_start.view().data()[y];
    usize high = field.row_start.view().data()[y + 1];
    // First entry with cell >= i.
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (all[middle].cell < i) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const usize end = field.row_start.view().data()[y + 1];
    for (usize k = low; k < end && all[k].cell == i; ++k) {
        if (all[k].channel == c) return all + k;
    }
    return nullptr;
}

} // namespace

void row_value(const FieldState& field, const usize y, const usize i, const double* s, const double* tau, double* out) noexcept {
    (void)s;
    const usize width = field.width;
    const bool end = !interior_cell(i, width);
    const u8* const row = field.source.view().data() + y * width * 4;
    // Interior cells: anchor plus (raw + e) . tau, raw in K2 row_cells' operation order, so the
    // tiled preview (transfer.cpp) and this point evaluation give the same bits. In exact
    // arithmetic the uncorrected form equals the linear kernel s_t over y[i-2 .. i+3].
    double window[6][channel_count];
    if (!end) {
        for (usize r = 0; r < 6; ++r) decode(row + (i - 2 + r) * 4, window[r]);
    }
    double anchor[channel_count];
    decode(row + i * 4, anchor);
    const CellShape interior_shape{};
    for (usize c = 0; c < channel_count; ++c) {
        const Correction* const k = find(field, y, i, c);
        double currents[5]{0.0, 0.0, 0.0, 0.0, 0.0};
        if (end) {
            for (usize m = 0; m < 5; ++m) currents[m] = k == nullptr ? 0.0 : (*k).e[m];
        } else {
            double w[8];
            w[0] = window[0][c];
            for (usize r = 0; r < 6; ++r) w[r + 1] = window[r][c];
            w[7] = window[5][c];
            raw_for_shape(w, interior_shape, currents);
            if (k != nullptr) {
                for (usize m = 0; m < 5; ++m) currents[m] = currents[m] + (*k).e[m];
            }
        }
        double value = anchor[c] + currents[0] * tau[0];
        value = value + currents[1] * tau[1];
        value = value + currents[2] * tau[2];
        value = value + currents[3] * tau[3];
        value = value + currents[4] * tau[4];
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
    (*state).width = width;
    (*state).height = height;
    (*state).kernels = &detail::kernels_for(simd::selected_level());
    // width, height <= 2^15, so width * height * 4 <= 2^32 fits usize on every target.
    const OperationStatus source = (*state).source.resize(width * height * 4);
    if (source.failed()) return OperationResult<Field>::failure(exhausted);
    const OperationStatus rows = (*state).row_start.resize(height + 1);
    if (rows.failed()) return OperationResult<Field>::failure(exhausted);
    return OperationResult<Field>::success(static_cast<Field&&>(field));
}

OperationStatus Field::compile(const Rgba8Image& source) noexcept {
    if (state_ == nullptr) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.compile",
                                                          ErrorDetail{.message = "empty field"}));
    }
    FieldState& field = *state_;
    if (source.width != field.width || source.height != field.height || source.row_stride < field.width * 4 ||
        source.pixels.count() < (field.height - 1) * source.row_stride + field.width * 4) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.conv.field.compile",
                                                          ErrorDetail{.message = "the image does not have the field's size"}));
    }
    field.ready = false;
    detail::CompileScratch scratch;
    if (detail::prepare(scratch, field.width).failed()) {
        return OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.field.compile"));
    }
    u8* const copy = field.source.mutable_view().data();
    const u8* const from = source.pixels.data();
    for (usize y = 0; y < field.height; ++y) {
        for (usize b = 0; b < field.width * 4; ++b) copy[y * field.width * 4 + b] = from[y * source.row_stride + b];
    }
    field.corrections.clear();
    usize* const row_start = field.row_start.mutable_view().data();
    for (usize y = 0; y < field.height; ++y) {
        row_start[y] = field.corrections.count();
        const OperationStatus row = detail::compile_row(field, scratch, y);
        if (row.failed()) {
            field.corrections.clear();
            return OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.field.compile"));
        }
    }
    row_start[field.height] = field.corrections.count();
    field.ready = true;
    return OperationStatus::success();
}

bool Field::ready() const noexcept { return state_ != nullptr && (*state_).ready; }
usize Field::width() const noexcept { return state_ == nullptr ? 0 : (*state_).width; }
usize Field::height() const noexcept { return state_ == nullptr ? 0 : (*state_).height; }

usize Field::storage_bytes() const noexcept {
    if (state_ == nullptr) return 0;
    const FieldState& field = *state_;
    const usize total = sizeof(FieldState) + field.source.capacity() + field.row_start.capacity() * sizeof(usize) +
                        field.corrections.capacity() * sizeof(detail::Correction);
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
    detail::field_value(*state_, u, v, values.data());
    return OperationStatus::success();
}

} // namespace stxlib::conv
