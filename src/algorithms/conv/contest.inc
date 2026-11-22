// K1, the contest (kernels.hpp). Included by variant.inc.


// Inputs, outputs and the idle bytes never overlap (the callers' contract): stated with restrict
// so the compiler drops its run-time overlap checks and loop versioning.
using In = const double* __restrict;
using Out = double* __restrict;

template<class L> struct Raw final {
    typename L::Value r[5];
};

// Interior raw currents from w[1..6] at lane offset i (surface_kernel.hpp compile_cell).
template<class L> STXLIB_CONV_INLINE void raw_group(const double* const window[8], std::size_t i, typename L::Value* r) noexcept {
    using V = typename L::Value;
    const V w1 = L::load(window[1] + i);
    const V w2 = L::load(window[2] + i);
    const V w3 = L::load(window[3] + i);
    const V w4 = L::load(window[4] + i);
    const V w5 = L::load(window[5] + i);
    const V w6 = L::load(window[6] + i);
    const V eight = L::set(8.0);
    const V twelve = L::set(12.0);
    const V sixteen = L::set(16.0);
    const V thirty = L::set(30.0);
    const V five = L::set(5.0);
    const V twenty = L::set(20.0);
    // left_m = (w1 - 8 w2 + 8 w4 - w5) / 12
    V left_m = L::sub(w1, L::mul(eight, w2));
    left_m = L::add(left_m, L::mul(eight, w4));
    left_m = L::sub(left_m, w5);
    left_m = L::div(left_m, twelve);
    // left_q = (16 w4 - w5 - 30 w3 + 16 w2 - w1) / 12
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
    r[0] = L::div(left_m, five);
    r[1] = L::add(L::div(left_m, five), L::div(left_q, twenty));
    r[3] = L::sub(L::div(right_m, five), L::div(right_q, twenty));
    r[4] = L::div(right_m, five);
    V middle = L::sub(delta, r[0]);
    middle = L::sub(middle, r[1]);
    middle = L::sub(middle, r[3]);
    middle = L::sub(middle, r[4]);
    r[2] = middle;
}

template<class L> STXLIB_CONV_INLINE typename L::Mask one_signed_group(const typename L::Value* r, typename L::Value delta) noexcept {
    using V = typename L::Value;
    using M = typename L::Mask;
    const V zero = L::set(0.0);
    M none_negative = L::invert(L::less(r[0], zero));
    M none_positive = L::invert(L::less(zero, r[0]));
    M all_zero = L::equal(r[0], zero);
    for (std::size_t k = 1U; k < 5U; ++k) {
        none_negative = L::both(none_negative, L::invert(L::less(r[k], zero)));
        none_positive = L::both(none_positive, L::invert(L::less(zero, r[k])));
        all_zero = L::both(all_zero, L::equal(r[k], zero));
    }
    const M rising = L::both(L::less(zero, delta), none_negative);
    const M falling = L::both(L::less(delta, zero), none_positive);
    const M flat = L::both(L::equal(delta, zero), all_zero);
    return L::either(L::either(rising, falling), flat);
}

template<class L> STXLIB_CONV_INLINE std::size_t pass_a_group(const double* const window[8], const Out* raw, u32* __restrict list,
                                                       std::size_t count, std::size_t i) noexcept {
    using V = typename L::Value;
    V r[5];
    raw_group<L>(window, i, r);
    for (std::size_t k = 0U; k < 5U; ++k) L::store(raw[k] + i, r[k]);
    const V delta = L::sub(L::load(window[4] + i), L::load(window[3] + i));
    const typename L::Mask ok = one_signed_group<L>(r, delta);
    // Every item is written to the list; the count advances only for the contested.
    for (std::size_t k = 0U; k < L::width; ++k) {
        list[count] = static_cast<u32>(i + k);
        count = count + (1U - L::flag(ok, k));
    }
    return count;
}

std::size_t pass_a(const double* const window[8], double* const raw[5], u32* __restrict list, std::size_t n) {
    // Window planes may overlap each other (shifted views of one row), never the outputs.
    const std::array<const double*, 8> w{window[0], window[1], window[2], window[3], window[4], window[5], window[6], window[7]};
    const std::array<Out, 5> r{raw[0], raw[1], raw[2], raw[3], raw[4]};
    std::size_t count = 0U;
    std::size_t i = 0U;
    for (; i + Lanes::width <= n; i += Lanes::width) count = pass_a_group<Lanes>(w.data(), r.data(), list, count, i);
    for (; i < n; ++i) count = pass_a_group<ScalarLanes>(w.data(), r.data(), list, count, i);
    return count;
}

template<class L> STXLIB_CONV_INLINE void order_pair(typename L::Value& a, typename L::Value& b) noexcept {
    const typename L::Value low = least<L>(a, b);
    const typename L::Value high = greatest<L>(a, b);
    a = low;
    b = high;
}

