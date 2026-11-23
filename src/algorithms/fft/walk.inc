// The PSFT's kernels, generic over the precision T and the tier's vector
// policies (walk_lanes.inc: WalkPolicies<T>::W, Nr, H). Included only by
// walk_variant.inc, inside the tier's namespace, its anonymous namespace and
// its target attribute.
//
// The PSFT, the progressively self-sorting Fourier transform (in-house,
// 2026-11-23; the prototype is ~/Developer/work/fft/ideal walk.hpp, measured
// and checked there, and the reference this port is held to by its frozen
// hashes, tests/fft_walk_tests.cpp; the source files keep the working name
// walk). For N = 2^m real
// samples it runs the m stages of a radix-2 real FFT as radix-4 and radix-2
// passes, then a terminal pass for the last two stages:
// - Each stage turns one time bit of the address into one frequency bit. For
//   natural order at both ends, address bit s must be exchanged with bit
//   m - 1 - s for each s below m/2; that is the whole sort. The PSFT does each
//   exchange inside the later stage of its pair, as the addresses that
//   stage's cells store to. No pass moves data without computing.
// - The first ceil(m/2) stages are plain (a Bruun DIF's first half: each run
//   [P | Q] becomes [P + wQ | P - wQ] in place); the rest are crossed (two
//   runs that differ in one frequency bit write their sums to one far run
//   and their differences to another), so the bins come out in order.
// - Real-only: row kappa's real part sits at the address the complex walk
//   gives (kappa, j) and minus its imaginary part at (2^t - kappa, j); no
//   complex value is formed. The cell is BFFT's: with r = c oa - s ob and
//   i = s oa + c ob, (ea, eb, oa, ob) becomes (ea + r, eb + i, ea - r, i - eb).
// - Self-sorting in-place FFTs are published (Temperton 1991, Hegland 1994);
//   this is a real-only placement of their exchanges, not a new idea.
//
// Work is ordered by the plan's schedule (walk_plan.cpp): cache-sized blocks
// pass by pass, larger blocks depth-first. The order of independent cells
// never changes a value.
//
// Subnormals (A1 rule 4). The caller's input is read through walk_load,
// walk_get or walk_in, which flush (a subnormal reads as zero of its sign),
// and the output is written through walk_put, walk_store or walk_out, which
// flush the same way: the first pass that reads the input (In) and the last
// that writes the output (Out) carry the rule, so no transform scans, copies
// or rewrites its arrays for it. Work buffers are read and written as they
// are.
//
// Floating point. W::fma(a, b, c) is a + b c and W::fms(a, b, c) is a - b c:
// one fused operation in the fused build, a multiply then an add or subtract
// in the exact build (walk_lanes.inc). No other form is fused, and nothing
// else differs between the builds. Every operation is in T: no wider
// intermediate (A1 rule 5).

// The tier's strip and table for T.
template <typename T>
[[nodiscard]] const T* strip_of(const WalkPass& pass) noexcept;
template <>
[[nodiscard]] inline const double* strip_of<double>(const WalkPass& pass) noexcept {
    return pass.strip64;
}
template <>
[[nodiscard]] inline const float* strip_of<float>(const WalkPass& pass) noexcept {
    return pass.strip32;
}

template <typename T>
[[nodiscard]] const T* twiddles_of(const WalkTables& tables) noexcept;
template <>
[[nodiscard]] inline const double* twiddles_of<double>(const WalkTables& tables) noexcept {
    return tables.twiddles64;
}
template <>
[[nodiscard]] inline const float* twiddles_of<float>(const WalkTables& tables) noexcept {
    return tables.twiddles32;
}

// cos(pi/4), and half of it, in T.
template <typename T>
inline constexpr T walk_root_half = static_cast<T>(0.70710678118654752440);
template <typename T>
inline constexpr T walk_root_quarter = static_cast<T>(0.5 * 0.70710678118654752440);

// The three buffers of one transform: the caller's array and the two work
// buffers, the second `elements + stagger` reals after the first.
template <typename T>
struct WalkBuffers final {
    T* buffer[3];
};

// ---------------------------------------------------------------------------
// Forward passes. s holds the state before the pass, d receives it after.
// ---------------------------------------------------------------------------

// Row 0 of a radix-4 pass: two plain stages, no twiddle.
template <typename V, bool Gapped, bool In, typename T>
void forward4_dc(const WalkPass& p, const T* const s, T* const d) noexcept {
    const T* const a = s + p.dc_in;
    const T* const b = a + p.quarters[1];
    const T* const c = a + p.quarters[2];
    const T* const e = a + p.quarters[3];
    T* const o0 = d + p.dc_out[0];
    T* const o1 = d + p.dc_out[1];
    T* const o2 = d + p.dc_out[2];
    T* const o3 = d + p.dc_out[3];
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v va = walk_load<V, In>(a + i);
            const typename V::v vb = walk_load<V, In>(b + i);
            const typename V::v vc = walk_load<V, In>(c + i);
            const typename V::v ve = walk_load<V, In>(e + i);
            const typename V::v p0 = V::add(va, vc);
            const typename V::v p1 = V::add(vb, ve);
            V::st(o0 + j, V::add(p0, p1));
            V::st(o1 + j, V::sub(p0, p1));
            V::st(o2 + j, V::sub(va, vc));
            V::st(o3 + j, V::sub(vb, ve));
        }
    }
}

