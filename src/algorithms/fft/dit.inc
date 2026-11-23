// BFFT's real DIT kernel (src/detail/bruun_dit_kernel.hpp at edf5f01): the
// mirrored-lane walk in the normalised (unit-frame) basis, BFFT's live
// forward_simd / inverse_simd and their float32 forms. Included only by
// variant.inc, after lanes.inc.
//
// The walk. Seed nodes are gathered from the input in bit-reversed order,
// radix-4 merges reduce children modulo the real quadratic factors of
// z^N - 1, and bin k leaves the tree as a residue pair (re, -im). The two
// lanes of every V2 are the mirror trees of node b and node b + N/4, which
// run identical butterflies with identical twiddles below the last merge
// and read adjacent inputs (rev(b + N/4) = rev(b) + 1); the one lane
// crossing is the terminal merge, fused with the projection to bins. The
// float32 walk adds a second lane axis, the cA / cB halves the radix-4
// terminal combines, so each V4F holds nodes {s, s + N/4, s + N/8, s + 3N/8}.
// Each merge is a Givens rotation with c^2 + s^2 = 1 from the (cos, sin)
// tables. The inverse runs the same walk backwards; its 0.5 factors
// compose to 1/N, so irfft(rfft(x)) = x as for DIF.
//
// Differences from BFFT, none of which changes a value:
//   - BFFT runs this walk at STXLIB_FFT_LEVEL >= 1 for N >= 16 and a scalar
//     walk in a different (raw) basis otherwise. Here the walk runs at every
//     tier; the scalar tier's V2 and V4F are lane arrays (lanes.inc), so each
//     lane computes what the vector lane computes and the tiers agree by
//     construction. Plans run DIT only from N = 16 (plan.cpp).
//   - The tables are the plan's (DitTables): the twiddles come from
//     sincospi, as every stx_fft table does (plan.cpp), where BFFT calls
//     cos and sin; the float kernels round them where they use them, as
//     BFFT does.
//   - BFFT's raw-basis twiddle table, its scalar walk, the split-output
//     writer and its debug asserts are not copied.

// BFFT's bitrev_int (bruun_simd_backend.hpp): r reversed over t bits.
inline int dit_bitrev(int r, int t) noexcept {
    int out = 0;
    for (int i = 0; i < t; ++i) {
        out = (out << 1) | (r & 1);
        r >>= 1;
    }
    return out;
}

class DitKernel final {
public:
    explicit DitKernel(const DitTables& tables) noexcept
        : n_(tables.n), half_(tables.half), twiddle_stage_count_(tables.twiddle_stage_count),
          radix2_terminal_(tables.radix2_terminal), rev_(tables.rev), twiddle_offset_(tables.twiddle_offset),
          cos_(tables.cos), sin_(tables.sin), cs_(tables.cs), fterm_cs_(tables.fterm_cs) {}

    // float64 forward (BFFT's forward_simd, N >= 16): work holds N doubles.
    void forward64(const double* input, Complex64* output, double* work) const {
        compute_vwork_norm(input, work);
        const terminal_complex_writer<Complex64> w{output};
        if (radix2_terminal_) {
            terminal_radix2_v_norm(work, w);
        } else {
            terminal_v_norm(work, w);
        }
    }

    // float64 inverse (BFFT's inverse_simd, N >= 16). BFFT measured a fused
    // three-level odd-size terminal 8-21% slower than these two passes.
    void inverse64(const Complex64* input, double* output, double* work) const {
        if (radix2_terminal_) {
            terminal_radix2_inv_v_norm(input, work);
        } else {
            terminal_inv_v_norm(input, work);
        }
        compute_vwork_inv_norm(work, output);
    }

    // float32 (BFFT's forward_simd_f32 / inverse_simd_f32, N >= 16): work
    // holds N floats.
    void forward32(const float* input, Complex32* output, float* work) const {
        forward_simd_f32_wide(input, output, work);
    }

    void inverse32(const Complex32* input, float* output, float* work) const {
        inverse_simd_f32_wide(input, output, work);
    }

private:
    // DIT blocking thresholds are byte-derived so double/f32 paths stay in
    // cache-footprint parity. 32768 bytes was the measured point where a
    // single fused seed block stopped being the best default on the NEON
    // development target; 256 is the cache-blocked stage size in elements.
    static constexpr int kFuseBlockBytes = 32768;
    static constexpr int kBlockedStageSize = 256;
    static constexpr int kRowTileBlockCount = 64;
    static constexpr int kF32MinInverseChain = 4;

    template <typename T>
    static bool should_cache_block(int n) {
        return static_cast<usize>(n) * sizeof(T) >= static_cast<usize>(kFuseBlockBytes);
    }

    static int bitrev_int(int r, int t) noexcept { return dit_bitrev(r, t); }

    int n_;
    int half_;
    int twiddle_stage_count_;
    bool radix2_terminal_;
    const int* rev_;
    const int* twiddle_offset_;
    const double* cos_;
    const double* sin_;
    const double* cs_;
    const double* fterm_cs_;

    // Normalized-basis scalar merge taking a precomputed rotation (c, s) from
    // the cos_/sin_ tables instead of deriving s = sqrt(1 - c^2) from a
    // 2*cos twiddle. Same result as pair_reduce, without the small-angle sqrt
    // cancellation; used by the SIMD terminals' scalar node specials.
    static STXLIB_FFT_INLINE void pair_reduce_cs(double ea, double eb, double oa, double ob,
                                                   double c, double s,
                                                   double& la, double& lb, double& ha, double& hb) {
        const double r = c * oa - s * ob;
        const double i = s * oa + c * ob;
        la = ea + r;
        lb = eb + i;
        ha = ea - r;
        hb = i - eb;
    }

    static STXLIB_FFT_INLINE void pair_expand_cs(double la, double lb, double ha, double hb,
                                                   double c, double s,
                                                   double& ea, double& eb, double& oa, double& ob) {
        const double r = 0.5 * (la - ha);
        const double i = 0.5 * (lb + hb);
        ea = 0.5 * (la + ha);
        eb = 0.5 * (lb - hb);
        oa = c * r + s * i;
        ob = c * i - s * r;
    }

    // ------------------------------------------------------------------
    // Mirrored-lane SIMD walk. Lanes of every vector hold node s (lane 0)
    // and node s + N/4 (lane 1); see the header commentary.
    // ------------------------------------------------------------------

    // Normalized-basis merge, SIMD form of the scalar pair_reduce: the odd
    // child (oa, ob) is rotated by (c, s) = (cos, sin) before combining with
    // the even child. Because c^2 + s^2 = 1 exactly (no 2*cos twiddle that
    // grows toward the tree top, no 1 - (2cos)^2 cancellation), residue pairs
    // stay in the local unit frame and the leaf pair is directly (re, -im).
    static STXLIB_FFT_INLINE void pair_reduce_v2_norm(V2 ea, V2 eb, V2 oa, V2 ob,
                                                        V2 c, V2 s,
                                                        V2& la, V2& lb, V2& ha, V2& hb) {
        const V2 r = v2_sub(v2_mul(c, oa), v2_mul(s, ob));   // c*oa - s*ob
        const V2 im = v2_add(v2_mul(s, oa), v2_mul(c, ob));  // s*oa + c*ob
        la = v2_add(ea, r);
        lb = v2_add(eb, im);
        ha = v2_sub(ea, r);
        hb = v2_sub(im, eb);
    }

    // Seed: fused half-split + radix-2 merge into standard s=4 blocks, both
    // mirror lanes at once. rev(b + N/4) == rev(b) + 1 makes both input reads
    // contiguous V2 loads; rev(b + 1) == rev(b) + N/8 removes the second
    // table lookup.
    void seed_block_v(const double* input, double* vw, int base, int count) const {
        const int hq = half_ >> 1;
        for (int b = base; b < base + count; b += 2) {
            const int j0 = rev_[b];
            const int j1 = j0 + hq;
            const V2 a0 = v2_ld(input + j0);
            const V2 c0 = v2_ld(input + j0 + half_);
            const V2 a1 = v2_ld(input + j1);
            const V2 c1 = v2_ld(input + j1 + half_);
            const V2 e0 = v2_add(a0, c0);
            const V2 e1 = v2_add(a1, c1);
            double* dst = vw + 4 * b;
            v2_st(dst, v2_add(e0, e1));
            v2_st(dst + 2, v2_sub(e0, e1));
            v2_st(dst + 4, v2_sub(a0, c0));
            v2_st(dst + 6, v2_sub(a1, c1));
        }
    }

    static STXLIB_FFT_INLINE void store_butterfly4_v(double* vblk, int M, int i, const V2* o) {
        v2_st(vblk + 4 * i, o[0]);
        v2_st(vblk + 4 * i + 2, o[1]);
        v2_st(vblk + 4 * (4 * M - i), o[2]);
        v2_st(vblk + 4 * (4 * M - i) + 2, o[3]);
        v2_st(vblk + 4 * (2 * M - i), o[4]);
        v2_st(vblk + 4 * (2 * M - i) + 2, o[5]);
        v2_st(vblk + 4 * (2 * M + i), o[6]);
        v2_st(vblk + 4 * (2 * M + i) + 2, o[7]);
    }

    // -------- Normalized-basis SIMD forward (same walk, stable rotation) -----
    // Identical mirrored-lane structure to butterfly4_v / merge4_v / the
    // driver above, but every pair_reduce runs the (c, s) rotation. Twiddle
    // pointers now index the {c, s} table cs_ (2 doubles per entry).

    STXLIB_FFT_INLINE void butterfly4_v_norm(V2 a0, V2 b0, V2 a1, V2 b1,
                                               V2 a2, V2 b2, V2 a3, V2 b3,
                                               const double* cs2, const double* cs1, const double* cs1q,
                                               V2* o) const {
        const V2 c2 = v2_set1(cs2[0]);
        const V2 s2 = v2_set1(cs2[1]);
        V2 h0la, h0lb, h0ha, h0hb, h1la, h1lb, h1ha, h1hb;
        pair_reduce_v2_norm(a0, b0, a1, b1, c2, s2, h0la, h0lb, h0ha, h0hb);
        pair_reduce_v2_norm(a2, b2, a3, b3, c2, s2, h1la, h1lb, h1ha, h1hb);
        pair_reduce_v2_norm(h0la, h0lb, h1la, h1lb, v2_set1(cs1[0]), v2_set1(cs1[1]),
                            o[0], o[1], o[2], o[3]);
        pair_reduce_v2_norm(h0ha, h0hb, h1ha, h1hb, v2_set1(cs1q[0]), v2_set1(cs1q[1]),
                            o[4], o[5], o[6], o[7]);
    }