// surface_kernel.hpp capacity_threshold: the shortcut answer mass / 5 when mass < 4.5 x the
// smallest capacity, else the five prefix forms after the nine-comparator network. Both are
// computed; the shortcut is the first prefix form, so selecting it is exact.
template<class L> STXLIB_CONV_INLINE typename L::Value capacity_threshold(typename L::Value c0, typename L::Value c1, typename L::Value c2,
                                                                  typename L::Value c3, typename L::Value c4, typename L::Value mass) noexcept {
    using V = typename L::Value;
    V smallest = c0;
    smallest = L::select(L::less(c1, smallest), c1, smallest);
    smallest = L::select(L::less(c2, smallest), c2, smallest);
    smallest = L::select(L::less(c3, smallest), c3, smallest);
    smallest = L::select(L::less(c4, smallest), c4, smallest);
    // x / 4 and x / 2 are exactly x * 0.25 and x * 0.5 in IEEE arithmetic (a power-of-two
    // reciprocal is exact), so the multiplications keep stx_conv's bits; mass / 5 is computed once.
    const V even = L::div(mass, L::set(5.0));
    order_pair<L>(c0, c1);
    order_pair<L>(c3, c4);
    order_pair<L>(c2, c4);
    order_pair<L>(c2, c3);
    order_pair<L>(c1, c4);
    order_pair<L>(c0, c3);
    order_pair<L>(c0, c2);
    order_pair<L>(c1, c3);
    order_pair<L>(c1, c2);
    V result = even;
    V sum = c0;
    result = greatest<L>(result, L::mul(L::sub(mass, sum), L::set(0.25)));
    sum = L::add(sum, c1);
    result = greatest<L>(result, L::div(L::sub(mass, sum), L::set(3.0)));
    sum = L::add(sum, c2);
    result = greatest<L>(result, L::mul(L::sub(mass, sum), L::set(0.5)));
    sum = L::add(sum, c3);
    result = greatest<L>(result, L::sub(mass, sum));
    return L::select(L::less(mass, L::mul(L::set(4.5), smallest)), even, result);
}

// Pass B on one lane group, split into load, compute (pure, on registers) and store, so that
// several groups can be computed in one iteration and their dependent chains overlap
// (PERFORMANCE_REVIEW.md R1: independent accumulators in flight).
template<class L> struct PassBIn final {
    typename L::Value r[5];
    typename L::Value w2, w3, w4, w5;
};
template<class L> struct PassBOut final {
    typename L::Value admitted[5];
    typename L::Mask inside;
};

template<class L> STXLIB_CONV_INLINE void pass_b_load(const In* window, const In* raw, std::size_t i, PassBIn<L>& in) noexcept {
    for (std::size_t k = 0U; k < 5U; ++k) in.r[k] = L::load(raw[k] + i);
    in.w2 = L::load(window[2] + i);
    in.w3 = L::load(window[3] + i);
    in.w4 = L::load(window[4] + i);
    in.w5 = L::load(window[5] + i);
}