// The ridge of a radix-4 pass: the real row 2^(t-1), rotated by an eighth turn.
template <typename V, bool Gapped, bool In, typename T>
void forward4_ridge(const WalkPass& p, const T* const s, T* const d) noexcept {
    const T* const a = s + p.ridge_in;
    const T* const b = a + p.quarters[1];
    const T* const c = a + p.quarters[2];
    const T* const e = a + p.quarters[3];
    T* const o0 = d + p.ridge_out[0];
    T* const o1 = d + p.ridge_out[1];
    T* const o2 = d + p.ridge_out[2];
    T* const o3 = d + p.ridge_out[3];
    const typename V::v k = V::set1(walk_root_half<T>);
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v va = walk_load<V, In>(a + i);
            const typename V::v vb = walk_load<V, In>(b + i);
            const typename V::v vc = walk_load<V, In>(c + i);
            const typename V::v ve = walk_load<V, In>(e + i);
            const typename V::v r = V::mul(k, V::sub(vb, ve));
            const typename V::v im = V::mul(k, V::add(vb, ve));
            V::st(o0 + j, V::add(va, r));
            V::st(o1 + j, V::add(vc, im));
            V::st(o2 + j, V::sub(va, r));
            V::st(o3 + j, V::sub(im, vc));
        }
    }
}

// Radix-4 cells first .. first + count - 1.
template <typename V, bool Gapped, bool In, typename T>
void forward4_cells(const WalkPass& p, const T* const s, T* const d, const u32 first, const u32 count) noexcept {
    const T* const strip = strip_of<T>(p);
    const u32 f0 = p.offsets[0];
    const u32 f1 = p.offsets[1];
    const u32 f2 = p.offsets[2];
    const u32 f3 = p.offsets[3];
    for (u32 cx = first; cx < first + count; ++cx) {
        const WalkCell4& cell = p.cells4[cx];
        const T* const ar = s + cell.in_re;
        const T* const br = ar + p.quarters[1];
        const T* const cr = ar + p.quarters[2];
        const T* const dr = ar + p.quarters[3];
        const T* const ai = s + cell.in_im;
        const T* const bi = ai + p.quarters[1];
        const T* const ci = ai + p.quarters[2];
        const T* const di = ai + p.quarters[3];
        T* const o0 = d + cell.out_o + f0;
        T* const o1 = d + cell.out_o + f1;
        T* const o2 = d + cell.out_o + f2;
        T* const o3 = d + cell.out_o + f3;
        T* const b0 = d + cell.out_b + f3;
        T* const b1 = d + cell.out_b + f0;
        T* const b2 = d + cell.out_b + f1;
        T* const b3 = d + cell.out_b + f2;
        const T* const tq = strip + 4 * static_cast<usize>(cx);
        const typename V::v c1 = V::set1(tq[0]);
        const typename V::v s1 = V::set1(tq[1]);
        const typename V::v c2 = V::set1(tq[2]);
        const typename V::v s2 = V::set1(tq[3]);
        const u32 stretch = Gapped ? p.stretch : p.chunk;
        const u32 end = Gapped ? p.chunk : 1;
        const u32 step = Gapped ? stretch : 1;
        for (u32 z0 = 0; z0 < end; z0 += step) {
            const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
            const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
            for (u32 z = 0; z < stretch; z += V::L) {
                const u32 i = i0 + z;
                const u32 j = j0 + z;
                const typename V::v xar = walk_load<V, In>(ar + i);
                const typename V::v xbr = walk_load<V, In>(br + i);
                const typename V::v xcr = walk_load<V, In>(cr + i);
                const typename V::v xdr = walk_load<V, In>(dr + i);
                const typename V::v xai = walk_load<V, In>(ai + i);
                const typename V::v xbi = walk_load<V, In>(bi + i);
                const typename V::v xci = walk_load<V, In>(ci + i);
                const typename V::v xdi = walk_load<V, In>(di + i);
                const typename V::v r1 = V::fms(V::mul(c1, xcr), s1, xci);
                const typename V::v i1 = V::fma(V::mul(s1, xcr), c1, xci);
                const typename V::v r2 = V::fms(V::mul(c1, xdr), s1, xdi);
                const typename V::v i2 = V::fma(V::mul(s1, xdr), c1, xdi);
                const typename V::v laA = V::add(xar, r1);
                const typename V::v laB = V::add(xai, i1);
                const typename V::v haA = V::sub(xar, r1);
                const typename V::v haB = V::sub(i1, xai);
                const typename V::v lbA = V::add(xbr, r2);
                const typename V::v lbB = V::add(xbi, i2);
                const typename V::v hbA = V::sub(xbr, r2);
                const typename V::v hbB = V::sub(i2, xbi);
                const typename V::v r3 = V::fms(V::mul(c2, lbA), s2, lbB);
                const typename V::v i3 = V::fma(V::mul(s2, lbA), c2, lbB);
                const typename V::v r4 = V::fms(V::mul(s2, hbA), c2, hbB);
                const typename V::v i4 = V::fma(V::mul(c2, hbA), s2, hbB);
                V::st(o0 + j, V::add(laA, r3));
                V::st(b0 + j, V::add(laB, i3));
                V::st(b2 + j, V::sub(laA, r3));
                V::st(o2 + j, V::sub(i3, laB));
                V::st(b1 + j, V::add(haA, r4));
                V::st(o3 + j, V::add(haB, i4));
                V::st(o1 + j, V::sub(haA, r4));
                V::st(b3 + j, V::sub(i4, haB));
            }
        }
    }
}

// Row 0 of a radix-2 pass.
template <typename V, bool Gapped, bool In, typename T>
void forward2_dc(const WalkPass& p, const T* const s, T* const d) noexcept {
    const T* const a = s + p.dc_in;
    const T* const b = a + p.quarters[1];
    T* const o0 = d + p.dc_out[0];
    T* const o1 = d + p.dc_out[1];
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v va = walk_load<V, In>(a + i);
            const typename V::v vb = walk_load<V, In>(b + i);
            V::st(o0 + j, V::add(va, vb));
            V::st(o1 + j, V::sub(va, vb));
        }
    }
}