    void merge4_v_norm(double* vblk, int M, const double* cs2, const double* cs1) const {
        double* c0 = vblk;
        double* c1 = vblk + 4 * M;
        double* c2 = vblk + 8 * M;
        double* c3 = vblk + 12 * M;

        const V2 dc0 = v2_ld(c0);
        const V2 ny0 = v2_ld(c0 + 2);
        const V2 dc1 = v2_ld(c1);
        const V2 ny1 = v2_ld(c1 + 2);
        const V2 dc2 = v2_ld(c2);
        const V2 ny2 = v2_ld(c2 + 2);
        const V2 dc3 = v2_ld(c3);
        const V2 ny3 = v2_ld(c3 + 2);
        V2 ma, mb, mha, mhb;
        pair_reduce_v2_norm(ny0, ny1, ny2, ny3, v2_set1(cs1[2 * (M - 1)]), v2_set1(cs1[2 * (M - 1) + 1]),
                            ma, mb, mha, mhb);
        const V2 h0dc = v2_add(dc0, dc1);
        const V2 h0ny = v2_sub(dc0, dc1);
        const V2 h1dc = v2_add(dc2, dc3);
        const V2 h1ny = v2_sub(dc2, dc3);
        v2_st(c0, v2_add(h0dc, h1dc));
        v2_st(c0 + 2, v2_sub(h0dc, h1dc));
        v2_st(c1, ma);
        v2_st(c1 + 2, mb);
        v2_st(c2, h0ny);
        v2_st(c2 + 2, h1ny);
        v2_st(c3, mha);
        v2_st(c3 + 2, mhb);

        for (int i = 1; i < M - i; ++i) {
            const int m = M - i;
            const V2 a0i = v2_ld(c0 + 4 * i);
            const V2 b0i = v2_ld(c0 + 4 * i + 2);
            const V2 a1i = v2_ld(c1 + 4 * i);
            const V2 b1i = v2_ld(c1 + 4 * i + 2);
            const V2 a2i = v2_ld(c2 + 4 * i);
            const V2 b2i = v2_ld(c2 + 4 * i + 2);
            const V2 a3i = v2_ld(c3 + 4 * i);
            const V2 b3i = v2_ld(c3 + 4 * i + 2);
            const V2 a1m = v2_ld(c1 + 4 * m);
            const V2 b1m = v2_ld(c1 + 4 * m + 2);
            const V2 a3m = v2_ld(c3 + 4 * m);
            const V2 b3m = v2_ld(c3 + 4 * m + 2);
            V2 oi[8];
            butterfly4_v_norm(a0i, b0i, a1i, b1i, a2i, b2i, a3i, b3i,
                              cs2 + 2 * (i - 1), cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), oi);
            store_butterfly4_v(vblk, M, i, oi);
            const V2 a0m = v2_ld(c0 + 4 * m);
            const V2 b0m = v2_ld(c0 + 4 * m + 2);
            const V2 a2m = v2_ld(c2 + 4 * m);
            const V2 b2m = v2_ld(c2 + 4 * m + 2);
            V2 om[8];
            butterfly4_v_norm(a0m, b0m, a1m, b1m, a2m, b2m, a3m, b3m,
                              cs2 + 2 * (m - 1), cs1 + 2 * (m - 1), cs1 + 2 * (2 * M - m - 1), om);
            store_butterfly4_v(vblk, M, m, om);
        }

