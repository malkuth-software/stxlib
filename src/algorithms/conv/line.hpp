#pragma once

// stxlib::conv's scalar line arithmetic: decoding, the paper-closure jets of an open line, raw
// currents, the admission of any cell (interior or end), the linear kernel s_t, the tail
// weights, and encoding. Restated from the CONV paper's box factor and stx_conv's
// surface_kernel.hpp (STX 0fc2a32), same formulas and operation order. End cells are
// compiled exactly as stx_conv compiles them. Interior cells take K1's idle tests, which
// return the raw currents where the Surface's projection returns them up to rounding, so
// field values equal the Surface's to rounding and its encoded bytes on every measured
// fixture (work/conv/FFT_MATH.md Part II); tests/conv_tests.cpp holds golden digests.

#include "stxlib/types.hpp"

namespace stxlib::conv::detail {

inline constexpr usize channel_count = 4;

// Premultiplied decode of one straight RGBA8 pixel.
inline void decode(const u8* pixel, double* out) noexcept {
    const double alpha = static_cast<double>(pixel[3]) / 255.0;
    out[0] = static_cast<double>(pixel[0]) * alpha / 255.0;
    out[1] = static_cast<double>(pixel[1]) * alpha / 255.0;
    out[2] = static_cast<double>(pixel[2]) * alpha / 255.0;
    out[3] = alpha;
}

// Straight RGBA8 from a premultiplied value: alpha clamped to [0, 1], colour to [0, alpha].
// A pixel whose alpha byte is 0 is written 0, 0, 0, 0: its colour would be rounding noise
// divided by a vanishing alpha, and is invisible. (stx_conv's encode writes that noise; the
// bytes differ only at invisible pixels.) A non-finite value is encoded as transparent black.
inline void encode(const double* premultiplied, u8* pixel) noexcept {
    double alpha = premultiplied[3];
    alpha = alpha < 0.0 ? 0.0 : alpha > 1.0 ? 1.0 : alpha;
    if (!(alpha == alpha)) alpha = 0.0;
    if (static_cast<u8>(alpha * 255.0 + 0.5) == 0) {
        pixel[0] = 0;
        pixel[1] = 0;
        pixel[2] = 0;
        pixel[3] = 0;
        return;
    }
    for (usize c = 0; c < 3; ++c) {
        double colour = premultiplied[c];
        colour = colour < 0.0 ? 0.0 : colour > alpha ? alpha : colour;
        if (!(colour == colour)) colour = 0.0;
        const double straight = alpha > 0.0 ? colour / alpha : 0.0;
        pixel[c] = static_cast<u8>(straight * 255.0 + 0.5);
    }
    pixel[3] = static_cast<u8>(alpha * 255.0 + 0.5);
}

// The jets a node takes in an open line of n >= 2 nodes without seams (stx_conv's Jet and
// pick_jet, paper closure): the node is `before` nodes after the first and `after` before the
// last. In such a line a node's jet does not depend on the cell that sees it.
enum class Jet : u8 {
    interior = 0,
    paper_first,
    paper_central,
    paper_last,
    first4,
    second4,
    penultimate4,
    last4,
    first3,
    central3,
    last3,
    first2,
    last2,
};

inline Jet jet_of(const usize node, const usize n) noexcept {
    const usize before = node;
    const usize after = n - 1 - node;
    if (before >= 2 && after >= 2) return Jet::interior;
    const usize span = before + after;
    if (span >= 4) {
        if (before == 0) return Jet::paper_first;
        if (after == 0) return Jet::paper_last;
        return Jet::paper_central;
    }
    if (span >= 3) {
        if (before == 0) return Jet::first4;
        if (after == 0) return Jet::last4;
        if (before == 1) return Jet::second4;
        return Jet::penultimate4;
    }
    if (span == 2) {
        if (before == 0) return Jet::first3;
        if (after == 0) return Jet::last3;
        return Jet::central3;
    }
    return before == 0 ? Jet::first2 : Jet::last2;
}

// First and second jets at window[centre] (window = y[i-3 .. i+4], centre 3 or 4).
inline void node_jet(const double* window, const usize centre, const Jet kind, double& m, double& q) noexcept {
    const double before3 = window[centre >= 3 ? centre - 3 : 0];
    const double before2 = window[centre - 2];
    const double before1 = window[centre - 1];
    const double here = window[centre];
    const double after1 = window[centre + 1];
    const double after2 = window[centre + 2];
    const double after3 = window[centre + 3 <= 7 ? centre + 3 : 7];
    switch (kind) {
    case Jet::interior:
        m = (before2 - 8.0 * before1 + 8.0 * after1 - after2) / 12.0;
        q = (16.0 * after1 - after2 - 30.0 * here + 16.0 * before1 - before2) / 12.0;
        return;
    case Jet::paper_first:
        m = (4.0 * after1 - 3.0 * here - after2) / 2.0;
        q = 2.0 * here - 5.0 * after1 + 4.0 * after2 - after3;
        return;
    case Jet::paper_central:
    case Jet::central3:
        m = (after1 - before1) / 2.0;
        q = before1 - 2.0 * here + after1;
        return;
    case Jet::paper_last:
        m = (3.0 * here - 4.0 * before1 + before2) / 2.0;
        q = 2.0 * here - 5.0 * before1 + 4.0 * before2 - before3;
        return;
    case Jet::first4:
        m = (18.0 * after1 - 11.0 * here - 9.0 * after2 + 2.0 * after3) / 6.0;
        q = 2.0 * here - 5.0 * after1 + 4.0 * after2 - after3;
        return;
    case Jet::second4:
        m = (6.0 * after1 - 2.0 * before1 - 3.0 * here - after2) / 6.0;
        q = before1 - 2.0 * here + after1;
        return;
    case Jet::penultimate4:
        m = (2.0 * after1 + 3.0 * here - 6.0 * before1 + before2) / 6.0;
        q = before1 - 2.0 * here + after1;
        return;
    case Jet::last4:
        m = (11.0 * here - 18.0 * before1 + 9.0 * before2 - 2.0 * before3) / 6.0;
        q = 2.0 * here - 5.0 * before1 + 4.0 * before2 - before3;
        return;
    case Jet::first3:
        m = (4.0 * after1 - 3.0 * here - after2) / 2.0;
        q = here - 2.0 * after1 + after2;
        return;
    case Jet::last3:
        m = (3.0 * here - 4.0 * before1 + before2) / 2.0;
        q = here - 2.0 * before1 + before2;
        return;
    case Jet::first2:
        m = after1 - here;
        q = 0.0;
        return;
    case Jet::last2:
        m = here - before1;
        q = 0.0;
        return;
    }
    // Unreachable for a Jet made by jet_of; the secant, as stx_conv's fallback.
    m = after1 - here;
    q = 0.0;
}

// The shape of cell i of an open line of n >= 2 nodes: its two jets and whether it has neighbours.
struct CellShape final {
    Jet left{Jet::interior};
    Jet right{Jet::interior};
    bool has_previous{true};
    bool has_next{true};
};

inline CellShape cell_shape(const usize cell, const usize n) noexcept {
    CellShape shape;
    shape.left = jet_of(cell, n);
    shape.right = jet_of(cell + 1, n);
    shape.has_previous = cell > 0;
    shape.has_next = cell + 2 < n;
    return shape;
}

// Interior cells (both jets interior): 2 <= cell and cell + 4 <= n.
inline bool interior_cell(const usize cell, const usize n) noexcept { return cell >= 2 && cell + 4 <= n; }

inline void raw_for_shape(const double* window, const CellShape& shape, double* raw) noexcept {
    double left_m = 0.0;
    double left_q = 0.0;
    double right_m = 0.0;
    double right_q = 0.0;
    node_jet(window, 3, shape.left, left_m, left_q);
    node_jet(window, 4, shape.right, right_m, right_q);
    const double delta = window[4] - window[3];
    raw[0] = left_m / 5.0;
    raw[1] = left_m / 5.0 + left_q / 20.0;
    raw[3] = right_m / 5.0 - right_q / 20.0;
    raw[4] = right_m / 5.0;
    raw[2] = delta - raw[0] - raw[1] - raw[3] - raw[4];
}

inline double least(const double a, const double b) noexcept { return b < a ? b : a; }
inline double greatest(const double a, const double b) noexcept { return b > a ? b : a; }
inline double positive_part(const double x) noexcept { return x > 0.0 ? x : 0.0; }
inline double negative_part(const double x) noexcept { return x < 0.0 ? 0.0 - x : 0.0; }

inline bool one_signed(const double* raw, const double delta) noexcept {
    if (delta > 0.0) return raw[0] >= 0.0 && raw[1] >= 0.0 && raw[2] >= 0.0 && raw[3] >= 0.0 && raw[4] >= 0.0;
    if (delta < 0.0) return raw[0] <= 0.0 && raw[1] <= 0.0 && raw[2] <= 0.0 && raw[3] <= 0.0 && raw[4] <= 0.0;
    return raw[0] == 0.0 && raw[1] == 0.0 && raw[2] == 0.0 && raw[3] == 0.0 && raw[4] == 0.0;
}

// stx_conv's capacity_threshold, same order.
inline double capacity_threshold(double c0, double c1, double c2, double c3, double c4, const double mass) noexcept {
    double smallest = c0;
    smallest = c1 < smallest ? c1 : smallest;
    smallest = c2 < smallest ? c2 : smallest;
    smallest = c3 < smallest ? c3 : smallest;
    smallest = c4 < smallest ? c4 : smallest;
    if (mass < 4.5 * smallest) return mass / 5.0;
    double* const slots[5] = {&c0, &c1, &c2, &c3, &c4};
    const usize network[9][2] = {{0, 1}, {3, 4}, {2, 4}, {2, 3}, {1, 4}, {0, 3}, {0, 2}, {1, 3}, {1, 2}};
    for (usize k = 0; k < 9; ++k) {
        const double low = least(*slots[network[k][0]], *slots[network[k][1]]);
        const double high = greatest(*slots[network[k][0]], *slots[network[k][1]]);
        *slots[network[k][0]] = low;
        *slots[network[k][1]] = high;
    }
    double result = mass / 5.0;
    double sum = c0;
    result = greatest(result, (mass - sum) * 0.25);
    sum = sum + c1;
    result = greatest(result, (mass - sum) / 3.0);
    sum = sum + c2;
    result = greatest(result, (mass - sum) * 0.5);
    sum = sum + c3;
    result = greatest(result, mass - sum);
    return result;
}

// Admission of a cell from its window and shape (stx_conv's compile_cell, same operation
// order, bubble off). With idle_tests (the separated field's interior cells, as K1) it first
// returns true when admission is idle: the raw currents are one-signed, or every one lies in
// its box, and the cell's currents are then the raw ones. Without them (end cells, as
// stx_conv's compile_line_cell) it always projects and writes the admitted currents.
inline bool admit_cell(const double* window, const CellShape& shape, const double* raw, double* admitted, const bool idle_tests) noexcept {
    const double delta = window[4] - window[3];
    if (idle_tests && one_signed(raw, delta)) return true;
    double left_m = 0.0;
    double unused = 0.0;
    double right_m = 0.0;
    node_jet(window, 3, shape.left, left_m, unused);
    node_jet(window, 4, shape.right, right_m, unused);
    const double previous_delta = shape.has_previous ? window[3] - window[2] : 0.0;
    const double next_delta = shape.has_next ? window[5] - window[4] : 0.0;
    const double previous_last = shape.has_previous ? left_m / 5.0 : 0.0;
    const double next_first = shape.has_next ? right_m / 5.0 : 0.0;
    double room_positive[5];
    double room_negative[5];
    double head_positive = least(positive_part(previous_delta), positive_part(positive_part(raw[0]) - negative_part(previous_last)));
    double head_negative = least(negative_part(previous_delta), positive_part(negative_part(raw[0]) - positive_part(previous_last)));
    for (usize j = 0; j < 5; ++j) {
        head_positive = least(head_positive, positive_part(raw[j]));
        head_negative = least(head_negative, negative_part(raw[j]));
        room_positive[j] = head_positive;
        room_negative[j] = head_negative;
    }
    double tail_positive = least(positive_part(next_delta), positive_part(positive_part(raw[4]) - negative_part(next_first)));
    double tail_negative = least(negative_part(next_delta), positive_part(negative_part(raw[4]) - positive_part(next_first)));
    for (usize step = 0; step < 5; ++step) {
        const usize j = 4 - step;
        tail_positive = least(tail_positive, positive_part(raw[j]));
        tail_negative = least(tail_negative, negative_part(raw[j]));
        room_positive[j] = room_positive[j] + tail_positive;
        room_negative[j] = room_negative[j] + tail_negative;
    }
    double total_positive = 0.0;
    double total_negative = 0.0;
    for (usize j = 0; j < 5; ++j) {
        total_positive = total_positive + room_positive[j];
        total_negative = total_negative + room_negative[j];
    }
    const double plus = positive_part(delta);
    const double minus = negative_part(delta);
    const double upper_base = plus + least(total_negative, 4.0 * plus);
    const double lower_base = minus + least(total_positive, 4.0 * minus);
    double high[5];
    double low[5];
    bool inside = true;
    for (usize j = 0; j < 5; ++j) {
        high[j] = room_positive[j] + upper_base;
        low[j] = 0.0 - (room_negative[j] + lower_base);
        inside = inside && raw[j] <= high[j] && raw[j] >= low[j];
    }
    if (idle_tests && inside) return true;
    double clipped[5];
    double down[5];
    double up[5];
    double sum = 0.0;
    for (usize j = 0; j < 5; ++j) {
        clipped[j] = least(high[j], greatest(low[j], raw[j]));
        down[j] = clipped[j] - low[j];
        up[j] = high[j] - clipped[j];
        sum = sum + clipped[j];
    }
    const double residual = sum - delta;
    const bool lower = 0.0 < residual;
    double capacity[5];
    for (usize j = 0; j < 5; ++j) capacity[j] = lower ? down[j] : up[j];
    const double mass = lower ? residual : 0.0 - residual;
    const double threshold = capacity_threshold(capacity[0], capacity[1], capacity[2], capacity[3], capacity[4], mass);
    for (usize j = 0; j < 5; ++j) {
        const double shift = least(threshold, capacity[j]);
        admitted[j] = lower ? clipped[j] - shift : clipped[j] + shift;
    }
    return false;
}

// The linear kernel s_t[r], r = -2..3 (FFT_MATH.md section 10), by Horner.
inline void kernel_weights(const double t, double* s) noexcept {
    s[0] = t * (1.0 / 12.0 + t * (-1.0 / 24.0 + t * (-3.0 / 8.0 + t * (13.0 / 24.0 + t * (-5.0 / 24.0)))));
    s[1] = t * (-2.0 / 3.0 + t * (2.0 / 3.0 + t * (13.0 / 8.0 + t * (-8.0 / 3.0 + t * (25.0 / 24.0)))));
    s[2] = 1.0 + t * t * (-5.0 / 4.0 + t * (-35.0 / 12.0 + t * (21.0 / 4.0 + t * (-25.0 / 12.0))));
    s[3] = t * (2.0 / 3.0 + t * (2.0 / 3.0 + t * (11.0 / 4.0 + t * (-31.0 / 6.0 + t * (25.0 / 12.0)))));
    s[4] = t * (-1.0 / 12.0 + t * (-1.0 / 24.0 + t * (-11.0 / 8.0 + t * (61.0 / 24.0 + t * (-25.0 / 24.0)))));
    s[5] = t * t * t * (7.0 / 24.0 + t * (-1.0 / 2.0 + t * (5.0 / 24.0)));
}

// The synthesis tail weights g_k(t) (stx_conv's make_basis).
inline void tail_weights(const double t, double* tau) noexcept {
    const double s = 1.0 - t;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double t4 = t2 * t2;
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double s4 = s2 * s2;
    tau[4] = t4 * t;
    tau[3] = tau[4] + 5.0 * (t4 * s);
    tau[2] = tau[3] + 10.0 * (t3 * s2);
    tau[1] = tau[2] + 10.0 * (t2 * s3);
    tau[0] = tau[1] + 5.0 * (t * s4);
}

// The cell and phase of a node coordinate, clamped to [0, cells] (stx_conv's locate).
inline void locate(const double x, const usize cells, usize& cell, double& t) noexcept {
    const double extent = static_cast<double>(cells);
    double position = x < 0.0 ? 0.0 : x > extent ? extent : x;
    if (!(position == position)) position = 0.0;
    // position is in [0, extent], so truncation is floor.
    double whole = static_cast<double>(static_cast<u64>(position));
    if (whole >= extent) whole = extent - 1.0;
    cell = static_cast<usize>(whole);
    t = position - whole;
}

inline double evaluate_cell(const double anchor, const double* currents, const double* tau) noexcept {
    double value = anchor + currents[0] * tau[0];
    value = value + currents[1] * tau[1];
    value = value + currents[2] * tau[2];
    value = value + currents[3] * tau[3];
    value = value + currents[4] * tau[4];
    return value;
}

} // namespace stxlib::conv::detail
