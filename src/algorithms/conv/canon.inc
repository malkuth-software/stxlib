// K3, canon cells (kernels.hpp). Included by variant.inc.


constexpr std::size_t ch = lanes_channels;

// beta[m][a]: power coefficients of B_m^5; tauc[k][b] = sum_{l > k} beta[l][b]. Integers, exact;
// compile-time, so the kernel's zero tests fold away (the big-dumb pass found 26 run-time
// branches from passing them by pointer). Equal to the caller's tables by construction.
struct PowerTables final {
    double beta[6][6]{};
    double tauc[5][6]{};
};
constexpr PowerTables make_power_tables() {
    PowerTables p{};
    double binom[6][6]{};
    for (std::size_t n = 0U; n < 6U; ++n) {
        binom[n][0] = 1.0;
        for (std::size_t k = 1U; k <= n; ++k) binom[n][k] = binom[n - 1U][k - 1U] + (k <= n - 1U ? binom[n - 1U][k] : 0.0);
    }
    for (std::size_t m = 0U; m < 6U; ++m) {
        for (std::size_t i = 0U; m + i <= 5U; ++i) p.beta[m][m + i] = binom[5][m] * binom[5 - m][i] * ((i % 2U == 0U) ? 1.0 : -1.0);
    }
    for (std::size_t k = 0U; k < 5U; ++k) {
        for (std::size_t l = k + 1U; l <= 5U; ++l) {
            for (std::size_t b = 0U; b < 6U; ++b) p.tauc[k][b] = p.tauc[k][b] + p.beta[l][b];
        }
    }
    return p;
}
constexpr PowerTables power = make_power_tables();

template<class L> STXLIB_CONV_INLINE void g_group(const double* __restrict R, const double* __restrict P3, double* __restrict g,
                                           std::size_t c) noexcept {
    using V = typename L::Value;
    V acc[36];
    for (std::size_t k = 0U; k < 36U; ++k) acc[k] = L::set(0.0);
    for (std::size_t m = 0U; m < 6U; ++m) {
        V inner[6];
        for (std::size_t b = 0U; b < 6U; ++b) {
            V v = b == 0U ? L::load(P3 + m * ch + c) : L::set(0.0);
            for (std::size_t k = 0U; k < 5U; ++k) v = L::add(v, L::mul(L::load(R + (k * 6U + m) * ch + c), L::set(power.tauc[k][b])));
            inner[b] = v;
        }
        for (std::size_t a = 0U; a < 6U; ++a) {
            const double coefficient = power.beta[m][a];
            if (coefficient == 0.0) continue;
            for (std::size_t b = 0U; b < 6U; ++b) acc[a * 6U + b] = L::add(acc[a * 6U + b], L::mul(L::set(coefficient), inner[b]));
        }
    }
    for (std::size_t k = 0U; k < 36U; ++k) L::store(g + k * ch + c, acc[k]);
}

void g_coefficients(const double* R, const double* P3, double* g) {
    std::size_t c = 0U;
    for (; c + Lanes::width <= ch; c += Lanes::width) g_group<Lanes>(R, P3, g, c);
    for (; c < ch; ++c) g_group<ScalarLanes>(R, P3, g, c);
}

template<class L> STXLIB_CONV_INLINE void window_group(const double* P, const double* b5, double* const w[8], std::size_t n, std::size_t c) noexcept {
    using V = typename L::Value;
    for (std::size_t k = 0U; k < 8U; ++k) {
        V v = L::set(0.0);
        for (std::size_t m = 0U; m < 6U; ++m) v = L::add(v, L::mul(L::set(b5[6U * n + m]), L::load(P + (k * 6U + m) * ch + c)));
        L::store(w[k] + 4U * n + c, v);
    }
}

void node_windows(const double* P, std::size_t nodes, const double* b5, double* const w[8]) {
    for (std::size_t n = 0U; n < nodes; ++n) {
        std::size_t c = 0U;
        for (; c + Lanes::width <= ch; c += Lanes::width) window_group<Lanes>(P, b5, w, n, c);
        for (; c < ch; ++c) window_group<ScalarLanes>(P, b5, w, n, c);
    }
}