        if (M >= 2) {
            const int i = M >> 1;
            const V2 a0i = v2_ld(c0 + 4 * i);
            const V2 b0i = v2_ld(c0 + 4 * i + 2);
            const V2 a1i = v2_ld(c1 + 4 * i);
            const V2 b1i = v2_ld(c1 + 4 * i + 2);
            const V2 a2i = v2_ld(c2 + 4 * i);
            const V2 b2i = v2_ld(c2 + 4 * i + 2);
            const V2 a3i = v2_ld(c3 + 4 * i);
            const V2 b3i = v2_ld(c3 + 4 * i + 2);
            V2 oi[8];
            butterfly4_v_norm(a0i, b0i, a1i, b1i, a2i, b2i, a3i, b3i,
                              cs2 + 2 * (i - 1), cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), oi);
            store_butterfly4_v(vblk, M, i, oi);
        }
    }

    void seed_fused_block_v_norm(const double* input, double* vw, int base, int count) const {
        seed_block_v(input, vw, base, count);
        int stage = 2;
        for (int s = 16; (s >> 1) <= count; s <<= 2, stage += 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int sub = base; sub < base + count; sub += (s >> 1)) {
                merge4_v_norm(vw + 4 * sub, M, cs2, cs1);
            }
        }
    }

    void compute_vwork_norm(const double* input, double* vw) const {
        const int q = half_ >> 1;
        const int chain_limit = radix2_terminal_ ? (n_ >> 1) : (n_ >> 2);
        const int s_target = should_cache_block<double>(n_) ? kBlockedStageSize : chain_limit;
        const int block_slots = s_target >> 1;
        const int nb = q / block_slots;
        const int nb_bits = ilog2_pow2(nb);
        for (int p = 0; p < nb; ++p) {
            const int base = bitrev_int(p, nb_bits) * block_slots;
            seed_fused_block_v_norm(input, vw, base, block_slots);
        }

        int s = s_target << 2;
        int stage = ilog2_pow2(s) - 2;
        for (; s <= chain_limit; s <<= 2, stage += 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int off = 0; off < n_; off += 2 * s) {
                merge4_v_norm(vw + off, M, cs2, cs1);
            }
        }
    }

    template <typename Complex>
    struct terminal_complex_writer {
        Complex* X;
        STXLIB_FFT_INLINE void store_bin(int k, double re, double im) const {
            X[k].re = re;
            X[k].im = im;
        }
        STXLIB_FFT_INLINE void store_pair(int k0, int k1, V2 re, V2 im) const {
            v2_st(&X[k0].re, v2_unplo(re, im));
            v2_st(&X[k1].re, v2_unphi(re, im));
        }
    };

    // -------- Normalized-basis SIMD terminals --------
    // Same mirrored-lane structure as terminal_v / terminal_radix2_v, but the
    // last merge runs the (c, s) rotation, so each leaf pair is already
    // (re, -im): the residue -> complex projection collapses to a sign flip,
    // and the high child needs no separate per-bin twiddle at all.
    static STXLIB_FFT_INLINE V2 v2_neg(V2 x) {
        return v2_sub(v2_set1(0.0), x);
    }

    template <typename Writer>
    void terminal_v_norm(const double* vw, const Writer& w) const {
        const int M = half_ >> 2;   // child node count = n/8
        const double* cA = vw;
        const double* cB = vw + 4 * M;

        const V2 dcA = v2_ld(cA);
        const V2 nyA = v2_ld(cA + 2);
        const V2 dcB = v2_ld(cB);
        const V2 nyB = v2_ld(cB + 2);
        const V2 sum = v2_add(dcA, dcB);   // {h0dc, h1dc}
        const V2 dif = v2_sub(dcA, dcB);   // {h0ny, h1ny}
        w.store_bin(0, v2_lane0(sum) + v2_lane1(sum), 0.0);
        w.store_bin(half_, v2_lane0(sum) - v2_lane1(sum), 0.0);
        w.store_bin(2 * M, v2_lane0(dif), -v2_lane1(dif));   // bin n/4
        {
            // node M: angle 2*pi*M/n = pi/4, rotation (cos_[M], sin_[M]).
            double la, lb, ha, hb;
            pair_reduce_cs(v2_lane0(nyA), v2_lane0(nyB), v2_lane1(nyA), v2_lane1(nyB),
                           cos_[M], sin_[M],
                           la, lb, ha, hb);
            w.store_bin(M, la, -lb);
            w.store_bin(half_ - M, ha, -hb);
        }

        const double* rec = fterm_cs_;
        for (int i = 1; i < M; ++i, rec += 6) {
            const V2 aA = v2_ld(cA + 4 * i);
            const V2 bA = v2_ld(cA + 4 * i + 2);
            const V2 aB = v2_ld(cB + 4 * i);
            const V2 bB = v2_ld(cB + 4 * i + 2);
            V2 la, lb, ha, hb;
            pair_reduce_v2_norm(aA, bA, aB, bB, v2_set1(rec[0]), v2_set1(rec[1]), la, lb, ha, hb);
            const V2 ua = v2_unplo(la, ha);   // {h0 low, h0 high}
            const V2 ub = v2_unplo(lb, hb);
            const V2 va = v2_unphi(la, ha);   // {h1 low, h1 high}
            const V2 vb = v2_unphi(lb, hb);
            V2 fa, fb, ga, gb;
            pair_reduce_v2_norm(ua, ub, va, vb, v2_ld(rec + 2), v2_ld(rec + 4), fa, fb, ga, gb);
            // fa/fb lanes = bins {i, n/4 - i}; ga/gb lanes = bins {n/2 - i, n/4 + i}.
            w.store_pair(i, 2 * M - i, fa, v2_neg(fb));
            w.store_pair(half_ - i, 2 * M + i, ga, v2_neg(gb));
        }
    }

    template <typename Writer>
    void terminal_radix2_v_norm(const double* vw, const Writer& w) const {
        const int q = half_ >> 1;

        const V2 dc = v2_ld(vw);
        const V2 ny = v2_ld(vw + 2);
        w.store_bin(0, v2_lane0(dc) + v2_lane1(dc), 0.0);
        w.store_bin(half_, v2_lane0(dc) - v2_lane1(dc), 0.0);
        w.store_bin(q, v2_lane0(ny), -v2_lane1(ny));

        int i = 1;
        for (; i + 1 < q; i += 2) {
            const V2 a0 = v2_ld(vw + 4 * i);
            const V2 b0 = v2_ld(vw + 4 * i + 2);
            const V2 a1 = v2_ld(vw + 4 * (i + 1));
            const V2 b1 = v2_ld(vw + 4 * (i + 1) + 2);
            const V2 ea = v2_unplo(a0, a1);
            const V2 oa = v2_unphi(a0, a1);
            const V2 eb = v2_unplo(b0, b1);
            const V2 ob = v2_unphi(b0, b1);
            // c/s for the two lane nodes {i, i+1} come from the per-bin
            // (cos_[k], sin_[k]) tables; the rotation emits both the
            // low child (bins i, i+1) and its mirror high child (bins n/2-i,
            // n/2-i-1) already as (re, -im).
            const V2 cv = v2_ld(cos_ + i);
            const V2 sv = v2_ld(sin_ + i);
            V2 la, lb, ha, hb;
            pair_reduce_v2_norm(ea, eb, oa, ob, cv, sv, la, lb, ha, hb);
            w.store_pair(i, i + 1, la, v2_neg(lb));
            w.store_pair(half_ - i, half_ - i - 1, ha, v2_neg(hb));
        }

        if (i < q) {
            const V2 a = v2_ld(vw + 4 * i);
            const V2 b = v2_ld(vw + 4 * i + 2);
            double la, lb, ha, hb;
            pair_reduce_cs(v2_lane0(a), v2_lane0(b), v2_lane1(a), v2_lane1(b),
                           cos_[i], sin_[i],
                           la, lb, ha, hb);
            const int hi = half_ - i;
            w.store_bin(i, la, -lb);
            w.store_bin(hi, ha, -hb);
        }
    }

    // ==================================================================
    // Wide f32 SIMD (4-lane V4F) -- block-paired mirrored walk.
    // Portable across the V4F backends (NEON / SSE), same guard as the rest
    // of the level-1 SIMD.
    //
    // Second lane axis (beyond the mirror pair {node b, b+N/4}) is the
    // cA/cB split the radix-4 terminal already combines: paired-slot s bundles
    // double-slot s (in cA=[0,N/8)) and double-slot s+N/8 (in cB=[N/8,N/4)).
    // The 4 v4f lanes are {cA.lo, cA.hi, cB.lo, cB.hi} = nodes
    // {s, s+N/4, s+N/8, s+3N/8}. cA and cB run identical twiddles through
    // every interior stage, so merge4_v4f / split4_v4f are byte-for-byte V4F
    // transcriptions of merge4_v_norm / split4_v_norm; only the seed
    // (contiguous paired V4F loads) and the terminals (split the v4f into
    // cA=low2 / cB=high2) are cA/cB-aware.
    //
    // Odd log2(n) runs the paired interior through N/4, then depairs the
    // final cross-lane merge4 plus radix-2 terminal into the V2-like f32
    // layout.
    // ==================================================================

    static STXLIB_FFT_INLINE void pair_reduce_v4f_norm(V4F ea, V4F eb, V4F oa, V4F ob,
                                                         V4F c, V4F s,
                                                         V4F& la, V4F& lb, V4F& ha, V4F& hb) {
        const V4F r = v4f_sub(v4f_mul(c, oa), v4f_mul(s, ob));   // c*oa - s*ob
        const V4F im = v4f_add(v4f_mul(s, oa), v4f_mul(c, ob));  // s*oa + c*ob
        la = v4f_add(ea, r);
        lb = v4f_add(eb, im);
        ha = v4f_sub(ea, r);
        hb = v4f_sub(im, eb);
    }

    static STXLIB_FFT_INLINE V4F v4f_set1cs(const double* p) { return v4f_set1(static_cast<float>(*p)); }

    static STXLIB_FFT_INLINE void pair_reduce_cs_f32(float ea, float eb, float oa, float ob, float c, float s,
                                                       float& la, float& lb, float& ha, float& hb) {
        const float r = c * oa - s * ob;
        const float im = s * oa + c * ob;
        la = ea + r;
        lb = eb + im;
        ha = ea - r;
        hb = im - eb;
    }

    static STXLIB_FFT_INLINE void pair_expand_cs_f32(float la, float lb, float ha, float hb, float c, float s,
                                                       float& ea, float& eb, float& oa, float& ob) {
        const float r = 0.5f * (la - ha);
        const float im = 0.5f * (lb + hb);
        ea = 0.5f * (la + ha);
        eb = 0.5f * (lb - hb);
        oa = c * r + s * im;
        ob = c * im - s * r;
    }

    // The templated scalar pair steps (T = float or double): twiddles read
    // from the double tables and cast to T at use, as BFFT's
    // bruun_dit_kernel.hpp pair_reduce_t and pair_expand_norm_t.
    template <typename T>
    static STXLIB_FFT_INLINE void pair_reduce_t(T ea, T eb, T oa, T ob, const double* cs, T& la, T& lb, T& ha,
                                                T& hb) {
        const T c = static_cast<T>(cs[0]);
        const T s = static_cast<T>(cs[1]);
        const T r = c * oa - s * ob;
        const T im = s * oa + c * ob;
        la = ea + r;
        lb = eb + im;
        ha = ea - r;
        hb = im - eb;
    }

    template <typename T>
    static STXLIB_FFT_INLINE void pair_expand_norm_t(T la, T lb, T ha, T hb, const double* cs, T& ea, T& eb, T& oa,
                                                     T& ob) {
        const T c = static_cast<T>(cs[0]);
        const T s = static_cast<T>(cs[1]);
        const T half = static_cast<T>(0.5);
        const T r = half * (la - ha);
        const T im = half * (lb + hb);
        ea = half * (la + ha);
        eb = half * (lb - hb);
        oa = c * r + s * im;
        ob = c * im - s * r;
    }

    STXLIB_FFT_INLINE void butterfly4_v4f(V4F a0, V4F b0, V4F a1, V4F b1,
                                            V4F a2, V4F b2, V4F a3, V4F b3,
                                            const double* cs2, const double* cs1, const double* cs1q,
                                            V4F* o) const {
        const V4F c2 = v4f_set1cs(cs2);
        const V4F s2 = v4f_set1cs(cs2 + 1);
        V4F h0la, h0lb, h0ha, h0hb, h1la, h1lb, h1ha, h1hb;
        pair_reduce_v4f_norm(a0, b0, a1, b1, c2, s2, h0la, h0lb, h0ha, h0hb);
        pair_reduce_v4f_norm(a2, b2, a3, b3, c2, s2, h1la, h1lb, h1ha, h1hb);
        pair_reduce_v4f_norm(h0la, h0lb, h1la, h1lb, v4f_set1cs(cs1), v4f_set1cs(cs1 + 1),
                             o[0], o[1], o[2], o[3]);
        pair_reduce_v4f_norm(h0ha, h0hb, h1ha, h1hb, v4f_set1cs(cs1q), v4f_set1cs(cs1q + 1),
                             o[4], o[5], o[6], o[7]);
    }

    static STXLIB_FFT_INLINE void store_butterfly4_v4f(float* vblk, int M, int i, const V4F* o) {
        v4f_st(vblk + 8 * i, o[0]);
        v4f_st(vblk + 8 * i + 4, o[1]);
        v4f_st(vblk + 8 * (4 * M - i), o[2]);
        v4f_st(vblk + 8 * (4 * M - i) + 4, o[3]);
        v4f_st(vblk + 8 * (2 * M - i), o[4]);
        v4f_st(vblk + 8 * (2 * M - i) + 4, o[5]);
        v4f_st(vblk + 8 * (2 * M + i), o[6]);
        v4f_st(vblk + 8 * (2 * M + i) + 4, o[7]);
    }

    void merge4_v4f(float* vblk, int M, const double* cs2, const double* cs1) const {
        float* c0 = vblk;
        float* c1 = vblk + 8 * M;
        float* c2 = vblk + 16 * M;
        float* c3 = vblk + 24 * M;

        const V4F dc0 = v4f_ld(c0);
        const V4F ny0 = v4f_ld(c0 + 4);
        const V4F dc1 = v4f_ld(c1);
        const V4F ny1 = v4f_ld(c1 + 4);
        const V4F dc2 = v4f_ld(c2);
        const V4F ny2 = v4f_ld(c2 + 4);
        const V4F dc3 = v4f_ld(c3);
        const V4F ny3 = v4f_ld(c3 + 4);
        V4F ma, mb, mha, mhb;
        pair_reduce_v4f_norm(ny0, ny1, ny2, ny3, v4f_set1cs(cs1 + 2 * (M - 1)), v4f_set1cs(cs1 + 2 * (M - 1) + 1),
                             ma, mb, mha, mhb);
        const V4F h0dc = v4f_add(dc0, dc1);
        const V4F h0ny = v4f_sub(dc0, dc1);
        const V4F h1dc = v4f_add(dc2, dc3);
        const V4F h1ny = v4f_sub(dc2, dc3);
        v4f_st(c0, v4f_add(h0dc, h1dc));
        v4f_st(c0 + 4, v4f_sub(h0dc, h1dc));
        v4f_st(c1, ma);
        v4f_st(c1 + 4, mb);
        v4f_st(c2, h0ny);
        v4f_st(c2 + 4, h1ny);
        v4f_st(c3, mha);
        v4f_st(c3 + 4, mhb);

        for (int i = 1; i < M - i; ++i) {
            const int m = M - i;
            const V4F a0i = v4f_ld(c0 + 8 * i);
            const V4F b0i = v4f_ld(c0 + 8 * i + 4);
            const V4F a1i = v4f_ld(c1 + 8 * i);
            const V4F b1i = v4f_ld(c1 + 8 * i + 4);
            const V4F a2i = v4f_ld(c2 + 8 * i);
            const V4F b2i = v4f_ld(c2 + 8 * i + 4);
            const V4F a3i = v4f_ld(c3 + 8 * i);
            const V4F b3i = v4f_ld(c3 + 8 * i + 4);
            const V4F a1m = v4f_ld(c1 + 8 * m);
            const V4F b1m = v4f_ld(c1 + 8 * m + 4);
            const V4F a3m = v4f_ld(c3 + 8 * m);
            const V4F b3m = v4f_ld(c3 + 8 * m + 4);
            V4F oi[8];
            butterfly4_v4f(a0i, b0i, a1i, b1i, a2i, b2i, a3i, b3i,
                           cs2 + 2 * (i - 1), cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), oi);
            store_butterfly4_v4f(vblk, M, i, oi);
            const V4F a0m = v4f_ld(c0 + 8 * m);
            const V4F b0m = v4f_ld(c0 + 8 * m + 4);
            const V4F a2m = v4f_ld(c2 + 8 * m);
            const V4F b2m = v4f_ld(c2 + 8 * m + 4);
            V4F om[8];
            butterfly4_v4f(a0m, b0m, a1m, b1m, a2m, b2m, a3m, b3m,
                           cs2 + 2 * (m - 1), cs1 + 2 * (m - 1), cs1 + 2 * (2 * M - m - 1), om);
            store_butterfly4_v4f(vblk, M, m, om);
        }

        if (M >= 2) {
            const int i = M >> 1;
            const V4F a0i = v4f_ld(c0 + 8 * i);
            const V4F b0i = v4f_ld(c0 + 8 * i + 4);
            const V4F a1i = v4f_ld(c1 + 8 * i);
            const V4F b1i = v4f_ld(c1 + 8 * i + 4);
            const V4F a2i = v4f_ld(c2 + 8 * i);
            const V4F b2i = v4f_ld(c2 + 8 * i + 4);
            const V4F a3i = v4f_ld(c3 + 8 * i);
            const V4F b3i = v4f_ld(c3 + 8 * i + 4);
            V4F oi[8];
            butterfly4_v4f(a0i, b0i, a1i, b1i, a2i, b2i, a3i, b3i,
                           cs2 + 2 * (i - 1), cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), oi);
            store_butterfly4_v4f(vblk, M, i, oi);
        }
    }

    // Seed a pair of paired-slots (s, s+1): gather the cA mirror pairs (slots
    // s, s+1) into v4f lanes {0,1} and the cB mirror pairs (slots s+eighth,
    // s+eighth+1) into lanes {2,3}, so the seed radix-2 runs on all four lanes
    // at once with contiguous loads.
    STXLIB_FFT_INLINE void seed_paired_pair_f32(const float* input, float* fvw, int s, int eighth) const {
        const int hq = half_ >> 1;
        const int jA = rev_[s];
        const int jB = rev_[s + eighth];
        (void)jB;
        const V4F a0 = v4f_ld(input + jA);
        const V4F c0 = v4f_ld(input + jA + half_);
        const V4F a1 = v4f_ld(input + jA + hq);
        const V4F c1 = v4f_ld(input + jA + hq + half_);
        const V4F e0 = v4f_add(a0, c0);
        const V4F e1 = v4f_add(a1, c1);
        v4f_st(fvw + 8 * s, v4f_add(e0, e1));            // paired-slot s   a-comp
        v4f_st(fvw + 8 * s + 4, v4f_sub(e0, e1));        // paired-slot s   b-comp
        v4f_st(fvw + 8 * (s + 1), v4f_sub(a0, c0));      // paired-slot s+1 a-comp
        v4f_st(fvw + 8 * (s + 1) + 4, v4f_sub(a1, c1));  // paired-slot s+1 b-comp
    }

    void seed_fused_block_v4f(const float* input, float* fvw, int base, int count) const {
        const int eighth = half_ >> 2;   // P = number of paired-slots = N/8
        for (int s = base; s < base + count; s += 2) {
            seed_paired_pair_f32(input, fvw, s, eighth);
        }
        int stage = 2;
        for (int s = 16; (s >> 1) <= count; s <<= 2, stage += 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int poff = base; poff < base + count; poff += (s >> 1)) {
                merge4_v4f(fvw + 8 * poff, M, cs2, cs1);
            }
        }
    }

    void compute_vwork_v4f(const float* input, float* fvw) const {
        const int eighth = half_ >> 2;   // P = number of paired-slots = N/8
        const int chain_limit = n_ >> 2;   // odd sizes finish the N/2 top merge separately
        const int s_target = should_cache_block<float>(n_) ? kBlockedStageSize : chain_limit;
        const int block_slots = s_target >> 1;
        const int nb = eighth / block_slots;
        const int nb_bits = ilog2_pow2(nb);
        for (int p = 0; p < nb; ++p) {
            const int base = bitrev_int(p, nb_bits) * block_slots;
            seed_fused_block_v4f(input, fvw, base, block_slots);
        }

        int s = s_target << 2;
        int stage = ilog2_pow2(s) - 2;
        for (; s <= chain_limit; s <<= 2, stage += 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int poff = 0; poff < eighth; poff += (s >> 1)) {
                merge4_v4f(fvw + 8 * poff, M, cs2, cs1);
            }
        }
    }

    STXLIB_FFT_INLINE void butterfly4_v_norm_f32_v4_inputs(V4F n0, V4F n1,
                                                             V4F n2, V4F n3,
                                                             const double* cs2,
                                                             const double* cs1, const double* cs1q,
                                                             V4F* out) const {
        const V4F a02 = v4f_catlo(n0, n2);
        const V4F b02 = v4f_cathi(n0, n2);
        const V4F a13 = v4f_catlo(n1, n3);
        const V4F b13 = v4f_cathi(n1, n3);

        V4F hla, hlb, hha, hhb;
        pair_reduce_v4f_norm(a02, b02, a13, b13,
                             v4f_set1cs(cs2), v4f_set1cs(cs2 + 1),
                             hla, hlb, hha, hhb);

        V4F la, lb, ha, hb;
        pair_reduce_v4f_norm(hla, hlb, v4f_cathi(hla, hla), v4f_cathi(hlb, hlb),
                             v4f_set1cs(cs1), v4f_set1cs(cs1 + 1), la, lb, ha, hb);
        out[0] = v4f_catlo(la, lb);
        out[1] = v4f_catlo(ha, hb);

        pair_reduce_v4f_norm(hha, hhb, v4f_cathi(hha, hha), v4f_cathi(hhb, hhb),
                             v4f_set1cs(cs1q), v4f_set1cs(cs1q + 1), la, lb, ha, hb);
        out[2] = v4f_catlo(la, lb);
        out[3] = v4f_catlo(ha, hb);
    }

    static STXLIB_FFT_INLINE void store_butterfly4_v_norm_f32_v4(float* vblk, int M, int i,
                                                                   const V4F* out) {
        v4f_st(vblk + 4 * i, out[0]);
        v4f_st(vblk + 4 * (4 * M - i), out[1]);
        v4f_st(vblk + 4 * (2 * M - i), out[2]);
        v4f_st(vblk + 4 * (2 * M + i), out[3]);
    }

    // Fused V4F->V2 depair + the extra radix-2-level merge for the odd forward:
    // reads the paired-V4F chain output fvw, produces the merge4 inputs in
    // registers (paired-slot i depairs to V2-slots {i, 2M+i}, paired-slot M+i
    // to {M+i, 3M+i} -- the four children of merge butterfly i), runs the merge,
    // and writes the V2 layout to vw. Collapses depair_v4f_to_v2_f32 +
    // merge4_v_norm_f32 into one pass. Source (fvw) and dest (vw) differ, so the
    // in-place palindrome merge4 needs is unnecessary -- a linear i sweep reads
    // every paired-slot once and writes every V2-slot once.
    void depair_merge4_v_norm_f32(const float* fvw, float* vw, int M,
                                  const double* cs2, const double* cs1) const {
        float* c0 = vw;
        float* c1 = vw + 4 * M;
        float* c2 = vw + 8 * M;
        float* c3 = vw + 12 * M;

        // i = 0 special (merge4_v_norm_f32's DC/Nyquist block).
        {
            const V4F av0 = v4f_ld(fvw);
            const V4F bv0 = v4f_ld(fvw + 4);
            const V4F avM = v4f_ld(fvw + 8 * M);
            const V4F bvM = v4f_ld(fvw + 8 * M + 4);
            float s0[4], sM[4], s2[4], s3[4];
            v4f_st(s0, v4f_catlo(av0, bv0));   // V2-slot 0
            v4f_st(s2, v4f_cathi(av0, bv0));   // V2-slot 2M
            v4f_st(sM, v4f_catlo(avM, bvM));   // V2-slot M
            v4f_st(s3, v4f_cathi(avM, bvM));   // V2-slot 3M
            float ma0, mb0, mha0, mhb0, ma1, mb1, mha1, mhb1;
            pair_reduce_cs_f32(s0[2], sM[2], s2[2], s3[2],
                               static_cast<float>(cs1[2 * (M - 1)]),
                               static_cast<float>(cs1[2 * (M - 1) + 1]),
                               ma0, mb0, mha0, mhb0);
            pair_reduce_cs_f32(s0[3], sM[3], s2[3], s3[3],
                               static_cast<float>(cs1[2 * (M - 1)]),
                               static_cast<float>(cs1[2 * (M - 1) + 1]),
                               ma1, mb1, mha1, mhb1);
            const float h0dc0 = s0[0] + sM[0];
            const float h0dc1 = s0[1] + sM[1];
            const float h0ny0 = s0[0] - sM[0];
            const float h0ny1 = s0[1] - sM[1];
            const float h1dc0 = s2[0] + s3[0];
            const float h1dc1 = s2[1] + s3[1];
            const float h1ny0 = s2[0] - s3[0];
            const float h1ny1 = s2[1] - s3[1];
            c0[0] = h0dc0 + h1dc0; c0[1] = h0dc1 + h1dc1; c0[2] = h0dc0 - h1dc0; c0[3] = h0dc1 - h1dc1;
            c1[0] = ma0; c1[1] = ma1; c1[2] = mb0; c1[3] = mb1;
            c2[0] = h0ny0; c2[1] = h0ny1; c2[2] = h1ny0; c2[3] = h1ny1;
            c3[0] = mha0; c3[1] = mha1; c3[2] = mhb0; c3[3] = mhb1;
        }

        for (int i = 1; i < M; ++i) {
            const V4F avi = v4f_ld(fvw + 8 * i);
            const V4F bvi = v4f_ld(fvw + 8 * i + 4);
            const V4F avM = v4f_ld(fvw + 8 * (M + i));
            const V4F bvM = v4f_ld(fvw + 8 * (M + i) + 4);
            V4F out[4];
            butterfly4_v_norm_f32_v4_inputs(v4f_catlo(avi, bvi),   // c0 = slot i
                                            v4f_catlo(avM, bvM),   // c1 = slot M+i
                                            v4f_cathi(avi, bvi),   // c2 = slot 2M+i
                                            v4f_cathi(avM, bvM),   // c3 = slot 3M+i
                                            cs2 + 2 * (i - 1), cs1 + 2 * (i - 1),
                                            cs1 + 2 * (2 * M - i - 1), out);
            store_butterfly4_v_norm_f32_v4(vw, M, i, out);
        }
    }

    void terminal_radix2_v_norm_f32(const float* vw, Complex32* X) const {
        const int q = half_ >> 1;
        X[0].re = vw[0] + vw[1];
        X[0].im = 0.0f;
        X[half_].re = vw[0] - vw[1];
        X[half_].im = 0.0f;
        X[q].re = vw[2];
        X[q].im = -vw[3];

        const V4F neg = v4f_set1(-1.0f);
        int i = 1;
        for (; i + 1 < q; i += 2) {
            const V4F n0 = v4f_ld(vw + 4 * i);
            const V4F n1 = v4f_ld(vw + 4 * (i + 1));
            const V4F ea_oa = v4f_ziplo(n0, n1);
            const V4F eb_ob = v4f_ziphi(n0, n1);
            const V4F ea = v4f_catlo(ea_oa, ea_oa);
            const V4F oa = v4f_cathi(ea_oa, ea_oa);
            const V4F eb = v4f_catlo(eb_ob, eb_ob);
            const V4F ob = v4f_cathi(eb_ob, eb_ob);
            const V4F cv = v4f_set4(static_cast<float>(cos_[i]),
                                          static_cast<float>(cos_[i + 1]),
                                          static_cast<float>(cos_[i]),
                                          static_cast<float>(cos_[i + 1]));
            const V4F sv = v4f_set4(static_cast<float>(sin_[i]),
                                          static_cast<float>(sin_[i + 1]),
                                          static_cast<float>(sin_[i]),
                                          static_cast<float>(sin_[i + 1]));
            V4F la, lb, ha, hb;
            pair_reduce_v4f_norm(ea, eb, oa, ob, cv, sv, la, lb, ha, hb);
            v4f_st(&X[i].re, v4f_ziplo(la, v4f_mul(lb, neg)));

            const int hi = half_ - i;
            const V4F high_pair = v4f_ziplo(ha, v4f_mul(hb, neg));
            v4f_st(&X[hi - 1].re, v4f_catlo(v4f_cathi(high_pair, high_pair), high_pair));
        }

        for (; i < q; ++i) {
            const int hi = half_ - i;
            float la, lb, ha, hb;
            pair_reduce_cs_f32(vw[4 * i], vw[4 * i + 2],
                               vw[4 * i + 1], vw[4 * i + 3],
                               static_cast<float>(cos_[i]),
                               static_cast<float>(sin_[i]),
                               la, lb, ha, hb);
            X[i].re = la;
            X[i].im = -lb;
            X[hi].re = ha;
            X[hi].im = -hb;
        }
    }

    // Terminal for the wide path: read each paired-slot, split the v4f into
    // Terminal: the single cA/cB lane crossing, in 128-bit V4F. Each paired
    // slot's a-comp v4f is {aA.lo, aA.hi, aB.lo, aB.hi} (mirror pair per half);
    // the radix-4 terminal combines cA with cB. CATHI aligns aB into the low
    // lanes for level A, ZIPLO does the mirror crossing (UNPLO/UNPHI), CATHI
    // aligns va for level B -- the same shuffle abstraction the DIF uses.
    void terminal_v4f_f32(const float* fvw, Complex32* X) const {
        const int M = half_ >> 2;   // = eighth = P
        const double* cs1 = cs_ + 2 * twiddle_offset_[twiddle_stage_count_ - 1];

        // paired-slot 0 (DC/Nyquist/midpoint specials): a-comp {cA.lo,cA.hi,
        // cB.lo,cB.hi} at fvw[0..3], b-comp at fvw[4..7].
        const float sum0 = fvw[0] + fvw[2];
        const float sum1 = fvw[1] + fvw[3];
        X[0].re = sum0 + sum1;      X[0].im = 0.0f;
        X[half_].re = sum0 - sum1;  X[half_].im = 0.0f;
        X[2 * M].re = fvw[0] - fvw[2];  X[2 * M].im = -(fvw[1] - fvw[3]);
        {
            float la, lb, ha, hb;
            pair_reduce_t<float>(fvw[4], fvw[6], fvw[5], fvw[7], cs1 + 2 * (M - 1), la, lb, ha, hb);
            X[M].re = la;         X[M].im = -lb;
            X[half_ - M].re = ha; X[half_ - M].im = -hb;
        }

        const double* rec = fterm_cs_;
        for (int i = 1; i < M; ++i, rec += 6) {
            const V4F av = v4f_ld(fvw + 8 * i);
            const V4F bv = v4f_ld(fvw + 8 * i + 4);
            // level A: ea=aA (av lanes 0,1), oa=aB (CATHI -> lanes 0,1)
            V4F la, lb, ha, hb;
            pair_reduce_v4f_norm(av, bv, v4f_cathi(av, av), v4f_cathi(bv, bv),
                                 v4f_set1cs(rec), v4f_set1cs(rec + 1), la, lb, ha, hb);
            // mirror crossing: ZIPLO(la,ha) = {ua | va} in lanes {0,1}|{2,3}
            const V4F uva = v4f_ziplo(la, ha);
            const V4F uvb = v4f_ziplo(lb, hb);
            // level B: ea=ua (lanes 0,1), oa=va (CATHI), per-lane c/s {rec2,rec3}
            const V4F cB = v4f_set4(static_cast<float>(rec[2]), static_cast<float>(rec[3]),
                                          static_cast<float>(rec[2]), static_cast<float>(rec[3]));
            const V4F sB = v4f_set4(static_cast<float>(rec[4]), static_cast<float>(rec[5]),
                                          static_cast<float>(rec[4]), static_cast<float>(rec[5]));
            V4F fa, fb, ga, gb;
            pair_reduce_v4f_norm(uva, uvb, v4f_cathi(uva, uva), v4f_cathi(uvb, uvb), cB, sB,
                                 fa, fb, ga, gb);
            float tfa[4], tfb[4], tga[4], tgb[4];
            v4f_st(tfa, fa); v4f_st(tfb, fb); v4f_st(tga, ga); v4f_st(tgb, gb);
            X[i].re = tfa[0];         X[i].im = -tfb[0];
            X[2 * M - i].re = tfa[1]; X[2 * M - i].im = -tfb[1];
            X[half_ - i].re = tga[0]; X[half_ - i].im = -tgb[0];
            X[2 * M + i].re = tga[1]; X[2 * M + i].im = -tgb[1];
        }
    }

    bool f32_wide_ok() const { return n_ >= 16; }

    void forward_simd_f32_wide(const float* input, Complex32* output, float* work) const {
        if (radix2_terminal_) {
            // Odd log2(n): the shared V4F chain lands in the output buffer used
            // as V4F scratch, the depair writes the V2 layout into work, the
            // extra radix-2-level merge runs in place there, and the terminal
            // reads work and writes the bins back into output. Swapping the
            // V4F/V2 buffer roles this way removes the full-array copy the old
            // ordering needed (it left the merged V2 data in output, which the
            // terminal must also write).
            float* v4f_scratch = reinterpret_cast<float*>(output);
            compute_vwork_v4f(input, v4f_scratch);
            const int s = n_ >> 1;
            const int stage = ilog2_pow2(s) - 2;
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            depair_merge4_v_norm_f32(v4f_scratch, work, s >> 3, cs2, cs1);
            terminal_radix2_v_norm_f32(work, output);
            return;
        }
        compute_vwork_v4f(input, work);
        terminal_v4f_f32(work, output);
    }

    // ------------------------------------------------------------------
    // Mirrored-lane inverse: the same walk backwards. Standard bins enter
    // through the terminal split (the only lane crossing, fused with
    // deprojection), every split below is lane-vertical, and the seed
    // scatter leaves as contiguous V2 stores at rev(b).
    // ------------------------------------------------------------------

    static STXLIB_FFT_INLINE void store_inv_butterfly4_v(double* vblk, int M, int i, const V2* o) {
        v2_st(vblk + 4 * i, o[0]);
        v2_st(vblk + 4 * i + 2, o[1]);
        v2_st(vblk + 4 * (M + i), o[2]);
        v2_st(vblk + 4 * (M + i) + 2, o[3]);
        v2_st(vblk + 4 * (2 * M + i), o[4]);
        v2_st(vblk + 4 * (2 * M + i) + 2, o[5]);
        v2_st(vblk + 4 * (3 * M + i), o[6]);
        v2_st(vblk + 4 * (3 * M + i) + 2, o[7]);
    }

    // Inverse seed: read one completed mirrored-lane seed block linearly from
    // vwork. Mirror lanes are adjacent time samples, so each destination is a
    // contiguous V2 store at rev(b); block order carries the permutation cost.
    void seed_scatter_block_v(const double* vw, double* output, int base, int count) const {
        const V2 hv = v2_set1(0.5);
        const int hq = half_ >> 1;
        for (int b = base; b < base + count; b += 2) {
            const int j0 = rev_[b];
            const int j1 = j0 + hq;
            const double* src = vw + 4 * b;
            const V2 va = v2_ld(src);
            const V2 vb = v2_ld(src + 2);
            const V2 vc = v2_ld(src + 4);
            const V2 vd = v2_ld(src + 6);
            const V2 e0 = v2_mul(v2_add(va, vb), hv);
            const V2 e1 = v2_mul(v2_sub(va, vb), hv);
            v2_st(output + j0, v2_mul(v2_add(e0, vc), hv));
            v2_st(output + j0 + half_, v2_mul(v2_sub(e0, vc), hv));
            v2_st(output + j1, v2_mul(v2_add(e1, vd), hv));
            v2_st(output + j1 + half_, v2_mul(v2_sub(e1, vd), hv));
        }
    }

    void seed_scatter_row_tiles_v(const double* vw, double* output,
                                  int block_slots, int nb, int nb_bits) const {
        const V2 hv = v2_set1(0.5);
        const int hq = half_ >> 1;
        const int pairs = block_slots >> 1;
        const int row_bits = ilog2_pow2(pairs);

        for (int r = 0; r < pairs; ++r) {
            const int row = bitrev_int(r, row_bits);
            const int row_base = 2 * row * nb;
            double* row0 = output + row_base;
            double* row1 = output + row_base + half_;
            double* row2 = output + row_base + hq;
            double* row3 = output + row_base + hq + half_;
            for (int h = 0; h < nb; ++h) {
                const int col = bitrev_int(h, nb_bits);
                const int b = h * block_slots + 2 * r;
                const double* src = vw + 4 * b;
                const V2 va = v2_ld(src);
                const V2 vb = v2_ld(src + 2);
                const V2 vc = v2_ld(src + 4);
                const V2 vd = v2_ld(src + 6);
                const V2 e0 = v2_mul(v2_add(va, vb), hv);
                const V2 e1 = v2_mul(v2_sub(va, vb), hv);
                v2_st(row0 + 2 * col, v2_mul(v2_add(e0, vc), hv));
                v2_st(row1 + 2 * col, v2_mul(v2_sub(e0, vc), hv));
                v2_st(row2 + 2 * col, v2_mul(v2_add(e1, vd), hv));
                v2_st(row3 + 2 * col, v2_mul(v2_sub(e1, vd), hv));
            }
        }
    }

    // -------- Normalized-basis SIMD inverse (exact inverse of the forward) --
    // pair_expand_v2_norm undoes pair_reduce_v2_norm: the same (c, s) rotation
    // run backwards, so the whole inverse composes with the forward to exactly
    // 1/N with no separate normalization and no accuracy loss. Deprojection is
    // now trivial - the forward stored (re, -im) = (a, b), so the inverse reads
    // a = re, b = -im with no cos/sin at all.

    static STXLIB_FFT_INLINE void pair_expand_v2_norm(V2 la, V2 lb, V2 ha, V2 hb,
                                                        V2 c, V2 s, V2 hv,
                                                        V2& ea, V2& eb, V2& oa, V2& ob) {
        const V2 r = v2_mul(v2_sub(la, ha), hv);   // 0.5*(la - ha)
        const V2 im = v2_mul(v2_add(lb, hb), hv);  // 0.5*(lb + hb)
        ea = v2_mul(v2_add(la, ha), hv);
        eb = v2_mul(v2_sub(lb, hb), hv);
        oa = v2_add(v2_mul(c, r), v2_mul(s, im));   // c*r + s*im
        ob = v2_sub(v2_mul(c, im), v2_mul(s, r));   // c*im - s*r
    }

    STXLIB_FFT_INLINE void inv_butterfly4_v_norm(V2 nia, V2 nib, V2 nha, V2 nhb,
                                                   V2 nqla, V2 nqlb, V2 nqha, V2 nqhb,
                                                   const double* cs1i, const double* cs1q, const double* cs2i,
                                                   V2 hv, V2* o) const {
        V2 h0la, h0lb, h1la, h1lb, h0ha, h0hb, h1ha, h1hb;
        pair_expand_v2_norm(nia, nib, nha, nhb, v2_set1(cs1i[0]), v2_set1(cs1i[1]), hv,
                            h0la, h0lb, h1la, h1lb);
        pair_expand_v2_norm(nqla, nqlb, nqha, nqhb, v2_set1(cs1q[0]), v2_set1(cs1q[1]), hv,
                            h0ha, h0hb, h1ha, h1hb);
        const V2 c2 = v2_set1(cs2i[0]);
        const V2 s2 = v2_set1(cs2i[1]);
        pair_expand_v2_norm(h0la, h0lb, h0ha, h0hb, c2, s2, hv, o[0], o[1], o[2], o[3]);
        pair_expand_v2_norm(h1la, h1lb, h1ha, h1hb, c2, s2, hv, o[4], o[5], o[6], o[7]);
    }

    void split4_v_norm(double* vblk, int M, const double* cs2, const double* cs1) const {
        const V2 hv = v2_set1(0.5);
        double* c0 = vblk;
        double* c1 = vblk + 4 * M;
        double* c2 = vblk + 8 * M;
        double* c3 = vblk + 12 * M;

        const V2 n0a = v2_ld(c0);
        const V2 n0b = v2_ld(c0 + 2);
        const V2 nMa = v2_ld(c1);
        const V2 nMb = v2_ld(c1 + 2);
        const V2 n2a = v2_ld(c2);
        const V2 n2b = v2_ld(c2 + 2);
        const V2 n3a = v2_ld(c3);
        const V2 n3b = v2_ld(c3 + 2);
        const V2 h0dc = v2_mul(v2_add(n0a, n0b), hv);
        const V2 h1dc = v2_mul(v2_sub(n0a, n0b), hv);
        V2 ny0, ny1, ny2, ny3;
        pair_expand_v2_norm(nMa, nMb, n3a, n3b,
                            v2_set1(cs1[2 * (M - 1)]), v2_set1(cs1[2 * (M - 1) + 1]), hv,
                            ny0, ny1, ny2, ny3);
        v2_st(c0, v2_mul(v2_add(h0dc, n2a), hv));
        v2_st(c0 + 2, ny0);
        v2_st(c1, v2_mul(v2_sub(h0dc, n2a), hv));
        v2_st(c1 + 2, ny1);
        v2_st(c2, v2_mul(v2_add(h1dc, n2b), hv));
        v2_st(c2 + 2, ny2);
        v2_st(c3, v2_mul(v2_sub(h1dc, n2b), hv));
        v2_st(c3 + 2, ny3);

        for (int i = 1; i < M - i; ++i) {
            const int m = M - i;
            const V2 ia = v2_ld(vblk + 4 * i);
            const V2 ib = v2_ld(vblk + 4 * i + 2);
            const V2 iha = v2_ld(vblk + 4 * (4 * M - i));
            const V2 ihb = v2_ld(vblk + 4 * (4 * M - i) + 2);
            const V2 iqla = v2_ld(vblk + 4 * (2 * M - i));
            const V2 iqlb = v2_ld(vblk + 4 * (2 * M - i) + 2);
            const V2 iqha = v2_ld(vblk + 4 * (2 * M + i));
            const V2 iqhb = v2_ld(vblk + 4 * (2 * M + i) + 2);
            const V2 mha = v2_ld(vblk + 4 * (4 * M - m));
            const V2 mhb = v2_ld(vblk + 4 * (4 * M - m) + 2);
            const V2 mqla = v2_ld(vblk + 4 * (2 * M - m));
            const V2 mqlb = v2_ld(vblk + 4 * (2 * M - m) + 2);
            V2 oi[8];
            inv_butterfly4_v_norm(ia, ib, iha, ihb, iqla, iqlb, iqha, iqhb,
                                  cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), cs2 + 2 * (i - 1), hv, oi);
            store_inv_butterfly4_v(vblk, M, i, oi);
            const V2 ma = v2_ld(vblk + 4 * m);
            const V2 mb = v2_ld(vblk + 4 * m + 2);
            const V2 mqha = v2_ld(vblk + 4 * (2 * M + m));
            const V2 mqhb = v2_ld(vblk + 4 * (2 * M + m) + 2);
            V2 om[8];
            inv_butterfly4_v_norm(ma, mb, mha, mhb, mqla, mqlb, mqha, mqhb,
                                  cs1 + 2 * (m - 1), cs1 + 2 * (2 * M - m - 1), cs2 + 2 * (m - 1), hv, om);
            store_inv_butterfly4_v(vblk, M, m, om);
        }

        if (M >= 2) {
            const int i = M >> 1;
            const V2 ia = v2_ld(vblk + 4 * i);
            const V2 ib = v2_ld(vblk + 4 * i + 2);
            const V2 iha = v2_ld(vblk + 4 * (4 * M - i));
            const V2 ihb = v2_ld(vblk + 4 * (4 * M - i) + 2);
            const V2 iqla = v2_ld(vblk + 4 * (2 * M - i));
            const V2 iqlb = v2_ld(vblk + 4 * (2 * M - i) + 2);
            const V2 iqha = v2_ld(vblk + 4 * (2 * M + i));
            const V2 iqhb = v2_ld(vblk + 4 * (2 * M + i) + 2);
            V2 oi[8];
            inv_butterfly4_v_norm(ia, ib, iha, ihb, iqla, iqlb, iqha, iqhb,
                                  cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), cs2 + 2 * (i - 1), hv, oi);
            store_inv_butterfly4_v(vblk, M, i, oi);
        }
    }

    void compute_vwork_inv_norm(double* vw, double* output) const {
        const int q = half_ >> 1;
        const int chain_limit = radix2_terminal_ ? (n_ >> 1) : (n_ >> 2);
        const int s_target = should_cache_block<double>(n_) ? kBlockedStageSize : chain_limit;
        const int block_slots = s_target >> 1;
        const int nb = q / block_slots;
        const int nb_bits = ilog2_pow2(nb);

        int s = chain_limit;
        int stage = ilog2_pow2(s) - 2;
        for (; s >= (s_target << 2); s >>= 2, stage -= 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int off = 0; off < n_; off += 2 * s) {
                split4_v_norm(vw + off, M, cs2, cs1);
            }
        }

        if (nb >= kRowTileBlockCount) {
            for (int p = 0; p < nb; ++p) {
                const int base = p * block_slots;
                int ss = s_target;
                int st = ilog2_pow2(ss) - 2;
                for (; ss >= 16; ss >>= 2, st -= 2) {
                    const double* cs2 = cs_ + 2 * twiddle_offset_[st - 1];
                    const double* cs1 = cs_ + 2 * twiddle_offset_[st];
                    const int M = ss >> 3;
                    for (int sub = base; sub < base + block_slots; sub += (ss >> 1)) {
                        split4_v_norm(vw + 4 * sub, M, cs2, cs1);
                    }
                }
            }
            seed_scatter_row_tiles_v(vw, output, block_slots, nb, nb_bits);
            return;
        }

        for (int p = 0; p < nb; ++p) {
            const int base = bitrev_int(p, nb_bits) * block_slots;
            int ss = s_target;
            int st = ilog2_pow2(ss) - 2;
            for (; ss >= 16; ss >>= 2, st -= 2) {
                const double* cs2 = cs_ + 2 * twiddle_offset_[st - 1];
                const double* cs1 = cs_ + 2 * twiddle_offset_[st];
                const int M = ss >> 3;
                for (int sub = base; sub < base + block_slots; sub += (ss >> 1)) {
                    split4_v_norm(vw + 4 * sub, M, cs2, cs1);
                }
            }
            seed_scatter_block_v(vw, output, base, block_slots);
        }
    }

    // Inverse terminal, normalized basis. Standard bins deproject to (a, b) =
    // (re, -im) with no cos/sin, then two normalized inverse-rotation levels
    // and the single lane crossing reproduce the mirrored-lane child slots.
    template <typename Complex>
    void terminal_inv_v_norm(const Complex* X, double* vw) const {
        const int M = half_ >> 2;   // child node count = n/8
        double* cA = vw;
        double* cB = vw + 4 * M;
        const double* cs1 = cs_ + 2 * twiddle_offset_[twiddle_stage_count_ - 1];
        const V2 hv = v2_set1(0.5);

        const double h0dc = 0.5 * (X[0].re + X[half_].re);
        const double h1dc = 0.5 * (X[0].re - X[half_].re);
        const double h0ny = X[2 * M].re;
        const double h1ny = -X[2 * M].im;
        {
            const double la = X[M].re;
            const double lb = -X[M].im;
            const double ha = X[half_ - M].re;
            const double hb = -X[half_ - M].im;
            double ny0, ny1, ny2, ny3;
            pair_expand_cs(la, lb, ha, hb, cs1[2 * (M - 1)], cs1[2 * (M - 1) + 1], ny0, ny1, ny2, ny3);
            v2_st(cA, v2_setlh(0.5 * (h0dc + h0ny), 0.5 * (h1dc + h1ny)));   // {C0dc, C2dc}
            v2_st(cA + 2, v2_setlh(ny0, ny2));                               // {C0ny, C2ny}
            v2_st(cB, v2_setlh(0.5 * (h0dc - h0ny), 0.5 * (h1dc - h1ny)));   // {C1dc, C3dc}
            v2_st(cB + 2, v2_setlh(ny1, ny3));                               // {C1ny, C3ny}
        }

        const double* rec = fterm_cs_;
        for (int i = 1; i < M; ++i, rec += 6) {
            const V2 xl0 = v2_ld(&X[i].re);
            const V2 xl1 = v2_ld(&X[2 * M - i].re);
            const V2 xh0 = v2_ld(&X[half_ - i].re);
            const V2 xh1 = v2_ld(&X[2 * M + i].re);
            const V2 re_l = v2_unplo(xl0, xl1);
            const V2 im_l = v2_unphi(xl0, xl1);
            const V2 re_h = v2_unplo(xh0, xh1);
            const V2 im_h = v2_unphi(xh0, xh1);
            // deproject: (a, b) = (re, -im) for bins {i, n/4-i} and {n/2-i, n/4+i}
            const V2 fa = re_l;
            const V2 fb = v2_neg(im_l);
            const V2 ga = re_h;
            const V2 gb = v2_neg(im_h);
            V2 ua, ub, va, vb;
            pair_expand_v2_norm(fa, fb, ga, gb, v2_ld(rec + 2), v2_ld(rec + 4), hv, ua, ub, va, vb);
            const V2 la = v2_unplo(ua, va);   // {h0 low, h1 low}
            const V2 lb = v2_unplo(ub, vb);
            const V2 ha = v2_unphi(ua, va);   // {h0 high, h1 high}
            const V2 hb = v2_unphi(ub, vb);
            V2 aA, bA, aB, bB;
            pair_expand_v2_norm(la, lb, ha, hb, v2_set1(rec[0]), v2_set1(rec[1]), hv, aA, bA, aB, bB);
            v2_st(cA + 4 * i, aA);
            v2_st(cA + 4 * i + 2, bA);
            v2_st(cB + 4 * i, aB);
            v2_st(cB + 4 * i + 2, bB);
        }
    }

    template <typename Complex>
    void terminal_radix2_inv_v_norm(const Complex* X, double* vw) const {
        const int q = half_ >> 1;
        const double* cs = cs_ + 2 * twiddle_offset_[twiddle_stage_count_ - 1];
        const V2 hv = v2_set1(0.5);

        v2_st(vw, v2_setlh(0.5 * (X[0].re + X[half_].re),
                           0.5 * (X[0].re - X[half_].re)));
        v2_st(vw + 2, v2_setlh(X[q].re, -X[q].im));

        int i = 1;
        for (; i + 1 < q; i += 2) {
            const V2 xl0 = v2_ld(&X[i].re);
            const V2 xl1 = v2_ld(&X[i + 1].re);
            const V2 re_l = v2_unplo(xl0, xl1);
            const V2 im_l = v2_unphi(xl0, xl1);

            const int hi0 = half_ - i;
            const int hi1 = half_ - i - 1;
            const V2 xh0 = v2_ld(&X[hi0].re);
            const V2 xh1 = v2_ld(&X[hi1].re);
            const V2 re_h = v2_unplo(xh0, xh1);
            const V2 im_h = v2_unphi(xh0, xh1);

            // low pair (bins i, i+1) and its mirror high pair both deproject
            // to (re, -im); one rotation (c, s of nodes i, i+1) inverts them.
            const V2 la = re_l;
            const V2 lb = v2_neg(im_l);
            const V2 ha = re_h;
            const V2 hb = v2_neg(im_h);
            const V2 cv = v2_ld(cos_ + i);
            const V2 sv = v2_ld(sin_ + i);

            V2 ea, eb, oa, ob;
            pair_expand_v2_norm(la, lb, ha, hb, cv, sv, hv, ea, eb, oa, ob);

            v2_st(vw + 4 * i, v2_unplo(ea, oa));
            v2_st(vw + 4 * i + 2, v2_unplo(eb, ob));
            v2_st(vw + 4 * (i + 1), v2_unphi(ea, oa));
            v2_st(vw + 4 * (i + 1) + 2, v2_unphi(eb, ob));
        }

        if (i < q) {
            const int hi = half_ - i;
            const double la = X[i].re;
            const double lb = -X[i].im;
            const double ha = X[hi].re;
            const double hb = -X[hi].im;
            double ea, eb, oa, ob;
            pair_expand_cs(la, lb, ha, hb, cs[2 * (i - 1)], cs[2 * (i - 1) + 1], ea, eb, oa, ob);
            v2_st(vw + 4 * i, v2_setlh(ea, oa));
            v2_st(vw + 4 * i + 2, v2_setlh(eb, ob));
        }
    }

    // ---- Wide f32 NEON inverse (exact reverse of the wide forward) ----
    static STXLIB_FFT_INLINE void pair_expand_v4f_norm(V4F la, V4F lb, V4F ha, V4F hb,
                                                         V4F c, V4F s,
                                                         V4F& ea, V4F& eb, V4F& oa, V4F& ob) {
        const V4F hv = v4f_set1(0.5f);
        const V4F r = v4f_mul(v4f_sub(la, ha), hv);
        const V4F im = v4f_mul(v4f_add(lb, hb), hv);
        ea = v4f_mul(v4f_add(la, ha), hv);
        eb = v4f_mul(v4f_sub(lb, hb), hv);
        oa = v4f_add(v4f_mul(c, r), v4f_mul(s, im));   // c*r + s*im
        ob = v4f_sub(v4f_mul(c, im), v4f_mul(s, r));   // c*im - s*r
    }

    STXLIB_FFT_INLINE void inv_butterfly4_v4f(V4F nia, V4F nib, V4F nha, V4F nhb,
                                                V4F nqla, V4F nqlb, V4F nqha, V4F nqhb,
                                                const double* cs1i, const double* cs1q, const double* cs2i,
                                                V4F* o) const {
        V4F h0la, h0lb, h1la, h1lb, h0ha, h0hb, h1ha, h1hb;
        pair_expand_v4f_norm(nia, nib, nha, nhb, v4f_set1cs(cs1i), v4f_set1cs(cs1i + 1), h0la, h0lb, h1la, h1lb);
        pair_expand_v4f_norm(nqla, nqlb, nqha, nqhb, v4f_set1cs(cs1q), v4f_set1cs(cs1q + 1), h0ha, h0hb, h1ha, h1hb);
        const V4F c2 = v4f_set1cs(cs2i);
        const V4F s2 = v4f_set1cs(cs2i + 1);
        pair_expand_v4f_norm(h0la, h0lb, h0ha, h0hb, c2, s2, o[0], o[1], o[2], o[3]);
        pair_expand_v4f_norm(h1la, h1lb, h1ha, h1hb, c2, s2, o[4], o[5], o[6], o[7]);
    }

    static STXLIB_FFT_INLINE void store_inv_butterfly4_v4f(float* vblk, int M, int i, const V4F* o) {
        v4f_st(vblk + 8 * i, o[0]);
        v4f_st(vblk + 8 * i + 4, o[1]);
        v4f_st(vblk + 8 * (M + i), o[2]);
        v4f_st(vblk + 8 * (M + i) + 4, o[3]);
        v4f_st(vblk + 8 * (2 * M + i), o[4]);
        v4f_st(vblk + 8 * (2 * M + i) + 4, o[5]);
        v4f_st(vblk + 8 * (3 * M + i), o[6]);
        v4f_st(vblk + 8 * (3 * M + i) + 4, o[7]);
    }

    void split4_v4f(float* vblk, int M, const double* cs2, const double* cs1) const {
        const V4F hv = v4f_set1(0.5f);
        float* c0 = vblk;
        float* c1 = vblk + 8 * M;
        float* c2 = vblk + 16 * M;
        float* c3 = vblk + 24 * M;

        const V4F n0a = v4f_ld(c0);
        const V4F n0b = v4f_ld(c0 + 4);
        const V4F nMa = v4f_ld(c1);
        const V4F nMb = v4f_ld(c1 + 4);
        const V4F n2a = v4f_ld(c2);
        const V4F n2b = v4f_ld(c2 + 4);
        const V4F n3a = v4f_ld(c3);
        const V4F n3b = v4f_ld(c3 + 4);
        const V4F h0dc = v4f_mul(v4f_add(n0a, n0b), hv);
        const V4F h1dc = v4f_mul(v4f_sub(n0a, n0b), hv);
        V4F ny0, ny1, ny2, ny3;
        pair_expand_v4f_norm(nMa, nMb, n3a, n3b, v4f_set1cs(cs1 + 2 * (M - 1)), v4f_set1cs(cs1 + 2 * (M - 1) + 1),
                             ny0, ny1, ny2, ny3);
        v4f_st(c0, v4f_mul(v4f_add(h0dc, n2a), hv));
        v4f_st(c0 + 4, ny0);
        v4f_st(c1, v4f_mul(v4f_sub(h0dc, n2a), hv));
        v4f_st(c1 + 4, ny1);
        v4f_st(c2, v4f_mul(v4f_add(h1dc, n2b), hv));
        v4f_st(c2 + 4, ny2);
        v4f_st(c3, v4f_mul(v4f_sub(h1dc, n2b), hv));
        v4f_st(c3 + 4, ny3);

        for (int i = 1; i < M - i; ++i) {
            const int m = M - i;
            const V4F ia = v4f_ld(vblk + 8 * i);
            const V4F ib = v4f_ld(vblk + 8 * i + 4);
            const V4F iha = v4f_ld(vblk + 8 * (4 * M - i));
            const V4F ihb = v4f_ld(vblk + 8 * (4 * M - i) + 4);
            const V4F iqla = v4f_ld(vblk + 8 * (2 * M - i));
            const V4F iqlb = v4f_ld(vblk + 8 * (2 * M - i) + 4);
            const V4F iqha = v4f_ld(vblk + 8 * (2 * M + i));
            const V4F iqhb = v4f_ld(vblk + 8 * (2 * M + i) + 4);
            const V4F mha = v4f_ld(vblk + 8 * (4 * M - m));
            const V4F mhb = v4f_ld(vblk + 8 * (4 * M - m) + 4);
            const V4F mqla = v4f_ld(vblk + 8 * (2 * M - m));
            const V4F mqlb = v4f_ld(vblk + 8 * (2 * M - m) + 4);
            V4F oi[8];
            inv_butterfly4_v4f(ia, ib, iha, ihb, iqla, iqlb, iqha, iqhb,
                               cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), cs2 + 2 * (i - 1), oi);
            store_inv_butterfly4_v4f(vblk, M, i, oi);
            const V4F ma = v4f_ld(vblk + 8 * m);
            const V4F mb = v4f_ld(vblk + 8 * m + 4);
            const V4F mqha = v4f_ld(vblk + 8 * (2 * M + m));
            const V4F mqhb = v4f_ld(vblk + 8 * (2 * M + m) + 4);
            V4F om[8];
            inv_butterfly4_v4f(ma, mb, mha, mhb, mqla, mqlb, mqha, mqhb,
                               cs1 + 2 * (m - 1), cs1 + 2 * (2 * M - m - 1), cs2 + 2 * (m - 1), om);
            store_inv_butterfly4_v4f(vblk, M, m, om);
        }

        if (M >= 2) {
            const int i = M >> 1;
            const V4F ia = v4f_ld(vblk + 8 * i);
            const V4F ib = v4f_ld(vblk + 8 * i + 4);
            const V4F iha = v4f_ld(vblk + 8 * (4 * M - i));
            const V4F ihb = v4f_ld(vblk + 8 * (4 * M - i) + 4);
            const V4F iqla = v4f_ld(vblk + 8 * (2 * M - i));
            const V4F iqlb = v4f_ld(vblk + 8 * (2 * M - i) + 4);
            const V4F iqha = v4f_ld(vblk + 8 * (2 * M + i));
            const V4F iqhb = v4f_ld(vblk + 8 * (2 * M + i) + 4);
            V4F oi[8];
            inv_butterfly4_v4f(ia, ib, iha, ihb, iqla, iqlb, iqha, iqhb,
                               cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1), cs2 + 2 * (i - 1), oi);
            store_inv_butterfly4_v4f(vblk, M, i, oi);
        }
    }

    STXLIB_FFT_INLINE void inv_butterfly4_v_norm_f32_v4_inputs(V4F ni, V4F nh,
                                                                 V4F nql, V4F nqh,
                                                                 const double* cs1i, const double* cs1q,
                                                                 const double* cs2i, V4F* out) const {
        V4F h0la, h0lb, h1la, h1lb;
        pair_expand_v4f_norm(v4f_catlo(ni, ni), v4f_cathi(ni, ni),
                             v4f_catlo(nh, nh), v4f_cathi(nh, nh),
                             v4f_set1cs(cs1i), v4f_set1cs(cs1i + 1),
                             h0la, h0lb, h1la, h1lb);

        V4F h0ha, h0hb, h1ha, h1hb;
        pair_expand_v4f_norm(v4f_catlo(nql, nql), v4f_cathi(nql, nql),
                             v4f_catlo(nqh, nqh), v4f_cathi(nqh, nqh),
                             v4f_set1cs(cs1q), v4f_set1cs(cs1q + 1),
                             h0ha, h0hb, h1ha, h1hb);

        V4F ea, eb, oa, ob;
        pair_expand_v4f_norm(h0la, h0lb, h0ha, h0hb,
                             v4f_set1cs(cs2i), v4f_set1cs(cs2i + 1),
                             ea, eb, oa, ob);
        out[0] = v4f_catlo(ea, eb);
        out[1] = v4f_catlo(oa, ob);

        pair_expand_v4f_norm(h1la, h1lb, h1ha, h1hb,
                             v4f_set1cs(cs2i), v4f_set1cs(cs2i + 1),
                             ea, eb, oa, ob);
        out[2] = v4f_catlo(ea, eb);
        out[3] = v4f_catlo(oa, ob);
    }

    STXLIB_FFT_INLINE void inv_butterfly4_v_norm_f32_v4(const float* vblk, int M, int i,
                                                           const double* cs1i, const double* cs1q,
                                                           const double* cs2i, V4F* out) const {
        inv_butterfly4_v_norm_f32_v4_inputs(v4f_ld(vblk + 4 * i),
                                            v4f_ld(vblk + 4 * (4 * M - i)),
                                            v4f_ld(vblk + 4 * (2 * M - i)),
                                            v4f_ld(vblk + 4 * (2 * M + i)),
                                            cs1i, cs1q, cs2i, out);
    }

    static STXLIB_FFT_INLINE void store_inv_butterfly4_v_norm_f32_v4(float* vblk, int M, int i,
                                                                       const V4F* out) {
        v4f_st(vblk + 4 * i, out[0]);
        v4f_st(vblk + 4 * (M + i), out[1]);
        v4f_st(vblk + 4 * (2 * M + i), out[2]);
        v4f_st(vblk + 4 * (3 * M + i), out[3]);
    }

    void terminal_radix2_inv_v_norm_f32(const Complex32* X, float* vw) const {
        const int q = half_ >> 1;
        vw[0] = 0.5f * (X[0].re + X[half_].re);
        vw[1] = 0.5f * (X[0].re - X[half_].re);
        vw[2] = X[q].re;
        vw[3] = -X[q].im;

        for (int i = 1; i < q; ++i) {
            const int hi = half_ - i;
            float ea, eb, oa, ob;
            pair_expand_cs_f32(X[i].re, -X[i].im, X[hi].re, -X[hi].im,
                               static_cast<float>(cos_[i]),
                               static_cast<float>(sin_[i]),
                               ea, eb, oa, ob);
            vw[4 * i] = ea;
            vw[4 * i + 1] = oa;
            vw[4 * i + 2] = eb;
            vw[4 * i + 3] = ob;
        }
    }

    // Inverse terminal: standard bins -> paired-slot layout (reverse of
    // terminal_v4f_f32). Inverse rotations in V4F, ZIPLO undoes the mirror
    // crossing, CATLO writes cA/cB back into the low2/high2 lanes.
    void terminal_inv_v4f_f32(const Complex32* X, float* fvw) const {
        const int M = half_ >> 2;
        const double* cs1 = cs_ + 2 * twiddle_offset_[twiddle_stage_count_ - 1];

        {
            const float sum0 = 0.5f * (X[0].re + X[half_].re);
            const float sum1 = 0.5f * (X[0].re - X[half_].re);
            const float d0 = X[2 * M].re;
            const float d1 = -X[2 * M].im;
            fvw[0] = 0.5f * (sum0 + d0);
            fvw[2] = 0.5f * (sum0 - d0);
            fvw[1] = 0.5f * (sum1 + d1);
            fvw[3] = 0.5f * (sum1 - d1);
            float e4, e6, e5, e7;
            pair_expand_norm_t<float>(X[M].re, -X[M].im, X[half_ - M].re, -X[half_ - M].im,
                                      cs1 + 2 * (M - 1), e4, e6, e5, e7);
            fvw[4] = e4;
            fvw[5] = e5;
            fvw[6] = e6;
            fvw[7] = e7;
        }

        const double* rec = fterm_cs_;
        for (int i = 1; i < M; ++i, rec += 6) {
            // deproject the 4 bins to (a, b) = (re, -im) in lanes {0,1}
            const V4F fa = v4f_set4(X[i].re, X[2 * M - i].re, X[i].re, X[2 * M - i].re);
            const V4F fb = v4f_set4(-X[i].im, -X[2 * M - i].im, -X[i].im, -X[2 * M - i].im);
            const V4F ga = v4f_set4(X[half_ - i].re, X[2 * M + i].re, X[half_ - i].re, X[2 * M + i].re);
            const V4F gb = v4f_set4(-X[half_ - i].im, -X[2 * M + i].im, -X[half_ - i].im, -X[2 * M + i].im);
            const V4F cB = v4f_set4(static_cast<float>(rec[2]), static_cast<float>(rec[3]),
                                          static_cast<float>(rec[2]), static_cast<float>(rec[3]));
            const V4F sB = v4f_set4(static_cast<float>(rec[4]), static_cast<float>(rec[5]),
                                          static_cast<float>(rec[4]), static_cast<float>(rec[5]));
            // inverse level B -> ua, ub, va, vb (lanes 0,1)
            V4F ua, ub, va, vb;
            pair_expand_v4f_norm(fa, fb, ga, gb, cB, sB, ua, ub, va, vb);
            // reconstruct {la | ha} = ZIPLO(ua, va) (undoes the forward crossing)
            const V4F laha = v4f_ziplo(ua, va);
            const V4F lbhb = v4f_ziplo(ub, vb);
            // inverse level A -> aA, bA, aB, bB (lanes 0,1)
            V4F aA, bA, aB, bB;
            pair_expand_v4f_norm(laha, lbhb, v4f_cathi(laha, laha), v4f_cathi(lbhb, lbhb),
                                 v4f_set1cs(rec), v4f_set1cs(rec + 1), aA, bA, aB, bB);
            v4f_st(fvw + 8 * i, v4f_catlo(aA, aB));       // {aA.lo,aA.hi,aB.lo,aB.hi}
            v4f_st(fvw + 8 * i + 4, v4f_catlo(bA, bB));
        }
    }

    // Inverse seed scatter: reverse of seed_paired_pair_f32. cA is lanes {0,1},
    // cB is lanes {2,3}; each is the double seed_scatter_block_v core, scalar.
    STXLIB_FFT_INLINE void seed_scatter_paired_pair_f32(const float* fvw, float* output, int s, int eighth) const {
        const V4F hv = v4f_set1(0.5f);
        const int hq = half_ >> 1;
        const float* p = fvw + 8 * s;
        const float* q = fvw + 8 * (s + 1);
        const int j0 = rev_[s];
        const int jB = rev_[s + eighth];
        (void)jB;

        const V4F va = v4f_ld(p);
        const V4F vb = v4f_ld(p + 4);
        const V4F vc = v4f_ld(q);
        const V4F vd = v4f_ld(q + 4);
        const V4F e0 = v4f_mul(v4f_add(va, vb), hv);
        const V4F e1 = v4f_mul(v4f_sub(va, vb), hv);
        v4f_st(output + j0,           v4f_mul(v4f_add(e0, vc), hv));
        v4f_st(output + j0 + half_,   v4f_mul(v4f_sub(e0, vc), hv));
        v4f_st(output + j0 + hq,      v4f_mul(v4f_add(e1, vd), hv));
        v4f_st(output + j0 + hq + half_, v4f_mul(v4f_sub(e1, vd), hv));
    }

    // Collect-a-scatter-then-organize (row tiles), paired-layout port of
    // seed_scatter_row_tiles_v. rev(s+eighth) = rev(s)+2 makes a paired slot's
    // cA/cB outputs contiguous, so each chunk is one V4F store {cA.lo, cA.hi,
    // cB.lo, cB.hi}. Reads stride the vwork (scattered loads); writes land in
    // cache-local output rows -> no scatter to output.
    void seed_scatter_row_tiles_paired_f32(const float* fvw, float* output,
                                           int block_slots, int nb, int nb_bits) const {
        const V4F hv = v4f_set1(0.5f);
        const int hq = half_ >> 1;
        const int pairs = block_slots >> 1;
        const int row_bits = ilog2_pow2(pairs);
        for (int r = 0; r < pairs; ++r) {
            const int row = bitrev_int(r, row_bits);
            const int row_base = 4 * row * nb;
            float* row0 = output + row_base;
            float* row1 = output + row_base + half_;
            float* row2 = output + row_base + hq;
            float* row3 = output + row_base + hq + half_;
            for (int h = 0; h < nb; ++h) {
                const int col = bitrev_int(h, nb_bits);
                const int b = h * block_slots + 2 * r;
                const V4F va = v4f_ld(fvw + 8 * b);
                const V4F vb = v4f_ld(fvw + 8 * b + 4);
                const V4F vc = v4f_ld(fvw + 8 * (b + 1));
                const V4F vd = v4f_ld(fvw + 8 * (b + 1) + 4);
                const V4F e0 = v4f_mul(v4f_add(va, vb), hv);
                const V4F e1 = v4f_mul(v4f_sub(va, vb), hv);
                v4f_st(row0 + 4 * col, v4f_mul(v4f_add(e0, vc), hv));
                v4f_st(row1 + 4 * col, v4f_mul(v4f_sub(e0, vc), hv));
                v4f_st(row2 + 4 * col, v4f_mul(v4f_add(e1, vd), hv));
                v4f_st(row3 + 4 * col, v4f_mul(v4f_sub(e1, vd), hv));
            }
        }
    }

    // Mirrors compute_vwork_inv_norm on the paired layout: full-array split of
    // the top levels, per-block (cache-resident) split of the bottom levels,
    // then organize the output (row tiles for large N, block scatter for small).
    void compute_vwork_inv_v4f(float* fvw, float* output) const {
        const int eighth = half_ >> 2;   // P paired-slots
        int chain_limit = kF32MinInverseChain;
        for (int s = 16; s <= (n_ >> 2); s <<= 2) {
            chain_limit = s;
        }
        const int s_target = should_cache_block<float>(n_) ? kBlockedStageSize : chain_limit;
        const int block_slots = s_target >> 1;   // paired-slots per block
        const int nb = eighth / block_slots;
        const int nb_bits = ilog2_pow2(nb);

        int s = chain_limit;
        int stage = ilog2_pow2(s) - 2;
        for (; s >= (s_target << 2); s >>= 2, stage -= 2) {
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            const int M = s >> 3;
            for (int poff = 0; poff < eighth; poff += (s >> 1)) {
                split4_v4f(fvw + 8 * poff, M, cs2, cs1);
            }
        }

        if (nb >= kRowTileBlockCount) {
            for (int p = 0; p < nb; ++p) {
                const int base = p * block_slots;
                int ss = s_target;
                int st = ilog2_pow2(ss) - 2;
                for (; ss >= 16; ss >>= 2, st -= 2) {
                    const double* cs2 = cs_ + 2 * twiddle_offset_[st - 1];
                    const double* cs1 = cs_ + 2 * twiddle_offset_[st];
                    const int M = ss >> 3;
                    for (int sub = base; sub < base + block_slots; sub += (ss >> 1)) {
                        split4_v4f(fvw + 8 * sub, M, cs2, cs1);
                    }
                }
            }
            seed_scatter_row_tiles_paired_f32(fvw, output, block_slots, nb, nb_bits);
            return;
        }

        for (int p = 0; p < nb; ++p) {
            const int base = bitrev_int(p, nb_bits) * block_slots;
            int ss = s_target;
            int st = ilog2_pow2(ss) - 2;
            for (; ss >= 16; ss >>= 2, st -= 2) {
                const double* cs2 = cs_ + 2 * twiddle_offset_[st - 1];
                const double* cs1 = cs_ + 2 * twiddle_offset_[st];
                const int M = ss >> 3;
                for (int sub = base; sub < base + block_slots; sub += (ss >> 1)) {
                    split4_v4f(fvw + 8 * sub, M, cs2, cs1);
                }
            }
            for (int sub = base; sub < base + block_slots; sub += 2) {
                seed_scatter_paired_pair_f32(fvw, output, sub, eighth);
            }
        }
    }

    // Fused first radix-4 split + V2->V4F pairing: reads the post-terminal
    // V2-f32 slots from vw (cache-hot), writes the paired-V4F work layout
    // directly, collapsing split4_v_norm_f32 + pair_v2_to_v4f_f32 into one
    // pass. Same palindromic read pattern as split4_v_norm_f32, but on the
    // cache-resident terminal output rather than the scattered input bins.
    void split4_pair_v_norm_f32(const float* vw, float* fvw, int M,
                                const double* cs2, const double* cs1) const {
        // i = 0 special (split4_v_norm_f32's i=0 algebra), packed to paired 0/M.
        {
            const float* c0 = vw;
            const float* c1 = vw + 4 * M;
            const float* c2 = vw + 8 * M;
            const float* c3 = vw + 12 * M;
            const float h0dc0 = 0.5f * (c0[0] + c0[2]);
            const float h0dc1 = 0.5f * (c0[1] + c0[3]);
            const float h1dc0 = 0.5f * (c0[0] - c0[2]);
            const float h1dc1 = 0.5f * (c0[1] - c0[3]);
            const float cM = static_cast<float>(cs1[2 * (M - 1)]);
            const float sM = static_cast<float>(cs1[2 * (M - 1) + 1]);
            float ny00, ny01, ny20, ny21, ny10, ny11, ny30, ny31;
            pair_expand_cs_f32(c1[0], c1[2], c3[0], c3[2], cM, sM, ny00, ny01, ny20, ny21);
            pair_expand_cs_f32(c1[1], c1[3], c3[1], c3[3], cM, sM, ny10, ny11, ny30, ny31);
            v4f_st(fvw,     v4f_set4(0.5f * (h0dc0 + c2[0]), 0.5f * (h0dc1 + c2[1]),
                                     0.5f * (h1dc0 + c2[2]), 0.5f * (h1dc1 + c2[3])));
            v4f_st(fvw + 4, v4f_set4(ny00, ny10, ny20, ny30));
            v4f_st(fvw + 8 * M,     v4f_set4(0.5f * (h0dc0 - c2[0]), 0.5f * (h0dc1 - c2[1]),
                                             0.5f * (h1dc0 - c2[2]), 0.5f * (h1dc1 - c2[3])));
            v4f_st(fvw + 8 * M + 4, v4f_set4(ny01, ny11, ny21, ny31));
        }

        for (int i = 1; i < M; ++i) {
            V4F out[4];
            inv_butterfly4_v_norm_f32_v4_inputs(v4f_ld(vw + 4 * i),
                                                v4f_ld(vw + 4 * (4 * M - i)),
                                                v4f_ld(vw + 4 * (2 * M - i)),
                                                v4f_ld(vw + 4 * (2 * M + i)),
                                                cs1 + 2 * (i - 1), cs1 + 2 * (2 * M - i - 1),
                                                cs2 + 2 * (i - 1), out);
            v4f_st(fvw + 8 * i,           v4f_catlo(out[0], out[2]));
            v4f_st(fvw + 8 * i + 4,       v4f_cathi(out[0], out[2]));
            v4f_st(fvw + 8 * (M + i),     v4f_catlo(out[1], out[3]));
            v4f_st(fvw + 8 * (M + i) + 4, v4f_cathi(out[1], out[3]));
        }
    }

    void inverse_simd_f32_wide(const Complex32* input, float* output, float* work) const {
        if (radix2_terminal_) {
            terminal_radix2_inv_v_norm_f32(input, output);
            const int s = n_ >> 1;
            const int stage = ilog2_pow2(s) - 2;
            const double* cs2 = cs_ + 2 * twiddle_offset_[stage - 1];
            const double* cs1 = cs_ + 2 * twiddle_offset_[stage];
            split4_pair_v_norm_f32(output, work, s >> 3, cs2, cs1);
            compute_vwork_inv_v4f(work, output);
            return;
        }
        terminal_inv_v4f_f32(input, work);
        compute_vwork_inv_v4f(work, output);
    }
};