template<class L> STXLIB_CONV_INLINE void pass_b_compute(const PassBIn<L>& in, PassBOut<L>& out) noexcept {
    using V = typename L::Value;
    using M = typename L::Mask;
    const V zero = L::set(0.0);
    const V* r = in.r;
    const V delta = L::sub(in.w4, in.w3);
    const V previous_delta = L::sub(in.w3, in.w2);
    const V next_delta = L::sub(in.w5, in.w4);
    const V previous_last = r[0];
    const V next_first = r[4];
    V room_positive[5];
    V room_negative[5];
    V head_positive = least<L>(positive_part<L>(previous_delta), positive_part<L>(L::sub(positive_part<L>(r[0]), negative_part<L>(previous_last))));
    V head_negative = least<L>(negative_part<L>(previous_delta), positive_part<L>(L::sub(negative_part<L>(r[0]), positive_part<L>(previous_last))));
    for (std::size_t j = 0U; j < 5U; ++j) {
        head_positive = least<L>(head_positive, positive_part<L>(r[j]));
        head_negative = least<L>(head_negative, negative_part<L>(r[j]));
        room_positive[j] = head_positive;
        room_negative[j] = head_negative;
    }
    V tail_positive = least<L>(positive_part<L>(next_delta), positive_part<L>(L::sub(positive_part<L>(r[4]), negative_part<L>(next_first))));
    V tail_negative = least<L>(negative_part<L>(next_delta), positive_part<L>(L::sub(negative_part<L>(r[4]), positive_part<L>(next_first))));
    for (std::size_t step = 0U; step < 5U; ++step) {
        const std::size_t j = 4U - step;
        tail_positive = least<L>(tail_positive, positive_part<L>(r[j]));
        tail_negative = least<L>(tail_negative, negative_part<L>(r[j]));
        room_positive[j] = L::add(room_positive[j], tail_positive);
        room_negative[j] = L::add(room_negative[j], tail_negative);
    }
    V total_positive = zero;
    V total_negative = zero;
    for (std::size_t j = 0U; j < 5U; ++j) {
        total_positive = L::add(total_positive, room_positive[j]);
        total_negative = L::add(total_negative, room_negative[j]);
    }
    const V plus = positive_part<L>(delta);
    const V minus = negative_part<L>(delta);
    const V upper_base = L::add(plus, least<L>(total_negative, L::mul(L::set(4.0), plus)));
    const V lower_base = L::add(minus, least<L>(total_positive, L::mul(L::set(4.0), minus)));
    V clipped[5];
    V down[5];
    V up[5];
    V sum = zero;
    M inside = L::equal(zero, zero);
    for (std::size_t j = 0U; j < 5U; ++j) {
        const V high = L::add(room_positive[j], upper_base);
        const V low = L::sub(zero, L::add(room_negative[j], lower_base));
        inside = L::both(inside, L::both(L::invert(L::less(high, r[j])), L::invert(L::less(r[j], low))));
        clipped[j] = least<L>(high, greatest<L>(low, r[j]));
        down[j] = L::sub(clipped[j], low);
        up[j] = L::sub(high, clipped[j]);
        sum = L::add(sum, clipped[j]);
    }
    const V residual = L::sub(sum, delta);
    const M lower = L::less(zero, residual);
    V capacity[5];
    for (std::size_t j = 0U; j < 5U; ++j) capacity[j] = L::select(lower, down[j], up[j]);
    const V mass = L::select(lower, residual, L::sub(zero, residual));
    const V threshold = capacity_threshold<L>(capacity[0], capacity[1], capacity[2], capacity[3], capacity[4], mass);
    for (std::size_t j = 0U; j < 5U; ++j) {
        const V shift = least<L>(threshold, capacity[j]);
        const V projected = L::select(lower, L::sub(clipped[j], shift), L::add(clipped[j], shift));
        out.admitted[j] = L::select(inside, r[j], projected);
    }
    out.inside = inside;
}

template<class L> STXLIB_CONV_INLINE void pass_b_store(const PassBOut<L>& out, const Out* admitted, u8* __restrict idle, std::size_t i) noexcept {
    for (std::size_t j = 0U; j < 5U; ++j) L::store(admitted[j] + i, out.admitted[j]);
    for (std::size_t k = 0U; k < L::width; ++k) idle[i + k] = static_cast<u8>(L::flag(out.inside, k));
}

template<class L> STXLIB_CONV_INLINE void pass_b_group(const In* window, const In* raw, const Out* admitted, u8* __restrict idle,
                                                std::size_t i) noexcept {
    PassBIn<L> in;
    PassBOut<L> out;
    pass_b_load<L>(window, raw, i, in);
    pass_b_compute<L>(in, out);
    pass_b_store<L>(out, admitted, idle, i);
}

// Two groups in flight: loads, two independent computations, stores. (Three groups spilled
// about a thousand stack slots on avx2: the big-dumb pass, 2026-11-23.)
template<class L> STXLIB_CONV_INLINE void pass_b_pair(const In* window, const In* raw, const Out* admitted, u8* __restrict idle,
                                               std::size_t i) noexcept {
    PassBIn<L> a;
    PassBIn<L> b;
    pass_b_load<L>(window, raw, i, a);
    pass_b_load<L>(window, raw, i + L::width, b);
    PassBOut<L> oa;
    PassBOut<L> ob;
    pass_b_compute<L>(a, oa);
    pass_b_compute<L>(b, ob);
    pass_b_store<L>(oa, admitted, idle, i);
    pass_b_store<L>(ob, admitted, idle, i + L::width);
}

void pass_b(const double* const window[8], const double* const raw[5], double* const admitted[5], u8* __restrict idle, std::size_t m) {
    const std::array<In, 8> w{window[0], window[1], window[2], window[3], window[4], window[5], window[6], window[7]};
    const std::array<In, 5> r{raw[0], raw[1], raw[2], raw[3], raw[4]};
    const std::array<Out, 5> a{admitted[0], admitted[1], admitted[2], admitted[3], admitted[4]};
    std::size_t i = 0U;
    for (; i + 2U * Lanes::width <= m; i += 2U * Lanes::width) pass_b_pair<Lanes>(w.data(), r.data(), a.data(), idle, i);
    for (; i + Lanes::width <= m; i += Lanes::width) pass_b_group<Lanes>(w.data(), r.data(), a.data(), idle, i);
    for (; i < m; ++i) pass_b_group<ScalarLanes>(w.data(), r.data(), a.data(), idle, i);
}
