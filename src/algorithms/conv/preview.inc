// K2, preview rows (kernels.hpp). Included by variant.inc.


constexpr std::size_t channels = lanes_channels;

// One channel group of a row cell at sample offset s (decoded[4 s + c]).
template<class L> STXLIB_CONV_INLINE void row_cell_group(const double* decoded, std::size_t s, std::size_t c, double* cell) noexcept {
    using V = typename L::Value;
    // Window samples w1..w6 = x-2 .. x+3 relative to the cell's left node x (sample s + 2).
    const V w1 = L::load(decoded + 4U * (s + 0U) + c);
    const V w2 = L::load(decoded + 4U * (s + 1U) + c);
    const V w3 = L::load(decoded + 4U * (s + 2U) + c);
    const V w4 = L::load(decoded + 4U * (s + 3U) + c);
    const V w5 = L::load(decoded + 4U * (s + 4U) + c);
    const V w6 = L::load(decoded + 4U * (s + 5U) + c);
    const V eight = L::set(8.0);
    const V twelve = L::set(12.0);
    const V sixteen = L::set(16.0);
    const V thirty = L::set(30.0);
    const V five = L::set(5.0);
    const V twenty = L::set(20.0);
    V left_m = L::sub(w1, L::mul(eight, w2));
    left_m = L::add(left_m, L::mul(eight, w4));
    left_m = L::sub(left_m, w5);
    left_m = L::div(left_m, twelve);
    V left_q = L::sub(L::mul(sixteen, w4), w5);
    left_q = L::sub(left_q, L::mul(thirty, w3));
    left_q = L::add(left_q, L::mul(sixteen, w2));
    left_q = L::sub(left_q, w1);
    left_q = L::div(left_q, twelve);
    V right_m = L::sub(w2, L::mul(eight, w3));
    right_m = L::add(right_m, L::mul(eight, w5));
    right_m = L::sub(right_m, w6);
    right_m = L::div(right_m, twelve);
    V right_q = L::sub(L::mul(sixteen, w5), w6);
    right_q = L::sub(right_q, L::mul(thirty, w4));
    right_q = L::add(right_q, L::mul(sixteen, w3));
    right_q = L::sub(right_q, w2);
    right_q = L::div(right_q, twelve);
    const V delta = L::sub(w4, w3);
    const V r0 = L::div(left_m, five);
    const V r1 = L::add(L::div(left_m, five), L::div(left_q, twenty));
    const V r3 = L::sub(L::div(right_m, five), L::div(right_q, twenty));
    const V r4 = L::div(right_m, five);
    V r2 = L::sub(delta, r0);
    r2 = L::sub(r2, r1);
    r2 = L::sub(r2, r3);
    r2 = L::sub(r2, r4);
    L::store(cell + 0U * channels + c, w3);
    L::store(cell + 1U * channels + c, r0);
    L::store(cell + 2U * channels + c, r1);
    L::store(cell + 3U * channels + c, r2);
    L::store(cell + 4U * channels + c, r3);
    L::store(cell + 5U * channels + c, r4);
}

void row_cells(const double* decoded, std::size_t count, double* local) {
    for (std::size_t k = 0U; k < count; ++k) {
        std::size_t c = 0U;
        for (; c + Lanes::width <= channels; c += Lanes::width) row_cell_group<Lanes>(decoded, k, c, local + 24U * k);
        for (; c < channels; ++c) row_cell_group<ScalarLanes>(decoded, k, c, local + 24U * k);
    }
}

// value = anchor + c0 tau0 + ... + c4 tau4, left to right (the scalar tiled preview's order).
template<class L> STXLIB_CONV_INLINE void row_value_group(const double* cell, const double* tau, std::size_t c, double* plane) noexcept {
    using V = typename L::Value;
    V value = L::add(L::load(cell + c), L::mul(L::load(cell + 1U * channels + c), L::set(tau[0])));
    value = L::add(value, L::mul(L::load(cell + 2U * channels + c), L::set(tau[1])));
    value = L::add(value, L::mul(L::load(cell + 3U * channels + c), L::set(tau[2])));
    value = L::add(value, L::mul(L::load(cell + 4U * channels + c), L::set(tau[3])));
    value = L::add(value, L::mul(L::load(cell + 5U * channels + c), L::set(tau[4])));
    L::store(plane + c, value);
}

void tile_windows(std::size_t queries, const double* const* rows, const double* tau_u, double* const w[8]) {
    for (std::size_t q = 0U; q < queries; ++q) {
        const double* tau = tau_u + 5U * q;
        for (std::size_t k = 0U; k < 8U; ++k) {
            double* plane = w[k] + 4U * q;
            const double* cell = rows[8U * q + k];   // never null: absent rows point at a zero cell
            std::size_t c = 0U;
            for (; c + Lanes::width <= channels; c += Lanes::width) row_value_group<Lanes>(cell, tau, c, plane);
            for (; c < channels; ++c) row_value_group<ScalarLanes>(cell, tau, c, plane);
        }
    }
}

// The weights are one query's, broadcast across its channel lanes.
template<class L> STXLIB_CONV_INLINE void linear_group(const double* const w[8], const double* sv, double* value, std::size_t lane) noexcept {
    using V = typename L::Value;
    V sum = L::set(0.0);
    for (std::size_t r = 0U; r < 6U; ++r) sum = L::add(sum, L::mul(L::set(sv[r]), L::load(w[r + 1U] + lane)));
    L::store(value + lane, sum);
}

void vertical_linear(std::size_t queries, const double* const w[8], const double* sv, double* value) {
    for (std::size_t q = 0U; q < queries; ++q) {
        std::size_t c = 0U;
        for (; c + Lanes::width <= channels; c += Lanes::width) linear_group<Lanes>(w, sv + 6U * q, value, 4U * q + c);
        for (; c < channels; ++c) linear_group<ScalarLanes>(w, sv + 6U * q, value, 4U * q + c);
    }
}

// Compact windows carry their own tail weights (tau[5 i + k]); scalar lanes, since the weights
// differ per window and the list is short.
void vertical_admitted(const double* w3, const double* tau, const double* const admitted[5], double* value, std::size_t m) {
    for (std::size_t i = 0U; i < m; ++i) {
        double sum = w3[i];
        for (std::size_t k = 0U; k < 5U; ++k) sum = sum + admitted[k][i] * tau[5U * i + k];
        value[i] = sum;
    }
}