// The ridge of a radix-2 pass only moves (its stage's twiddle is trivial);
// nothing is done when it stays where it is.
template <typename V, bool Gapped, bool In, typename T>
void forward2_ridge(const WalkPass& p, const T* const s, T* const d) noexcept {
    const T* const a = s + p.ridge_in;
    const T* const b = a + p.quarters[1];
    T* const o0 = d + p.ridge_out[0];
    T* const o1 = d + p.ridge_out[1];
    if (o0 == a && s == d) return;
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            V::st(o0 + j, walk_load<V, In>(a + i));
            V::st(o1 + j, walk_load<V, In>(b + i));
        }
    }
}

// Radix-2 cells first .. first + count - 1.
template <typename V, bool Gapped, bool In, typename T>
void forward2_cells(const WalkPass& p, const T* const s, T* const d, const u32 first, const u32 count) noexcept {
    const T* const strip = strip_of<T>(p);
    for (u32 cx = first; cx < first + count; ++cx) {
        const WalkCell2& cell = p.cells2[cx];
        const T* const ea = s + cell.in_re;
        const T* const oa = ea + p.quarters[1];
        const T* const eb = s + cell.in_im;
        const T* const ob = eb + p.quarters[1];
        T* const o0 = d + cell.out0;
        T* const o1 = d + cell.out1;
        T* const o2 = d + cell.out2;
        T* const o3 = d + cell.out3;
        const T* const tq = strip + 2 * static_cast<usize>(cx);
        const typename V::v cc = V::set1(tq[0]);
        const typename V::v ss = V::set1(tq[1]);
        const u32 stretch = Gapped ? p.stretch : p.chunk;
        const u32 end = Gapped ? p.chunk : 1;
        const u32 step = Gapped ? stretch : 1;
        for (u32 z0 = 0; z0 < end; z0 += step) {
            const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
            const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
            for (u32 z = 0; z < stretch; z += V::L) {
                const u32 i = i0 + z;
                const u32 j = j0 + z;
                const typename V::v vea = walk_load<V, In>(ea + i);
                const typename V::v voa = walk_load<V, In>(oa + i);
                const typename V::v veb = walk_load<V, In>(eb + i);
                const typename V::v vob = walk_load<V, In>(ob + i);
                const typename V::v r = V::fms(V::mul(cc, voa), ss, vob);
                const typename V::v im = V::fma(V::mul(ss, voa), cc, vob);
                V::st(o0 + j, V::add(vea, r));
                V::st(o1 + j, V::add(veb, im));
                V::st(o2 + j, V::sub(vea, r));
                V::st(o3 + j, V::sub(im, veb));
            }
        }
    }
}

// One step of the forward schedule, with vector policy V.
template <typename V, bool Gapped, bool In, typename T>
void forward_segment(const WalkPass& p, const WalkSegment& g, const T* const s, T* const d) noexcept {
    if (p.radix == 4) {
        switch (g.part) {
        case WalkPart::dc:
            forward4_dc<V, Gapped, In>(p, s, d);
            return;
        case WalkPart::ridge:
            forward4_ridge<V, Gapped, In>(p, s, d);
            return;
        case WalkPart::cells:
            forward4_cells<V, Gapped, In>(p, s, d, g.first, g.count);
            return;
        }
        return;
    }
    switch (g.part) {
    case WalkPart::dc:
        forward2_dc<V, Gapped, In>(p, s, d);
        return;
    case WalkPart::ridge:
        forward2_ridge<V, Gapped, In>(p, s, d);
        return;
    case WalkPart::cells:
        forward2_cells<V, Gapped, In>(p, s, d, g.first, g.count);
        return;
    }
}

// A forward step with its pass's choices made once: Gapped (its runs cross
// gaps) and In (it reads the caller's input, so its loads flush).
template <typename V, typename T>
void forward_step(const WalkPass& p, const WalkSegment& g, const T* const s, T* const d) noexcept {
    const bool gapped = p.chunk > p.stretch;
    const bool in = static_cast<u32>(p.source) == 0;
    if (gapped && in) forward_segment<V, true, true>(p, g, s, d);
    if (gapped && !in) forward_segment<V, true, false>(p, g, s, d);
    if (!gapped && in) forward_segment<V, false, true>(p, g, s, d);
    if (!gapped && !in) forward_segment<V, false, false>(p, g, s, d);
}

// ---------------------------------------------------------------------------
// Inverse passes: each forward pass undone. s holds the state after the
// pass, d receives the state before it.
// ---------------------------------------------------------------------------