template<class L> STXLIB_CONV_INLINE void strip_group(const double* __restrict g, const double* __restrict e, const double* __restrict basis,
                                               const double* __restrict anchor, double t0, double* __restrict q, std::size_t c) noexcept {
    using V = typename L::Value;
    V f[36];
    for (std::size_t k = 0U; k < 36U; ++k) f[k] = L::load(g + k * ch + c);
    if (e != nullptr) {
        for (std::size_t k = 0U; k < 5U; ++k) {
            V ec[6];
            for (std::size_t a = 0U; a < 6U; ++a) ec[a] = L::set(0.0);
            for (std::size_t n = 0U; n < 6U; ++n) {
                const V en = L::load(e + (n * 5U + k) * ch + c);
                for (std::size_t a = 0U; a < 6U; ++a) ec[a] = L::add(ec[a], L::mul(en, L::set(basis[n * 6U + a])));
            }
            for (std::size_t a = 0U; a < 6U; ++a) {
                for (std::size_t b = 0U; b < 6U; ++b) f[a * 6U + b] = L::add(f[a * 6U + b], L::mul(ec[a], L::set(power.tauc[k][b])));
            }
        }
    }
    V qv[42];
    for (std::size_t b = 0U; b < 6U; ++b) qv[b] = L::set(0.0);
    for (std::size_t a = 0U; a < 6U; ++a) {
        for (std::size_t b = 0U; b < 6U; ++b) qv[(a + 1U) * 6U + b] = L::div(f[a * 6U + b], L::set(static_cast<double>(a + 1U)));
    }
    const V t = L::set(t0);
    for (std::size_t b = 0U; b < 6U; ++b) {
        const V a_s = anchor != nullptr ? L::load(anchor + b * ch + c) : L::set(0.0);
        V at = L::set(0.0);
        for (std::size_t a = 7U; a > 1U; --a) at = L::add(L::mul(at, t), qv[(a - 1U) * 6U + b]);
        at = L::mul(at, t);
        qv[b] = L::sub(a_s, at);
    }
    for (std::size_t k = 0U; k < 42U; ++k) L::store(q + k * ch + c, qv[k]);
}

void strip_q(const double* g, const double* e, const double* basis, const double* anchor, double t0, double* q) {
    std::size_t c = 0U;
    for (; c + Lanes::width <= ch; c += Lanes::width) strip_group<Lanes>(g, e, basis, anchor, t0, q, c);
    for (; c < ch; ++c) strip_group<ScalarLanes>(g, e, basis, anchor, t0, q, c);
}

template<class L> STXLIB_CONV_INLINE void q_at_group(const double* q, double t, double* out, std::size_t c) noexcept {
    using V = typename L::Value;
    const V tt = L::set(t);
    for (std::size_t b = 0U; b < 6U; ++b) {
        V v = L::set(0.0);
        for (std::size_t a = 7U; a > 0U; --a) v = L::add(L::mul(v, tt), L::load(q + ((a - 1U) * 6U + b) * ch + c));
        L::store(out + b * ch + c, v);
    }
}

void q_at(const double* q, double t, double* out) {
    std::size_t c = 0U;
    for (; c + Lanes::width <= ch; c += Lanes::width) q_at_group<Lanes>(q, t, out, c);
    for (; c < ch; ++c) q_at_group<ScalarLanes>(q, t, out, c);
}

template<class L> STXLIB_CONV_INLINE void flux_group(const double* q, std::size_t points, const double* tu, const double* tv,
                                              const double* weight, double* sum4, std::size_t c) noexcept {
    using V = typename L::Value;
    V sum = L::load(sum4 + c);
    for (std::size_t p = 0U; p < points; ++p) {
        double tvp[6];
        tvp[0] = 1.0;
        for (std::size_t b = 1U; b < 6U; ++b) tvp[b] = tvp[b - 1U] * tv[p];
        const V u = L::set(tu[p]);
        V value = L::set(0.0);
        for (std::size_t a = 7U; a > 0U; --a) {
            V row = L::set(0.0);
            for (std::size_t b = 0U; b < 6U; ++b) row = L::add(row, L::mul(L::load(q + ((a - 1U) * 6U + b) * ch + c), L::set(tvp[b])));
            value = L::add(L::mul(value, u), row);
        }
        sum = L::add(sum, L::mul(L::set(weight[p]), value));
    }
    L::store(sum4 + c, sum);
}

void flux_points(const double* q, std::size_t points, const double* tu, const double* tv, const double* weight, double* sum4) {
    std::size_t c = 0U;
    for (; c + Lanes::width <= ch; c += Lanes::width) flux_group<Lanes>(q, points, tu, tv, weight, sum4, c);
    for (; c < ch; ++c) flux_group<ScalarLanes>(q, points, tu, tv, weight, sum4, c);
}

template<class L> STXLIB_CONV_INLINE void moments_group(const double* q, const double* m, double weight, double* sum4, std::size_t c) noexcept {
    using V = typename L::Value;
    V v = L::set(0.0);
    for (std::size_t a = 0U; a < 6U; ++a) {
        for (std::size_t b = 0U; b < 6U; ++b) {
            const V term = L::mul(L::mul(L::load(q + ((a + 1U) * 6U + b) * ch + c), L::set(static_cast<double>(a + 1U))), L::set(m[a * 6U + b]));
            v = L::add(v, term);
        }
    }
    L::store(sum4 + c, L::add(L::load(sum4 + c), L::mul(L::set(weight), v)));
}

void moments_contract(const double* q, const double* m, double weight, double* sum4) {
    std::size_t c = 0U;
    for (; c + Lanes::width <= ch; c += Lanes::width) moments_group<Lanes>(q, m, weight, sum4, c);
    for (; c < ch; ++c) moments_group<ScalarLanes>(q, m, weight, sum4, c);
}