template <typename V, bool Gapped, bool Out, typename T>
void inverse4_dc(const WalkPass& p, const T* const s, T* const d) noexcept {
    const typename V::v q25 = V::set1(static_cast<T>(0.25));
    const typename V::v h50 = V::set1(static_cast<T>(0.5));
    const T* const o0 = s + p.dc_out[0];
    const T* const o1 = s + p.dc_out[1];
    const T* const o2 = s + p.dc_out[2];
    const T* const o3 = s + p.dc_out[3];
    T* const a = d + p.dc_in;
    T* const b = a + p.quarters[1];
    T* const c = a + p.quarters[2];
    T* const e = a + p.quarters[3];
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v x0 = V::ld(o0 + j);
            const typename V::v x1 = V::ld(o1 + j);
            const typename V::v x2 = V::ld(o2 + j);
            const typename V::v x3 = V::ld(o3 + j);
            const typename V::v su = V::mul(q25, V::add(x0, x1));
            const typename V::v di = V::mul(q25, V::sub(x0, x1));
            walk_store<V, Out>(a + i, V::fma(su, h50, x2));
            walk_store<V, Out>(c + i, V::fms(su, h50, x2));
            walk_store<V, Out>(b + i, V::fma(di, h50, x3));
            walk_store<V, Out>(e + i, V::fms(di, h50, x3));
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse4_ridge(const WalkPass& p, const T* const s, T* const d) noexcept {
    const typename V::v h50 = V::set1(static_cast<T>(0.5));
    const typename V::v kh = V::set1(walk_root_quarter<T>);
    const T* const o0 = s + p.ridge_out[0];
    const T* const o1 = s + p.ridge_out[1];
    const T* const o2 = s + p.ridge_out[2];
    const T* const o3 = s + p.ridge_out[3];
    T* const a = d + p.ridge_in;
    T* const b = a + p.quarters[1];
    T* const c = a + p.quarters[2];
    T* const e = a + p.quarters[3];
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v x0 = V::ld(o0 + j);
            const typename V::v x1 = V::ld(o1 + j);
            const typename V::v x2 = V::ld(o2 + j);
            const typename V::v x3 = V::ld(o3 + j);
            const typename V::v r2 = V::sub(x0, x2);
            const typename V::v i2 = V::add(x1, x3);
            walk_store<V, Out>(a + i, V::mul(h50, V::add(x0, x2)));
            walk_store<V, Out>(c + i, V::mul(h50, V::sub(x1, x3)));
            walk_store<V, Out>(b + i, V::mul(kh, V::add(i2, r2)));
            walk_store<V, Out>(e + i, V::mul(kh, V::sub(i2, r2)));
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse4_cells(const WalkPass& p, const T* const s, T* const d, const u32 first, const u32 count) noexcept {
    const T* const strip = strip_of<T>(p);
    const typename V::v q25 = V::set1(static_cast<T>(0.25));
    const u32 f0 = p.offsets[0];
    const u32 f1 = p.offsets[1];
    const u32 f2 = p.offsets[2];
    const u32 f3 = p.offsets[3];
    for (u32 cx = first; cx < first + count; ++cx) {
        const WalkCell4& cell = p.cells4[cx];
        T* const ar = d + cell.in_re;
        T* const br = ar + p.quarters[1];
        T* const cr = ar + p.quarters[2];
        T* const dr = ar + p.quarters[3];
        T* const ai = d + cell.in_im;
        T* const bi = ai + p.quarters[1];
        T* const ci = ai + p.quarters[2];
        T* const di = ai + p.quarters[3];
        const T* const o0 = s + cell.out_o + f0;
        const T* const o1 = s + cell.out_o + f1;
        const T* const o2 = s + cell.out_o + f2;
        const T* const o3 = s + cell.out_o + f3;
        const T* const b0 = s + cell.out_b + f3;
        const T* const b1 = s + cell.out_b + f0;
        const T* const b2 = s + cell.out_b + f1;
        const T* const b3 = s + cell.out_b + f2;
        const T* const tq = strip + 4 * static_cast<usize>(cx);
        const T quarter = static_cast<T>(0.25);
        const typename V::v c1 = V::set1(tq[0]);
        const typename V::v s1 = V::set1(tq[1]);
        const typename V::v c1q = V::set1(quarter * tq[0]);
        const typename V::v s1q = V::set1(quarter * tq[1]);
        const typename V::v c2q = V::set1(quarter * tq[2]);
        const typename V::v s2q = V::set1(quarter * tq[3]);
        const u32 stretch = Gapped ? p.stretch : p.chunk;
        const u32 end = Gapped ? p.chunk : 1;
        const u32 step = Gapped ? stretch : 1;
        for (u32 z0 = 0; z0 < end; z0 += step) {
            const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
            const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
            for (u32 z = 0; z < stretch; z += V::L) {
                const u32 i = i0 + z;
                const u32 j = j0 + z;
                const typename V::v xo0 = V::ld(o0 + j);
                const typename V::v xo1 = V::ld(o1 + j);
                const typename V::v xo2 = V::ld(o2 + j);
                const typename V::v xo3 = V::ld(o3 + j);
                const typename V::v xb0 = V::ld(b0 + j);
                const typename V::v xb1 = V::ld(b1 + j);
                const typename V::v xb2 = V::ld(b2 + j);
                const typename V::v xb3 = V::ld(b3 + j);
                const typename V::v LA = V::add(xo0, xb2);
                const typename V::v R3 = V::sub(xo0, xb2);
                const typename V::v LB = V::sub(xb0, xo2);
                const typename V::v I3 = V::add(xb0, xo2);
                const typename V::v HA = V::add(xb1, xo1);
                const typename V::v R4 = V::sub(xb1, xo1);
                const typename V::v HB = V::sub(xo3, xb3);
                const typename V::v I4 = V::add(xo3, xb3);
                const typename V::v lbA = V::fma(V::mul(c2q, R3), s2q, I3);
                const typename V::v lbB = V::fms(V::mul(c2q, I3), s2q, R3);
                const typename V::v hbA = V::fma(V::mul(s2q, R4), c2q, I4);
                const typename V::v hbB = V::fms(V::mul(s2q, I4), c2q, R4);
                const typename V::v rr = V::sub(LA, HA);
                const typename V::v ii = V::add(LB, HB);
                const typename V::v r2 = V::sub(lbA, hbA);
                const typename V::v i2 = V::add(lbB, hbB);
                walk_store<V, Out>(ar + i, V::mul(q25, V::add(LA, HA)));
                walk_store<V, Out>(ai + i, V::mul(q25, V::sub(LB, HB)));
                walk_store<V, Out>(cr + i, V::fma(V::mul(c1q, rr), s1q, ii));
                walk_store<V, Out>(ci + i, V::fms(V::mul(c1q, ii), s1q, rr));
                walk_store<V, Out>(br + i, V::add(lbA, hbA));
                walk_store<V, Out>(bi + i, V::sub(lbB, hbB));
                walk_store<V, Out>(dr + i, V::fma(V::mul(c1, r2), s1, i2));
                walk_store<V, Out>(di + i, V::fms(V::mul(c1, i2), s1, r2));
            }
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse2_dc(const WalkPass& p, const T* const s, T* const d) noexcept {
    const typename V::v h50 = V::set1(static_cast<T>(0.5));
    const T* const o0 = s + p.dc_out[0];
    const T* const o1 = s + p.dc_out[1];
    T* const a = d + p.dc_in;
    T* const b = a + p.quarters[1];
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            const typename V::v x0 = V::ld(o0 + j);
            const typename V::v x1 = V::ld(o1 + j);
            walk_store<V, Out>(a + i, V::mul(h50, V::add(x0, x1)));
            walk_store<V, Out>(b + i, V::mul(h50, V::sub(x0, x1)));
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse2_ridge(const WalkPass& p, const T* const s, T* const d) noexcept {
    const T* const o0 = s + p.ridge_out[0];
    const T* const o1 = s + p.ridge_out[1];
    T* const a = d + p.ridge_in;
    T* const b = a + p.quarters[1];
    if (o0 == a && s == d) return;
    const u32 stretch = Gapped ? p.stretch : p.chunk;
    const u32 end = Gapped ? p.chunk : 1;
    const u32 step = Gapped ? stretch : 1;
    for (u32 z0 = 0; z0 < end; z0 += step) {
        const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
        const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
        for (u32 z = 0; z < stretch; z += V::L) {
            const u32 i = i0 + z;
            const u32 j = j0 + z;
            walk_store<V, Out>(a + i, V::ld(o0 + j));
            walk_store<V, Out>(b + i, V::ld(o1 + j));
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse2_cells(const WalkPass& p, const T* const s, T* const d, const u32 first, const u32 count) noexcept {
    const T* const strip = strip_of<T>(p);
    const typename V::v h50 = V::set1(static_cast<T>(0.5));
    const T half = static_cast<T>(0.5);
    for (u32 cx = first; cx < first + count; ++cx) {
        const WalkCell2& cell = p.cells2[cx];
        T* const ea = d + cell.in_re;
        T* const oa = ea + p.quarters[1];
        T* const eb = d + cell.in_im;
        T* const ob = eb + p.quarters[1];
        const T* const o0 = s + cell.out0;
        const T* const o1 = s + cell.out1;
        const T* const o2 = s + cell.out2;
        const T* const o3 = s + cell.out3;
        const T* const tq = strip + 2 * static_cast<usize>(cx);
        const typename V::v ch = V::set1(half * tq[0]);
        const typename V::v sh = V::set1(half * tq[1]);
        const u32 stretch = Gapped ? p.stretch : p.chunk;
        const u32 end = Gapped ? p.chunk : 1;
        const u32 step = Gapped ? stretch : 1;
        for (u32 z0 = 0; z0 < end; z0 += step) {
            const u32 i0 = Gapped ? walk_in_place(p, z0) : 0;
            const u32 j0 = Gapped ? walk_out_place(p, z0) : 0;
            for (u32 z = 0; z < stretch; z += V::L) {
                const u32 i = i0 + z;
                const u32 j = j0 + z;
                const typename V::v x0 = V::ld(o0 + j);
                const typename V::v x1 = V::ld(o1 + j);
                const typename V::v x2 = V::ld(o2 + j);
                const typename V::v x3 = V::ld(o3 + j);
                const typename V::v rr = V::sub(x0, x2);
                const typename V::v ii = V::add(x1, x3);
                walk_store<V, Out>(ea + i, V::mul(h50, V::add(x0, x2)));
                walk_store<V, Out>(eb + i, V::mul(h50, V::sub(x1, x3)));
                walk_store<V, Out>(oa + i, V::fma(V::mul(ch, rr), sh, ii));
                walk_store<V, Out>(ob + i, V::fms(V::mul(ch, ii), sh, rr));
            }
        }
    }
}

template <typename V, bool Gapped, bool Out, typename T>
void inverse_segment(const WalkPass& p, const WalkSegment& g, const T* const s, T* const d) noexcept {
    if (p.radix == 4) {
        switch (g.part) {
        case WalkPart::dc:
            inverse4_dc<V, Gapped, Out>(p, s, d);
            return;
        case WalkPart::ridge:
            inverse4_ridge<V, Gapped, Out>(p, s, d);
            return;
        case WalkPart::cells:
            inverse4_cells<V, Gapped, Out>(p, s, d, g.first, g.count);
            return;
        }
        return;
    }
    switch (g.part) {
    case WalkPart::dc:
        inverse2_dc<V, Gapped, Out>(p, s, d);
        return;
    case WalkPart::ridge:
        inverse2_ridge<V, Gapped, Out>(p, s, d);
        return;
    case WalkPart::cells:
        inverse2_cells<V, Gapped, Out>(p, s, d, g.first, g.count);
        return;
    }
}

// An inverse step with its pass's choices made once: Gapped, and Out (it
// writes the caller's output, buffer 0, so its stores flush).
template <typename V, typename T>
void inverse_step(const WalkPass& p, const WalkSegment& g, const T* const s, T* const d) noexcept {
    const bool gapped = p.chunk > p.stretch;
    const bool out = static_cast<u32>(p.source) == 0;
    if (gapped && out) inverse_segment<V, true, true>(p, g, s, d);
    if (gapped && !out) inverse_segment<V, true, false>(p, g, s, d);
    if (!gapped && out) inverse_segment<V, false, true>(p, g, s, d);
    if (!gapped && !out) inverse_segment<V, false, false>(p, g, s, d);
}

// ---------------------------------------------------------------------------
// The terminal pass: the last two stages, one row of four reals at a time,
// on the two-lane policy H. Rows 0 and N/8 are scalar.
// ---------------------------------------------------------------------------

// The larger angle 2k of row k, read straight (k <= N/16) or from its
// complement (k > N/16): Complement selects the region, so the row loop
// decides nothing.
template <typename T, bool Complement>
struct LargerAngle final {
    [[nodiscard]] static T cosine(const T* const table, const u32 eighth, const u32 k) noexcept {
        if constexpr (Complement) {
            return table[2 * (2 * eighth - 2 * k) + 1];
        } else {
            return table[2 * (2 * k)];
        }
    }
    [[nodiscard]] static T sine(const T* const table, const u32 eighth, const u32 k) noexcept {
        if constexpr (Complement) {
            return table[2 * (2 * eighth - 2 * k)];
        } else {
            return table[2 * (2 * k) + 1];
        }
    }
};

// What a forward row's two rotations leave: the sums S1 (bins k, N/4 - k)
// and S2 (bins N/2 - k, N/4 + k) of the real parts, and the matching
// imaginary sums before the sign of the first is taken (P1 + Q, P2 - Q).
template <typename H>
struct RowSums final {
    typename H::v real_near;   // S1
    typename H::v real_far;    // S2
    typename H::v imag_near;   // PB + i34 (minus Im of bins k, N/4 - k)
    typename H::v imag_far;    // PB - i34 (Im of bins N/2 - k, N/4 + k)
};

template <typename H, typename T, bool Complement, bool In>
[[nodiscard]] RowSums<H> forward_row(const WalkTables& tables, const WalkRow& w, const T* const s) noexcept {
    const T* const table = twiddles_of<T>(tables);
    const T* const re = s + w.in_re;
    const T* const im = s + w.in_im;
    const typename H::v R0 = walk_load<H, In>(re);
    const typename H::v R1 = walk_load<H, In>(re + 2);
    const typename H::v I0 = walk_load<H, In>(im);
    const typename H::v I1 = walk_load<H, In>(im + 2);
    const typename H::v c1 = H::set1(LargerAngle<T, Complement>::cosine(table, tables.eighth, w.k));
    const typename H::v s1 = H::set1(LargerAngle<T, Complement>::sine(table, tables.eighth, w.k));
    const typename H::v r = H::fms(H::mul(c1, R1), s1, I1);
    const typename H::v i = H::fma(H::mul(s1, R1), c1, I1);
    const typename H::v LA = H::add(R0, r);
    const typename H::v LB = H::add(I0, i);
    const typename H::v HA = H::sub(R0, r);
    const typename H::v HB = H::sub(i, I0);
    const typename H::v U = H::zip2(LA, HA);
    const typename H::v V = H::zip2(LB, HB);
    const typename H::v PA = H::zip1(LA, HA);
    const typename H::v PB = H::zip1(LB, HB);
    const typename H::v Tc = H::ld(table + 2 * w.k);
    const typename H::v Ts = H::swap(Tc);
    const typename H::v r34 = H::fms(H::mul(Tc, U), Ts, V);
    const typename H::v i34 = H::fma(H::mul(Ts, U), Tc, V);
    RowSums<H> sums;
    sums.real_near = H::add(PA, r34);
    sums.real_far = H::sub(PA, r34);
    sums.imag_near = H::add(PB, i34);
    sums.imag_far = H::sub(PB, i34);
    return sums;
}

template <typename H, typename T, bool Complement, bool In>
void terminal_rows(const WalkTables& tables, const T* const s, T* const X, const u32 first, const u32 last) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const WalkTerminal& p = tables.terminal;
    for (u32 index = first; index < last; ++index) {
        const WalkRow& w = p.rows[index];
        const RowSums<H> sums = forward_row<H, T, Complement, In>(tables, w, s);
        const typename H::v N1 = H::neg(sums.imag_near);
        const u32 k = w.k;
        walk_put<H>(X + 2 * k, H::zip1(sums.real_near, N1));
        walk_put<H>(X + 2 * (quarter - k), H::zip2(sums.real_near, N1));
        walk_put<H>(X + 2 * (half - k), H::zip1(sums.real_far, sums.imag_far));
        walk_put<H>(X + 2 * (quarter + k), H::zip2(sums.real_far, sums.imag_far));
    }
}

template <typename H, typename T, bool Complement, bool In>
void terminal_magnitude_rows(const WalkTables& tables, const T* const s, T* const mag, const u32 first,
                             const u32 last) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const WalkTerminal& p = tables.terminal;
    for (u32 index = first; index < last; ++index) {
        const WalkRow& w = p.rows[index];
        const RowSums<H> sums = forward_row<H, T, Complement, In>(tables, w, s);
        const typename H::v M1 = H::sqrt(H::fma(H::mul(sums.real_near, sums.real_near), sums.imag_near, sums.imag_near));
        const typename H::v M2 = H::sqrt(H::fma(H::mul(sums.real_far, sums.real_far), sums.imag_far, sums.imag_far));
        const u32 k = w.k;
        walk_putl<H>(mag + k, M1, 0);
        walk_putl<H>(mag + (quarter - k), M1, 1);
        walk_putl<H>(mag + (half - k), M2, 0);
        walk_putl<H>(mag + (quarter + k), M2, 1);
    }
}

// Rows 0 and N/8 of the forward terminal: bins 0, N/4, N/2 and N/8, 3N/8.
// Written interleaved (re, im) to X.
template <typename T, bool In>
void terminal_edges(const WalkTables& tables, const T* const s, T* const X) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const u32 k8 = quarter >> 1;
    const WalkTerminal& p = tables.terminal;
    const T* const r = s + p.dc_in;
    const T a = walk_in<T, In>(r[0]);
    const T b = walk_in<T, In>(r[1]);
    const T c = walk_in<T, In>(r[2]);
    const T e = walk_in<T, In>(r[3]);
    X[0] = walk_flush<T>((a + c) + (b + e));
    X[1] = static_cast<T>(0);
    X[2 * half] = walk_flush<T>((a + c) - (b + e));
    X[2 * half + 1] = static_cast<T>(0);
    X[2 * quarter] = walk_flush<T>(a - c);
    X[2 * quarter + 1] = walk_flush<T>(e - b);
    const T* const y = s + p.ridge_in;
    const T k = walk_root_half<T>;
    const T ya = walk_in<T, In>(y[0]);
    const T yb = walk_in<T, In>(y[1]);
    const T yc = walk_in<T, In>(y[2]);
    const T ye = walk_in<T, In>(y[3]);
    const T rr = k * (yb - ye);
    const T ii = k * (yb + ye);
    X[2 * k8] = walk_flush<T>(ya + rr);
    X[2 * k8 + 1] = walk_flush<T>(-(yc + ii));
    X[2 * (quarter + k8)] = walk_flush<T>(ya - rr);
    X[2 * (quarter + k8) + 1] = walk_flush<T>(yc - ii);
}

template <typename T, bool In>
void terminal_magnitude_edges(const WalkTables& tables, const T* const s, T* const mag) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const u32 k8 = quarter >> 1;
    const WalkTerminal& p = tables.terminal;
    const T* const r = s + p.dc_in;
    const T a = walk_in<T, In>(r[0]);
    const T b = walk_in<T, In>(r[1]);
    const T c = walk_in<T, In>(r[2]);
    const T e = walk_in<T, In>(r[3]);
    mag[0] = walk_flush<T>(std::fabs((a + c) + (b + e)));
    mag[half] = walk_flush<T>(std::fabs((a + c) - (b + e)));
    mag[quarter] = walk_flush<T>(std::sqrt((a - c) * (a - c) + (e - b) * (e - b)));
    const T* const y = s + p.ridge_in;
    const T k = walk_root_half<T>;
    const T ya = walk_in<T, In>(y[0]);
    const T yb = walk_in<T, In>(y[1]);
    const T yc = walk_in<T, In>(y[2]);
    const T ye = walk_in<T, In>(y[3]);
    const T rr = k * (yb - ye);
    const T ii = k * (yb + ye);
    mag[k8] = walk_flush<T>(std::sqrt((ya + rr) * (ya + rr) + (yc + ii) * (yc + ii)));
    mag[quarter + k8] = walk_flush<T>(std::sqrt((ya - rr) * (ya - rr) + (yc - ii) * (yc - ii)));
}

// One inverse terminal row region (Complement as for the forward rows).
template <typename H, typename T, bool Complement, bool Out>
void inverse_terminal_rows(const WalkTables& tables, const T* const X, T* const d, const u32 first,
                           const u32 last) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const T* const table = twiddles_of<T>(tables);
    const WalkTerminal& p = tables.terminal;
    const typename H::v q25 = H::set1(static_cast<T>(0.25));
    const T quarter_scale = static_cast<T>(0.25);
    for (u32 index = first; index < last; ++index) {
        const WalkRow& w = p.rows[index];
        const u32 k = w.k;
        const typename H::v v0 = walk_get<H>(X + 2 * k);
        const typename H::v v1 = walk_get<H>(X + 2 * (quarter - k));
        const typename H::v v2 = walk_get<H>(X + 2 * (half - k));
        const typename H::v v3 = walk_get<H>(X + 2 * (quarter + k));
        const typename H::v S1 = H::zip1(v0, v1);
        const typename H::v N1 = H::zip2(v0, v1);
        const typename H::v S2 = H::zip1(v2, v3);
        const typename H::v N2 = H::zip2(v2, v3);
        const typename H::v PA = H::add(S1, S2);
        const typename H::v R2 = H::sub(S1, S2);
        const typename H::v PB = H::sub(N2, N1);
        const typename H::v In = H::add(N1, N2);
        const typename H::v Tc = H::ld(table + 2 * k);
        const typename H::v Ts = H::swap(Tc);
        const typename H::v U = H::fms(H::mul(Tc, R2), Ts, In);
        const typename H::v V = H::neg(H::fma(H::mul(Ts, R2), Tc, In));
        const typename H::v LA = H::zip1(PA, U);
        const typename H::v HA = H::zip2(PA, U);
        const typename H::v LB = H::zip1(PB, V);
        const typename H::v HB = H::zip2(PB, V);
        const typename H::v rr = H::sub(LA, HA);
        const typename H::v ii = H::add(LB, HB);
        const T c1 = LargerAngle<T, Complement>::cosine(table, tables.eighth, k);
        const T s1 = LargerAngle<T, Complement>::sine(table, tables.eighth, k);
        const typename H::v c1q = H::set1(quarter_scale * c1);
        const typename H::v s1q = H::set1(quarter_scale * s1);
        T* const re = d + w.in_re;
        T* const im = d + w.in_im;
        walk_store<H, Out>(re, H::mul(q25, H::add(LA, HA)));
        walk_store<H, Out>(im, H::mul(q25, H::sub(LB, HB)));
        walk_store<H, Out>(re + 2, H::fma(H::mul(c1q, rr), s1q, ii));
        walk_store<H, Out>(im + 2, H::fms(H::mul(c1q, ii), s1q, rr));
    }
}

// Rows 0 and N/8 of the inverse terminal.
template <typename T, bool Out>
void inverse_terminal_edges(const WalkTables& tables, const T* const X, T* const d) noexcept {
    const u32 quarter = static_cast<u32>(tables.n) >> 2;
    const u32 half = static_cast<u32>(tables.n) >> 1;
    const u32 k8 = quarter >> 1;
    const WalkTerminal& p = tables.terminal;
    const T h = static_cast<T>(0.5);
    const T q = static_cast<T>(0.25);
    const T o0 = walk_flush<T>(X[0]);
    const T o1 = walk_flush<T>(X[2 * half]);
    const T o2 = walk_flush<T>(X[2 * quarter]);
    const T o3 = -walk_flush<T>(X[2 * quarter + 1]);
    T* const r = d + p.dc_in;
    const T su = q * (o0 + o1);
    const T di = q * (o0 - o1);
    r[0] = walk_out<T, Out>(su + h * o2);
    r[2] = walk_out<T, Out>(su - h * o2);
    r[1] = walk_out<T, Out>(di + h * o3);
    r[3] = walk_out<T, Out>(di - h * o3);
    const T y0 = walk_flush<T>(X[2 * k8]);
    const T y1 = -walk_flush<T>(X[2 * k8 + 1]);
    const T y2 = walk_flush<T>(X[2 * (quarter + k8)]);
    const T y3 = -walk_flush<T>(X[2 * (quarter + k8) + 1]);
    const T kh = walk_root_quarter<T>;
    const T r2 = y0 - y2;
    const T i2 = y1 + y3;
    T* const y = d + p.ridge_in;
    y[0] = walk_out<T, Out>(h * (y0 + y2));
    y[2] = walk_out<T, Out>(h * (y1 - y3));
    y[1] = walk_out<T, Out>(kh * (i2 + r2));
    y[3] = walk_out<T, Out>(kh * (i2 - r2));
}

// ---------------------------------------------------------------------------
// The transforms.
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] WalkBuffers<T> buffers_of(const WalkTables& tables, T* const caller, T* const work) noexcept {
    WalkBuffers<T> result{{caller, work, work + tables.elements + tables.stagger}};
    return result;
}

// Every forward pass in schedule order; the terminal's source is left in
// the buffer the terminal names. Each pass picks its policy once: W when its
// runs hold whole W vectors, else the narrower Nr.
template <typename T>
void forward_passes(const WalkTables& tables, const T* const x, T* const work) noexcept {
    using W = typename WalkPolicies<T>::W;
    using Nr = typename WalkPolicies<T>::Nr;
    WalkBuffers<T> b = buffers_of<T>(tables, nullptr, work);
    for (u32 index = 0; index < tables.segment_count; ++index) {
        const WalkSegment& g = tables.segments[index];
        const WalkPass& p = tables.passes[g.pass];
        const u32 from = static_cast<u32>(p.source);
        const T* const s = from == 0 ? x : b.buffer[from];
        T* const d = b.buffer[static_cast<u32>(p.destination)];
        if (p.chunk % W::L == 0) {
            forward_step<W>(p, g, s, d);
        } else {
            forward_step<Nr>(p, g, s, d);
        }
    }
}

template <typename T>
void walk_forward(const WalkTables& tables, const T* const x, T* const X, T* const work) noexcept {
    using H = typename WalkPolicies<T>::H;
    forward_passes<T>(tables, x, work);
    const WalkBuffers<T> b = buffers_of<T>(tables, nullptr, work);
    const T* const s = b.buffer[static_cast<u32>(tables.terminal.source)];
    // The terminal reads the caller's input only when no pass precedes it
    // (terminal.source 0); then its loads flush.
    if (static_cast<u32>(tables.terminal.source) == 0) {
        terminal_edges<T, true>(tables, s, X);
        terminal_rows<H, T, false, true>(tables, s, X, 0, tables.terminal.direct_rows);
        terminal_rows<H, T, true, true>(tables, s, X, tables.terminal.direct_rows, tables.terminal.row_count);
    } else {
        terminal_edges<T, false>(tables, s, X);
        terminal_rows<H, T, false, false>(tables, s, X, 0, tables.terminal.direct_rows);
        terminal_rows<H, T, true, false>(tables, s, X, tables.terminal.direct_rows, tables.terminal.row_count);
    }
}

template <typename T>
void walk_magnitude(const WalkTables& tables, const T* const x, T* const mag, T* const work) noexcept {
    using H = typename WalkPolicies<T>::H;
    forward_passes<T>(tables, x, work);
    const WalkBuffers<T> b = buffers_of<T>(tables, nullptr, work);
    const T* const s = b.buffer[static_cast<u32>(tables.terminal.source)];
    if (static_cast<u32>(tables.terminal.source) == 0) {
        terminal_magnitude_edges<T, true>(tables, s, mag);
        terminal_magnitude_rows<H, T, false, true>(tables, s, mag, 0, tables.terminal.direct_rows);
        terminal_magnitude_rows<H, T, true, true>(tables, s, mag, tables.terminal.direct_rows, tables.terminal.row_count);
    } else {
        terminal_magnitude_edges<T, false>(tables, s, mag);
        terminal_magnitude_rows<H, T, false, false>(tables, s, mag, 0, tables.terminal.direct_rows);
        terminal_magnitude_rows<H, T, true, false>(tables, s, mag, tables.terminal.direct_rows, tables.terminal.row_count);
    }
}

// The exact inverse of walk_forward: every pass undone in reverse schedule
// order, so inverse(forward(x)) = x with no separate scaling. The caller's
// output serves as the buffer of the first pass's input.
template <typename T>
void walk_inverse(const WalkTables& tables, const T* const X, T* const out, T* const work) noexcept {
    using W = typename WalkPolicies<T>::W;
    using Nr = typename WalkPolicies<T>::Nr;
    using H = typename WalkPolicies<T>::H;
    WalkBuffers<T> b = buffers_of<T>(tables, out, work);
    T* const t = b.buffer[static_cast<u32>(tables.terminal.source)];
    // The terminal writes the caller's output only when no pass follows it
    // (terminal.source 0); then its stores flush.
    if (static_cast<u32>(tables.terminal.source) == 0) {
        inverse_terminal_edges<T, true>(tables, X, t);
        inverse_terminal_rows<H, T, false, true>(tables, X, t, 0, tables.terminal.direct_rows);
        inverse_terminal_rows<H, T, true, true>(tables, X, t, tables.terminal.direct_rows, tables.terminal.row_count);
    } else {
        inverse_terminal_edges<T, false>(tables, X, t);
        inverse_terminal_rows<H, T, false, false>(tables, X, t, 0, tables.terminal.direct_rows);
        inverse_terminal_rows<H, T, true, false>(tables, X, t, tables.terminal.direct_rows, tables.terminal.row_count);
    }
    for (u32 index = tables.segment_count; index-- > 0;) {
        const WalkSegment& g = tables.segments[index];
        const WalkPass& p = tables.passes[g.pass];
        const T* const s = b.buffer[static_cast<u32>(p.destination)];
        T* const d = b.buffer[static_cast<u32>(p.source)];
        if (p.chunk % W::L == 0) {
            inverse_step<W>(p, g, s, d);
        } else {
            inverse_step<Nr>(p, g, s, d);
        }
    }
}
