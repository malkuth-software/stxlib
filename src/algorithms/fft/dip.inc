// stx_fft's DIP mode: BFFT's DIP kernel (src/detail/bruun_dip_kernel.hpp
// at edf5f01, MIT, the owner's own; NOTICE.md), float64 only as in BFFT.
// Included only by variant.inc, inside one SIMD tier's namespace
// (STXLIB_FFT_LEVEL is BFFT's BRUUN_LEVEL).
//
// The walk, its cells, seeds, tails and egress are BFFT's text, changed
// only by the mechanical transform described in dif.inc (fused
// multiply-adds spelled out, lanes.inc's names) and these: the tables
// (cos_, sin_, cs_, tperm_) live in the plan (DipTables), built by
// plan.cpp from sincospi with BFFT's quarter-wave symmetry step; the
// kernel is a view of them (DipKernel). BFFT's header follows.
//
// Selecting harmonic combs (the reason the owner keeps DIP; nothing in
// STXlib does it yet): the leaves of the subtree (d, E) are exactly the
// bins { j*E + d, j*E - d }, so a pair of combs of spacing E and offset d
// is one subtree of the walk below. Running fwd_tree only down to (d, E)
// leaves that comb pair's packet in its span [ a : w | b : w ], w = N/E,
// and egress_span names each of its bins (rank_bin, with the universal
// permutation T_w).
//

// Internal DIP (diagonal-in-packets) real FFT kernel.
//
// THE WALK (2026-07-04 rework). One in-place, depth-first descent of the Bruun
// diagonal tree; no ping-pong, no scratch, no interior permutation.
//
// State = contiguous SPANS. A bin span at packet level e is the phase packet
// (d, e): the residue pair (a, b) = (Re, -Im) of diagonal d, stored
// [ a : w | b : w ] with w = n/e. The ridge span is [ dc : w | ny : w ].
// The stage cell splits each row into (even | odd) column halves and applies
// the normalized Bruun rotation theta(d) = pi*d/e -- writing the two children
// (d, 2e) and (e-d, 2e) INTO THE PARENT'S OWN ADDRESSES:
//
//   [ ea : w2 | oa : w2 | eb : w2 | ob : w2 ]   parent (d, e)
//        |         |         |         |        cell_fwd_ip: four streams,
//        v         v         v         v        the SAME addresses read+written
//   [ la : w2 | lb : w2 | ha : w2 | hb : w2 ] = [ child (d, 2e) | child (e-d, 2e) ]
//
// so every span is self-contained forever after: the walk recurses into the
// children independently (depth-first), and once a span fits in cache its
// whole subtree completes cache-resident. The cache transition is emergent --
// no block sizes, no fuse depths, no tmp buffers. The Nyquist promotion is
// free: the ridge's ny row IS the promoted bin (e/2, 2e) in place (a = left
// half, b = right half), so the old copy_span pass no longer exists.
//
// THE DISORDER LEDGER. The bit-reversal cannot be destroyed, only placed.
// This walk pays the long-range component as COMB STREAMS at both ends and
// confines all scramble to L1:
//   - the seed reads 8 comb streams of the input (long-range strides but
//     sequential within each stream, prefetch-friendly), one streaming pass.
//     Stream count matters: power-of-two comb strides alias L1 sets, so a
//     16-stream seed blows associativity (measured 9x the 8-stream pass);
//   - the leaves of any subtree (d, E) are exactly the bins {j*E +/- d} --
//     two arithmetic combs -- and the map from the subtree's tree order to
//     ascending bin order is UNIVERSAL (d-independent): every leaf value is
//     m*E + s*d with (m, s) a function of the path only (lo child keeps
//     (m, s); the hi child at depth j maps to (2^j - m, -s)), and 2d < E
//     makes every comparison depend on (m, s) alone. So one tiny shared
//     table T_w per span size gives, when a cache-resident subtree
//     completes, a streaming emission of its bins: L1-hot permuted reads,
//     two monotonic strided writes (forward egress) -- and the mirror
//     ingress for the inverse (two monotonic strided reads of the bins,
//     L1-local scatter into the span, then a pure ascent). Measured: both
//     a fused per-leaf scatter and a whole-array gather pass lose to this
//     (far-range 16-byte traffic vs streams). The polarity is asymmetric:
//     the forward takes a radix-2 parity step at w == 2*kEgressW so egress
//     WRITES (which pay RFO) land on the densest combs the L1 span budget
//     allows, while the inverse skips it -- comb READS prefetch at any
//     density and the ingress scatter prefers the smaller landing span.
// The interior pays ZERO rearrangement: no transposes, no swaps, no sorting.
// Span addresses run in tree order; the ANGLE law stays purely diagonal:
// theta = pi*d/e, served at every level by the one scale-indexed length-n
// table via phase_table_index(d, e) = d*n/(2e).
//
// SIMD. Cells are isoclinic column rotations: (c, s) broadcast, four
// contiguous vector streams, loads and stores to the same addresses, no
// permutes at any width w2 >= 2 (AVX2 / V2 / scalar cascade; w2 is a power of
// two, so there is never a ragged tail). The LAST level (w2 == 1) runs as
// paired-lane vector cells (cell_fwd_pair / cell_inv_pair): the deepest span
// [a|o|b|p] is two contiguous vectors, so 4-5 shuffles per leaf cell buy the
// removal of all scalar flops and scalar moves. Instruction census at
// n = 4096 (per transform): ~17.5K vector loads / 16.5K vector stores /
// ~39K vector arith / ~4-5K shuffles vs 2 scalar flops, 4 scalar stores,
// ~2K scalar twiddle loads. In-place operation halves the memory traffic of
// the old out-of-place stage and the work buffer drops from 2n to n doubles.
//
// CONCURRENCY. The kernel is const after init; forward/inverse touch only the
// caller's work buffer (exactly n doubles) and the output. No internal
// scratch, no allocation, no shared mutable state.
//
// Validation lineage: experiments/phase_fft.py (the diagonal walk + the
// ordering theorem), experiments/dip_transport_symmetry.py (boundary-transport
// conservation; why interior transposes/ping-pongs were rejected),
// notes/dip_phase_packet_design.md (design history).

class DipKernel final {
public:
    explicit DipKernel(const DipTables& tables) noexcept
        : n_(tables.n), half_(tables.half), cos_(tables.cos), sin_(tables.sin), cs_(tables.cs),
          tperm_(tables.tperm) {}


    int size() const noexcept { return n_; }
    int bins() const noexcept { return half_ + 1; }
    int work_size() const noexcept { return n_; }

    void forward_standard(const double* __restrict input,
                          Complex64* __restrict output,
                          double* __restrict work) const {
        if (n_ >= 8) {
            // Seed at level 8: 8 comb read + 8 write streams, the mirror of
            // the inverse's tail8. Seeding at 16 (16 + 16 streams at power-of-
            // two strides) aliases L1 sets past associativity and measured 9x
            // slower than seed8 in isolation (3.8 ms vs 0.43 ms at n = 1M).
            forward_seed8(input, work);
            // level-8 slot order (tree order): [R][B2][B1][B3]
            const int q = n_ >> 3;
            fwd_ridge(work, q, 8, output);
            fwd_tree(work + 2 * static_cast<std::size_t>(q), q, 2, 8, output);
            fwd_tree(work + 4 * static_cast<std::size_t>(q), q, 1, 8, output);
            fwd_tree(work + 6 * static_cast<std::size_t>(q), q, 3, 8, output);
        } else {
            forward_seed4(input, work);
            fwd_tree(work + 2, 1, 1, 4, output);
        }
        output[0].re = work[0];
        output[0].im = 0.0;
        output[half_].re = work[1];
        output[half_].im = 0.0;
    }

    void inverse_standard(const Complex64* __restrict input,
                          double* __restrict output,
                          double* __restrict work) const {
        if (n_ >= 8) {
            // comb-ingress + ascend the level-8 subtrees, then unseed the
            // slot state [R][B2][B1][B3] to time order.
            const int q = n_ >> 3;
            inv_ridge(work, q, 8, input);
            inv_tree(work + 2 * static_cast<std::size_t>(q), q, 2, 8, input);
            inv_tree(work + 4 * static_cast<std::size_t>(q), q, 1, 8, input);
            inv_tree(work + 6 * static_cast<std::size_t>(q), q, 3, 8, input);
            inverse_tail8(work, output, q);
        } else {
            work[0] = input[0].re;      // dc
            work[1] = input[2].re;      // ny
            work[2] = input[1].re;      // a1
            work[3] = -input[1].im;     // b1
            inverse_tail4(work, output, 1);
        }
    }

private:
    // -----------------------------------------------------------------------
    // In-place span cells. The twiddle (c, s) is constant along the column
    // axis (isoclinic rotation): broadcast it and run pure vertical SIMD on
    // four contiguous streams, storing to the same addresses just loaded.
    // w2 is 1 or an even power of two, so the cascade has no ragged tail.
    // -----------------------------------------------------------------------

    // Forward Bruun cell on the span [ ea | oa | eb | ob ] (each w2 wide):
    // parent (d, e) -> children [ la | lb ] = (d, 2e), [ ha | hb ] = (e-d, 2e).
    static STXLIB_FFT_INLINE void cell_fwd_ip(double* __restrict v, int w2,
                                                double c, double s) {
        double* __restrict ea = v;
        double* __restrict oa = v + w2;
        double* __restrict eb = v + 2 * static_cast<std::size_t>(w2);
        double* __restrict ob = v + 3 * static_cast<std::size_t>(w2);
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        {
            const __m256d vc = _mm256_set1_pd(c);
            const __m256d vs = _mm256_set1_pd(s);
            for (; i + 3 < w2; i += 4) {
                const __m256d a = _mm256_loadu_pd(ea + i);
                const __m256d o = _mm256_loadu_pd(oa + i);
                const __m256d b = _mm256_loadu_pd(eb + i);
                const __m256d p = _mm256_loadu_pd(ob + i);
                const __m256d R = _mm256_sub_pd(_mm256_mul_pd(vc, o), _mm256_mul_pd(vs, p));
                const __m256d I = _mm256_add_pd(_mm256_mul_pd(vs, o), _mm256_mul_pd(vc, p));
                _mm256_storeu_pd(ea + i, _mm256_add_pd(a, R));
                _mm256_storeu_pd(oa + i, _mm256_add_pd(b, I));
                _mm256_storeu_pd(eb + i, _mm256_sub_pd(a, R));
                _mm256_storeu_pd(ob + i, _mm256_sub_pd(I, b));
            }
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        {
            const V2 vc = v2_set1(c);
            const V2 vs = v2_set1(s);
            for (; i + 1 < w2; i += 2) {
                const V2 a = v2_ld(ea + i);
                const V2 o = v2_ld(oa + i);
                const V2 b = v2_ld(eb + i);
                const V2 p = v2_ld(ob + i);
                const V2 R = v2_sub(v2_mul(vc, o), v2_mul(vs, p));
                const V2 I = v2_add(v2_mul(vs, o), v2_mul(vc, p));
                v2_st(ea + i, v2_add(a, R));
                v2_st(oa + i, v2_add(b, I));
                v2_st(eb + i, v2_sub(a, R));
                v2_st(ob + i, v2_sub(I, b));
            }
        }
#endif
        for (; i < w2; ++i) {
            const double a = ea[i];
            const double o = oa[i];
            const double b = eb[i];
            const double p = ob[i];
            const double R = c * o - s * p;
            const double I = s * o + c * p;
            ea[i] = a + R;    // la
            oa[i] = b + I;    // lb
            eb[i] = a - R;    // ha
            ob[i] = I - b;    // hb
        }
    }

    // Inverse Bruun cell on the span [ la | lb | ha | hb ] -> [ ea | oa | eb | ob ].
    // hc = 0.5*c, hs = 0.5*s fold the expand 0.5 into the twiddle for the
    // rotated outputs; the even outputs keep an explicit 0.5.
    static STXLIB_FFT_INLINE void cell_inv_ip(double* __restrict v, int w2,
                                                double hc, double hs) {
        double* __restrict la = v;
        double* __restrict lb = v + w2;
        double* __restrict ha = v + 2 * static_cast<std::size_t>(w2);
        double* __restrict hb = v + 3 * static_cast<std::size_t>(w2);
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        {
            const __m256d vhc = _mm256_set1_pd(hc);
            const __m256d vhs = _mm256_set1_pd(hs);
            const __m256d half = _mm256_set1_pd(0.5);
            for (; i + 3 < w2; i += 4) {
                const __m256d A = _mm256_loadu_pd(la + i);
                const __m256d B = _mm256_loadu_pd(lb + i);
                const __m256d C = _mm256_loadu_pd(ha + i);
                const __m256d D = _mm256_loadu_pd(hb + i);
                const __m256d u = _mm256_sub_pd(A, C);
                const __m256d t = _mm256_add_pd(B, D);
                _mm256_storeu_pd(la + i, _mm256_mul_pd(half, _mm256_add_pd(A, C)));
                _mm256_storeu_pd(lb + i, _mm256_add_pd(_mm256_mul_pd(vhc, u), _mm256_mul_pd(vhs, t)));
                _mm256_storeu_pd(ha + i, _mm256_mul_pd(half, _mm256_sub_pd(B, D)));
                _mm256_storeu_pd(hb + i, _mm256_sub_pd(_mm256_mul_pd(vhc, t), _mm256_mul_pd(vhs, u)));
            }
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        {
            const V2 vhc = v2_set1(hc);
            const V2 vhs = v2_set1(hs);
            const V2 half = v2_set1(0.5);
            for (; i + 1 < w2; i += 2) {
                const V2 A = v2_ld(la + i);
                const V2 B = v2_ld(lb + i);
                const V2 C = v2_ld(ha + i);
                const V2 D = v2_ld(hb + i);
                const V2 u = v2_sub(A, C);
                const V2 t = v2_add(B, D);
                v2_st(la + i, v2_mul(half, v2_add(A, C)));
                v2_st(lb + i, v2_add(v2_mul(vhc, u), v2_mul(vhs, t)));
                v2_st(ha + i, v2_mul(half, v2_sub(B, D)));
                v2_st(hb + i, v2_sub(v2_mul(vhc, t), v2_mul(vhs, u)));
            }
        }
#endif
        for (; i < w2; ++i) {
            const double A = la[i];
            const double B = lb[i];
            const double C = ha[i];
            const double D = hb[i];
            const double u = A - C;
            const double t = B + D;
            la[i] = 0.5 * (A + C);      // ea
            lb[i] = hc * u + hs * t;    // oa
            ha[i] = 0.5 * (B - D);      // eb
            hb[i] = hc * t - hs * u;    // ob
        }
    }

    // Fused two-level forward cell (the DIP mirror of the DIF's norm2_fused):
    // parent (d, e) plus both child cells in ONE pass over the span's eight
    // w4-wide streams -- half the span traffic and loop overhead of two
    // radix-2 passes. Stream layout in: [ a0 a1 a2 a3 | b0 b1 b2 b3 ]
    // (a-row and b-row quarters); out: the four grandchild pairs
    // (d,4e) (2e-d,4e) (e-d,4e) (e+d,4e) in tree order.
    static STXLIB_FFT_INLINE void cell4_fwd_ip(double* __restrict v, int w4,
                                                 double c0, double s0,
                                                 double cl, double sl,
                                                 double ch, double sh) {
        double* __restrict a0 = v;
        double* __restrict a1 = v + w4;
        double* __restrict a2 = v + 2 * static_cast<std::size_t>(w4);
        double* __restrict a3 = v + 3 * static_cast<std::size_t>(w4);
        double* __restrict b0 = v + 4 * static_cast<std::size_t>(w4);
        double* __restrict b1 = v + 5 * static_cast<std::size_t>(w4);
        double* __restrict b2 = v + 6 * static_cast<std::size_t>(w4);
        double* __restrict b3 = v + 7 * static_cast<std::size_t>(w4);
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        {
            const __m256d vc0 = _mm256_set1_pd(c0);
            const __m256d vs0 = _mm256_set1_pd(s0);
            const __m256d vcl = _mm256_set1_pd(cl);
            const __m256d vsl = _mm256_set1_pd(sl);
            const __m256d vch = _mm256_set1_pd(ch);
            const __m256d vsh = _mm256_set1_pd(sh);
            for (; i + 3 < w4; i += 4) {
                const __m256d A0 = _mm256_loadu_pd(a0 + i);
                const __m256d A1 = _mm256_loadu_pd(a1 + i);
                const __m256d A2 = _mm256_loadu_pd(a2 + i);
                const __m256d A3 = _mm256_loadu_pd(a3 + i);
                const __m256d B0 = _mm256_loadu_pd(b0 + i);
                const __m256d B1 = _mm256_loadu_pd(b1 + i);
                const __m256d B2 = _mm256_loadu_pd(b2 + i);
                const __m256d B3 = _mm256_loadu_pd(b3 + i);
                const __m256d R0 = _mm256_sub_pd(_mm256_mul_pd(vc0, A2), _mm256_mul_pd(vs0, B2));
                const __m256d I0 = _mm256_add_pd(_mm256_mul_pd(vs0, A2), _mm256_mul_pd(vc0, B2));
                const __m256d R1 = _mm256_sub_pd(_mm256_mul_pd(vc0, A3), _mm256_mul_pd(vs0, B3));
                const __m256d I1 = _mm256_add_pd(_mm256_mul_pd(vs0, A3), _mm256_mul_pd(vc0, B3));
                const __m256d la0 = _mm256_add_pd(A0, R0);
                const __m256d la1 = _mm256_add_pd(A1, R1);
                const __m256d lb0 = _mm256_add_pd(B0, I0);
                const __m256d lb1 = _mm256_add_pd(B1, I1);
                const __m256d ha0 = _mm256_sub_pd(A0, R0);
                const __m256d ha1 = _mm256_sub_pd(A1, R1);
                const __m256d hb0 = _mm256_sub_pd(I0, B0);
                const __m256d hb1 = _mm256_sub_pd(I1, B1);
                const __m256d Rl = _mm256_sub_pd(_mm256_mul_pd(vcl, la1), _mm256_mul_pd(vsl, lb1));
                const __m256d Il = _mm256_add_pd(_mm256_mul_pd(vsl, la1), _mm256_mul_pd(vcl, lb1));
                const __m256d Rh = _mm256_sub_pd(_mm256_mul_pd(vch, ha1), _mm256_mul_pd(vsh, hb1));
                const __m256d Ih = _mm256_add_pd(_mm256_mul_pd(vsh, ha1), _mm256_mul_pd(vch, hb1));
                _mm256_storeu_pd(a0 + i, _mm256_add_pd(la0, Rl));
                _mm256_storeu_pd(a1 + i, _mm256_add_pd(lb0, Il));
                _mm256_storeu_pd(a2 + i, _mm256_sub_pd(la0, Rl));
                _mm256_storeu_pd(a3 + i, _mm256_sub_pd(Il, lb0));
                _mm256_storeu_pd(b0 + i, _mm256_add_pd(ha0, Rh));
                _mm256_storeu_pd(b1 + i, _mm256_add_pd(hb0, Ih));
                _mm256_storeu_pd(b2 + i, _mm256_sub_pd(ha0, Rh));
                _mm256_storeu_pd(b3 + i, _mm256_sub_pd(Ih, hb0));
            }
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        {
            const V2 vc0 = v2_set1(c0);
            const V2 vs0 = v2_set1(s0);
            const V2 vcl = v2_set1(cl);
            const V2 vsl = v2_set1(sl);
            const V2 vch = v2_set1(ch);
            const V2 vsh = v2_set1(sh);
            for (; i + 1 < w4; i += 2) {
                const V2 A0 = v2_ld(a0 + i);
                const V2 A1 = v2_ld(a1 + i);
                const V2 A2 = v2_ld(a2 + i);
                const V2 A3 = v2_ld(a3 + i);
                const V2 B0 = v2_ld(b0 + i);
                const V2 B1 = v2_ld(b1 + i);
                const V2 B2 = v2_ld(b2 + i);
                const V2 B3 = v2_ld(b3 + i);
                const V2 R0 = v2_sub(v2_mul(vc0, A2), v2_mul(vs0, B2));
                const V2 I0 = v2_add(v2_mul(vs0, A2), v2_mul(vc0, B2));
                const V2 R1 = v2_sub(v2_mul(vc0, A3), v2_mul(vs0, B3));
                const V2 I1 = v2_add(v2_mul(vs0, A3), v2_mul(vc0, B3));
                const V2 la0 = v2_add(A0, R0);
                const V2 la1 = v2_add(A1, R1);
                const V2 lb0 = v2_add(B0, I0);
                const V2 lb1 = v2_add(B1, I1);
                const V2 ha0 = v2_sub(A0, R0);
                const V2 ha1 = v2_sub(A1, R1);
                const V2 hb0 = v2_sub(I0, B0);
                const V2 hb1 = v2_sub(I1, B1);
                const V2 Rl = v2_sub(v2_mul(vcl, la1), v2_mul(vsl, lb1));
                const V2 Il = v2_add(v2_mul(vsl, la1), v2_mul(vcl, lb1));
                const V2 Rh = v2_sub(v2_mul(vch, ha1), v2_mul(vsh, hb1));
                const V2 Ih = v2_add(v2_mul(vsh, ha1), v2_mul(vch, hb1));
                v2_st(a0 + i, v2_add(la0, Rl));
                v2_st(a1 + i, v2_add(lb0, Il));
                v2_st(a2 + i, v2_sub(la0, Rl));
                v2_st(a3 + i, v2_sub(Il, lb0));
                v2_st(b0 + i, v2_add(ha0, Rh));
                v2_st(b1 + i, v2_add(hb0, Ih));
                v2_st(b2 + i, v2_sub(ha0, Rh));
                v2_st(b3 + i, v2_sub(Ih, hb0));
            }
        }
#endif
        for (; i < w4; ++i) {
            const double A0 = a0[i];
            const double A1 = a1[i];
            const double A2 = a2[i];
            const double A3 = a3[i];
            const double B0 = b0[i];
            const double B1 = b1[i];
            const double B2 = b2[i];
            const double B3 = b3[i];
            const double R0 = c0 * A2 - s0 * B2;
            const double I0 = s0 * A2 + c0 * B2;
            const double R1 = c0 * A3 - s0 * B3;
            const double I1 = s0 * A3 + c0 * B3;
            const double la0 = A0 + R0;
            const double la1 = A1 + R1;
            const double lb0 = B0 + I0;
            const double lb1 = B1 + I1;
            const double ha0 = A0 - R0;
            const double ha1 = A1 - R1;
            const double hb0 = I0 - B0;
            const double hb1 = I1 - B1;
            const double Rl = cl * la1 - sl * lb1;
            const double Il = sl * la1 + cl * lb1;
            const double Rh = ch * ha1 - sh * hb1;
            const double Ih = sh * ha1 + ch * hb1;
            a0[i] = la0 + Rl;
            a1[i] = lb0 + Il;
            a2[i] = la0 - Rl;
            a3[i] = Il - lb0;
            b0[i] = ha0 + Rh;
            b1[i] = hb0 + Ih;
            b2[i] = ha0 - Rh;
            b3[i] = Ih - hb0;
        }
    }

    // Fused two-level inverse cell: both child inverse cells plus the parent
    // inverse cell in one pass. Twiddles arrive pre-halved (hc = 0.5*c).
    static STXLIB_FFT_INLINE void cell4_inv_ip(double* __restrict v, int w4,
                                                 double hc0, double hs0,
                                                 double hcl, double hsl,
                                                 double hch, double hsh) {
        double* __restrict a0 = v;
        double* __restrict a1 = v + w4;
        double* __restrict a2 = v + 2 * static_cast<std::size_t>(w4);
        double* __restrict a3 = v + 3 * static_cast<std::size_t>(w4);
        double* __restrict b0 = v + 4 * static_cast<std::size_t>(w4);
        double* __restrict b1 = v + 5 * static_cast<std::size_t>(w4);
        double* __restrict b2 = v + 6 * static_cast<std::size_t>(w4);
        double* __restrict b3 = v + 7 * static_cast<std::size_t>(w4);
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        {
            const __m256d vhc0 = _mm256_set1_pd(hc0);
            const __m256d vhs0 = _mm256_set1_pd(hs0);
            const __m256d vhcl = _mm256_set1_pd(hcl);
            const __m256d vhsl = _mm256_set1_pd(hsl);
            const __m256d vhch = _mm256_set1_pd(hch);
            const __m256d vhsh = _mm256_set1_pd(hsh);
            const __m256d half = _mm256_set1_pd(0.5);
            for (; i + 3 < w4; i += 4) {
                const __m256d X0 = _mm256_loadu_pd(a0 + i);
                const __m256d X1 = _mm256_loadu_pd(a1 + i);
                const __m256d X2 = _mm256_loadu_pd(a2 + i);
                const __m256d X3 = _mm256_loadu_pd(a3 + i);
                const __m256d X4 = _mm256_loadu_pd(b0 + i);
                const __m256d X5 = _mm256_loadu_pd(b1 + i);
                const __m256d X6 = _mm256_loadu_pd(b2 + i);
                const __m256d X7 = _mm256_loadu_pd(b3 + i);
                const __m256d u = _mm256_sub_pd(X0, X2);
                const __m256d t = _mm256_add_pd(X1, X3);
                const __m256d la0 = _mm256_mul_pd(half, _mm256_add_pd(X0, X2));
                const __m256d la1 = _mm256_add_pd(_mm256_mul_pd(vhcl, u), _mm256_mul_pd(vhsl, t));
                const __m256d lb0 = _mm256_mul_pd(half, _mm256_sub_pd(X1, X3));
                const __m256d lb1 = _mm256_sub_pd(_mm256_mul_pd(vhcl, t), _mm256_mul_pd(vhsl, u));
                const __m256d u2 = _mm256_sub_pd(X4, X6);
                const __m256d t2 = _mm256_add_pd(X5, X7);
                const __m256d ha0 = _mm256_mul_pd(half, _mm256_add_pd(X4, X6));
                const __m256d ha1 = _mm256_add_pd(_mm256_mul_pd(vhch, u2), _mm256_mul_pd(vhsh, t2));
                const __m256d hb0 = _mm256_mul_pd(half, _mm256_sub_pd(X5, X7));
                const __m256d hb1 = _mm256_sub_pd(_mm256_mul_pd(vhch, t2), _mm256_mul_pd(vhsh, u2));
                const __m256d u3 = _mm256_sub_pd(la0, ha0);
                const __m256d t3 = _mm256_add_pd(lb0, hb0);
                const __m256d u4 = _mm256_sub_pd(la1, ha1);
                const __m256d t4 = _mm256_add_pd(lb1, hb1);
                _mm256_storeu_pd(a0 + i, _mm256_mul_pd(half, _mm256_add_pd(la0, ha0)));
                _mm256_storeu_pd(a1 + i, _mm256_mul_pd(half, _mm256_add_pd(la1, ha1)));
                _mm256_storeu_pd(a2 + i, _mm256_add_pd(_mm256_mul_pd(vhc0, u3), _mm256_mul_pd(vhs0, t3)));
                _mm256_storeu_pd(a3 + i, _mm256_add_pd(_mm256_mul_pd(vhc0, u4), _mm256_mul_pd(vhs0, t4)));
                _mm256_storeu_pd(b0 + i, _mm256_mul_pd(half, _mm256_sub_pd(lb0, hb0)));
                _mm256_storeu_pd(b1 + i, _mm256_mul_pd(half, _mm256_sub_pd(lb1, hb1)));
                _mm256_storeu_pd(b2 + i, _mm256_sub_pd(_mm256_mul_pd(vhc0, t3), _mm256_mul_pd(vhs0, u3)));
                _mm256_storeu_pd(b3 + i, _mm256_sub_pd(_mm256_mul_pd(vhc0, t4), _mm256_mul_pd(vhs0, u4)));
            }
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        {
            const V2 vhc0 = v2_set1(hc0);
            const V2 vhs0 = v2_set1(hs0);
            const V2 vhcl = v2_set1(hcl);
            const V2 vhsl = v2_set1(hsl);
            const V2 vhch = v2_set1(hch);
            const V2 vhsh = v2_set1(hsh);
            const V2 half = v2_set1(0.5);
            for (; i + 1 < w4; i += 2) {
                const V2 X0 = v2_ld(a0 + i);
                const V2 X1 = v2_ld(a1 + i);
                const V2 X2 = v2_ld(a2 + i);
                const V2 X3 = v2_ld(a3 + i);
                const V2 X4 = v2_ld(b0 + i);
                const V2 X5 = v2_ld(b1 + i);
                const V2 X6 = v2_ld(b2 + i);
                const V2 X7 = v2_ld(b3 + i);
                const V2 u = v2_sub(X0, X2);
                const V2 t = v2_add(X1, X3);
                const V2 la0 = v2_mul(half, v2_add(X0, X2));
                const V2 la1 = v2_add(v2_mul(vhcl, u), v2_mul(vhsl, t));
                const V2 lb0 = v2_mul(half, v2_sub(X1, X3));
                const V2 lb1 = v2_sub(v2_mul(vhcl, t), v2_mul(vhsl, u));
                const V2 u2 = v2_sub(X4, X6);
                const V2 t2 = v2_add(X5, X7);
                const V2 ha0 = v2_mul(half, v2_add(X4, X6));
                const V2 ha1 = v2_add(v2_mul(vhch, u2), v2_mul(vhsh, t2));
                const V2 hb0 = v2_mul(half, v2_sub(X5, X7));
                const V2 hb1 = v2_sub(v2_mul(vhch, t2), v2_mul(vhsh, u2));
                const V2 u3 = v2_sub(la0, ha0);
                const V2 t3 = v2_add(lb0, hb0);
                const V2 u4 = v2_sub(la1, ha1);
                const V2 t4 = v2_add(lb1, hb1);
                v2_st(a0 + i, v2_mul(half, v2_add(la0, ha0)));
                v2_st(a1 + i, v2_mul(half, v2_add(la1, ha1)));
                v2_st(a2 + i, v2_add(v2_mul(vhc0, u3), v2_mul(vhs0, t3)));
                v2_st(a3 + i, v2_add(v2_mul(vhc0, u4), v2_mul(vhs0, t4)));
                v2_st(b0 + i, v2_mul(half, v2_sub(lb0, hb0)));
                v2_st(b1 + i, v2_mul(half, v2_sub(lb1, hb1)));
                v2_st(b2 + i, v2_sub(v2_mul(vhc0, t3), v2_mul(vhs0, u3)));
                v2_st(b3 + i, v2_sub(v2_mul(vhc0, t4), v2_mul(vhs0, u4)));
            }
        }
#endif
        for (; i < w4; ++i) {
            const double X0 = a0[i];
            const double X1 = a1[i];
            const double X2 = a2[i];
            const double X3 = a3[i];
            const double X4 = b0[i];
            const double X5 = b1[i];
            const double X6 = b2[i];
            const double X7 = b3[i];
            const double u = X0 - X2;
            const double t = X1 + X3;
            const double la0 = 0.5 * (X0 + X2);
            const double la1 = hcl * u + hsl * t;
            const double lb0 = 0.5 * (X1 - X3);
            const double lb1 = hcl * t - hsl * u;
            const double u2 = X4 - X6;
            const double t2 = X5 + X7;
            const double ha0 = 0.5 * (X4 + X6);
            const double ha1 = hch * u2 + hsh * t2;
            const double hb0 = 0.5 * (X5 - X7);
            const double hb1 = hch * t2 - hsh * u2;
            const double u3 = la0 - ha0;
            const double t3 = lb0 + hb0;
            const double u4 = la1 - ha1;
            const double t4 = lb1 + hb1;
            a0[i] = 0.5 * (la0 + ha0);
            a1[i] = 0.5 * (la1 + ha1);
            a2[i] = hc0 * u3 + hs0 * t3;
            a3[i] = hc0 * u4 + hs0 * t4;
            b0[i] = 0.5 * (lb0 - hb0);
            b1[i] = 0.5 * (lb1 - hb1);
            b2[i] = hc0 * t3 - hs0 * u3;
            b3[i] = hc0 * t4 - hs0 * u4;
        }
    }

    // In-place ridge fold on the dc row [ lo : q2 | hi : q2 ] -> [ dc' | ny' ].
    static STXLIB_FFT_INLINE void binom_fwd_ip(double* __restrict v, int q2) {
        double* __restrict lo = v;
        double* __restrict hi = v + q2;
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        for (; i + 3 < q2; i += 4) {
            const __m256d a = _mm256_loadu_pd(lo + i);
            const __m256d b = _mm256_loadu_pd(hi + i);
            _mm256_storeu_pd(lo + i, _mm256_add_pd(a, b));
            _mm256_storeu_pd(hi + i, _mm256_sub_pd(a, b));
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        for (; i + 1 < q2; i += 2) {
            const V2 a = v2_ld(lo + i);
            const V2 b = v2_ld(hi + i);
            v2_st(lo + i, v2_add(a, b));
            v2_st(hi + i, v2_sub(a, b));
        }
#endif
        for (; i < q2; ++i) {
            const double a = lo[i];
            const double b = hi[i];
            lo[i] = a + b;
            hi[i] = a - b;
        }
    }

    // In-place ridge unfold: [ dc' | ny' ] -> [ lo | hi ] with the 0.5.
    static STXLIB_FFT_INLINE void binom_inv_ip(double* __restrict v, int q2) {
        double* __restrict lo = v;
        double* __restrict hi = v + q2;
        int i = 0;
#if STXLIB_FFT_LEVEL >= 2
        {
            const __m256d half = _mm256_set1_pd(0.5);
            for (; i + 3 < q2; i += 4) {
                const __m256d a = _mm256_loadu_pd(lo + i);
                const __m256d b = _mm256_loadu_pd(hi + i);
                _mm256_storeu_pd(lo + i, _mm256_mul_pd(half, _mm256_add_pd(a, b)));
                _mm256_storeu_pd(hi + i, _mm256_mul_pd(half, _mm256_sub_pd(a, b)));
            }
        }
#endif
#if STXLIB_FFT_LEVEL >= 1
        {
            const V2 half = v2_set1(0.5);
            for (; i + 1 < q2; i += 2) {
                const V2 a = v2_ld(lo + i);
                const V2 b = v2_ld(hi + i);
                v2_st(lo + i, v2_mul(half, v2_add(a, b)));
                v2_st(hi + i, v2_mul(half, v2_sub(a, b)));
            }
        }
#endif
        for (; i < q2; ++i) {
            const double a = lo[i];
            const double b = hi[i];
            lo[i] = 0.5 * (a + b);
            hi[i] = 0.5 * (a - b);
        }
    }

    // theta(d, e) = pi*d/e -> table index d*n/(2e). e is always a power of
    // two and 2e exactly divides d*n at every call site, so the divide is a
    // shift by log2(2e) = ctz(e) + 1 (a single-cycle op, vs a ~12-cycle sdiv
    // that dominated the recursion overhead).
    int phase_table_index(int d, int e) const noexcept {
        return static_cast<int>((static_cast<long long>(d) * n_) >>
                                (ctz_u32(static_cast<unsigned>(e)) + 1));
    }

    // -----------------------------------------------------------------------
    // Paired-lane LEAF cells (w2 == 1). The deepest span [ a | o | b | p ] is
    // two contiguous vectors, so the last level runs fully in vector registers
    // instead of scalar:  [R, I] = [c, s]*[o, o] + [-s, c]*[p, p], lo child =
    // E + RI, hi child = NEGHI(E - RI) with E = [a, b]. The twiddle arrives as
    // one vector load from the interleaved cs_ table; [-s, c] is
    // SWAP(NEGHI([c, s])). 4 shuffles per cell -- the only permutes in the
    // kernel, and they buy the removal of ~8 scalar flops + 8 scalar moves
    // per leaf cell (the census target: zero scalar fp, zero scalar stores).
    // -----------------------------------------------------------------------

#if STXLIB_FFT_LEVEL >= 1
    // Register-twiddle forms: the caller passes [c, s] (forward) or the
    // pre-halved [hc, hs] (inverse) directly, so sibling spans -- whose
    // twiddles are exact swaps of each other (see the quarter-wave table
    // symmetry built by plan.cpp) -- share one table load: CS and v2_swap(CS).
    static STXLIB_FFT_INLINE void cell_fwd_pair_v(double* __restrict v,
                                                    V2 CS) {
        const V2 MS = v2_swap(v2_neghi(CS));          // [-s, c]
        const V2 L0 = v2_ld(v);                       // [a, o]
        const V2 L1 = v2_ld(v + 2);                   // [b, p]
        const V2 E = v2_unplo(L0, L1);                // [a, b]
        const V2 RI = v2_add(v2_mul(CS, v2_dup1(L0)), v2_mul(MS, v2_dup1(L1)));
        v2_st(v, v2_add(E, RI));                            // [a+R, b+I]
        v2_st(v + 2, v2_neghi(v2_sub(E, RI)));              // [a-R, I-b]
    }

    static STXLIB_FFT_INLINE void cell_inv_pair_v(double* __restrict v,
                                                    V2 HCS,
                                                    V2 half) {
        const V2 L = v2_ld(v);                        // [la, lb]
        const V2 NH = v2_neghi(v2_ld(v + 2));         // [ha, -hb]
        const V2 E = v2_mul(half, v2_add(L, NH));     // [ea, eb]
        const V2 UT = v2_sub(L, NH);                  // [u, t]
        const V2 TU = v2_neghi(v2_swap(UT));          // [t, -u]
        const V2 O = v2_add(v2_mul(v2_dup0(HCS), UT), v2_mul(v2_dup1(HCS), TU));
        v2_st(v, v2_unplo(E, O));                           // [ea, oa]
        v2_st(v + 2, v2_unphi(E, O));                       // [eb, ob]
    }

    STXLIB_FFT_INLINE void cell_fwd_pair(double* __restrict v, int k) const {
        cell_fwd_pair_v(v, v2_ld(cs_ + 2 * static_cast<std::size_t>(k)));
    }

    STXLIB_FFT_INLINE void cell_inv_pair(double* __restrict v, int k) const {
        const V2 half = v2_set1(0.5);
        cell_inv_pair_v(v, v2_mul(half, v2_ld(cs_ + 2 * static_cast<std::size_t>(k))), half);
    }
#else
    void cell_fwd_pair(double* __restrict v, int k) const {
        cell_fwd_ip(v, 1, cos_[static_cast<std::size_t>(k)],
                    sin_[static_cast<std::size_t>(k)]);
    }
    void cell_inv_pair(double* __restrict v, int k) const {
        cell_inv_ip(v, 1, 0.5 * cos_[static_cast<std::size_t>(k)],
                    0.5 * sin_[static_cast<std::size_t>(k)]);
    }
#endif

    // -----------------------------------------------------------------------
    // The depth-first walk. fwd_span descends the packet (d, e) held in
    // [ a : w | b : w ] at v; leaves (w == 1) stay in place -- the boundary
    // pass in forward_standard/inverse_standard resolves the permutation.
    // The w == 8 terminal straight-lines the last three levels (7 cells) so
    // the deep recursion collapses to one call per 16 doubles.
    // -----------------------------------------------------------------------

    // Mirror twiddle sharing (the DIT's mirrored-lane trick, free in the DIP
    // tree): sibling spans (d, 2e) and (e-d, 2e) have complementary angles,
    // so theta indices sum to n/4 and the quarter-wave-symmetric table makes
    // (cos, sin) of one sibling exactly the SWAP of the other. Each span8
    // therefore computes 4 indices and loads 2 twiddle vectors + 1 scalar
    // pair instead of 7 of each.
    STXLIB_FFT_INLINE void span8_fwd(double* __restrict v, int d, int e) const {
        const int e2 = e << 1;
        const int e4 = e2 << 1;
        const int k0 = phase_table_index(d, e);
        const int kl = phase_table_index(d, e2);
        const int k00 = phase_table_index(d, e4);
        const int k10 = phase_table_index(e - d, e4);
        const double cl = cos_[static_cast<std::size_t>(kl)];
        const double sl = sin_[static_cast<std::size_t>(kl)];
        cell_fwd_ip(v, 4, cos_[static_cast<std::size_t>(k0)], sin_[static_cast<std::size_t>(k0)]);
        cell_fwd_ip(v, 2, cl, sl);
        cell_fwd_ip(v + 8, 2, sl, cl);      // theta(e-d, 2e) = swap
#if STXLIB_FFT_LEVEL >= 1
        const V2 CS00 = v2_ld(cs_ + 2 * static_cast<std::size_t>(k00));
        const V2 CS10 = v2_ld(cs_ + 2 * static_cast<std::size_t>(k10));
        cell_fwd_pair_v(v, CS00);
        cell_fwd_pair_v(v + 4, v2_swap(CS00));   // (e2-d, 4e) = swap of (d, 4e)
        cell_fwd_pair_v(v + 8, CS10);
        cell_fwd_pair_v(v + 12, v2_swap(CS10));  // (e+d, 4e) = swap of (e-d, 4e)
#else
        cell_fwd_pair(v, k00);
        cell_fwd_pair(v + 4, phase_table_index(e2 - d, e4));
        cell_fwd_pair(v + 8, k10);
        cell_fwd_pair(v + 12, phase_table_index(e + d, e4));
#endif
    }

    STXLIB_FFT_INLINE void span8_inv(double* __restrict v, int d, int e) const {
        const int e2 = e << 1;
        const int e4 = e2 << 1;
        const int k0 = phase_table_index(d, e);
        const int kl = phase_table_index(d, e2);
        const int k00 = phase_table_index(d, e4);
        const int k10 = phase_table_index(e - d, e4);
        const double hcl = 0.5 * cos_[static_cast<std::size_t>(kl)];
        const double hsl = 0.5 * sin_[static_cast<std::size_t>(kl)];
#if STXLIB_FFT_LEVEL >= 1
        const V2 half = v2_set1(0.5);
        const V2 H00 = v2_mul(half, v2_ld(cs_ + 2 * static_cast<std::size_t>(k00)));
        const V2 H10 = v2_mul(half, v2_ld(cs_ + 2 * static_cast<std::size_t>(k10)));
        cell_inv_pair_v(v, H00, half);
        cell_inv_pair_v(v + 4, v2_swap(H00), half);
        cell_inv_pair_v(v + 8, H10, half);
        cell_inv_pair_v(v + 12, v2_swap(H10), half);
#else
        cell_inv_pair(v, k00);
        cell_inv_pair(v + 4, phase_table_index(e2 - d, e4));
        cell_inv_pair(v + 8, k10);
        cell_inv_pair(v + 12, phase_table_index(e + d, e4));
#endif
        cell_inv_ip(v, 2, hcl, hsl);
        cell_inv_ip(v + 8, 2, hsl, hcl);    // swap sibling
        cell_inv_ip(v, 4, 0.5 * cos_[static_cast<std::size_t>(k0)], 0.5 * sin_[static_cast<std::size_t>(k0)]);
    }

    void fwd_span(double* __restrict v, int w, int d, int e) const {
        if (w >= 32) {
            // radix-4 step: fused two-level cell, recurse into grandchildren.
            const int w4 = w >> 2;
            const int e2 = e << 1;
            const int e4 = e2 << 1;
            const int k0 = phase_table_index(d, e);
            const int kl = phase_table_index(d, e2);
            const double cl = cos_[static_cast<std::size_t>(kl)];
            const double sl = sin_[static_cast<std::size_t>(kl)];
            // theta(e-d, 2e) is the complement: (ch, sh) = (sl, cl) exactly
            cell4_fwd_ip(v, w4,
                         cos_[static_cast<std::size_t>(k0)], sin_[static_cast<std::size_t>(k0)],
                         cl, sl, sl, cl);
            fwd_span(v, w4, d, e4);
            fwd_span(v + (w >> 1), w4, e2 - d, e4);
            fwd_span(v + w, w4, e - d, e4);
            fwd_span(v + w + (w >> 1), w4, e + d, e4);
            return;
        }
        if (w == 16) {
            const int k = phase_table_index(d, e);
            cell_fwd_ip(v, 8, cos_[static_cast<std::size_t>(k)],
                        sin_[static_cast<std::size_t>(k)]);
            const int e2 = e << 1;
            span8_fwd(v, d, e2);
            span8_fwd(v + 16, e - d, e2);
            return;
        }
        if (w == 8) {
            span8_fwd(v, d, e);
            return;
        }
        if (w == 1) {
            return;
        }
        if (w == 2) {
            cell_fwd_pair(v, phase_table_index(d, e));
            return;
        }
        const int w2 = w >> 1;
        const int k = phase_table_index(d, e);
        cell_fwd_ip(v, w2, cos_[static_cast<std::size_t>(k)],
                    sin_[static_cast<std::size_t>(k)]);
        const int e2 = e << 1;
        fwd_span(v, w2, d, e2);
        fwd_span(v + w, w2, e - d, e2);
    }

    void fwd_ridge(double* __restrict v, int q, int e,
                   Complex64* __restrict out) const {
        if (q == 1) {
            return;    // dc/ny final at v[0], v[1]; the driver emits them
        }
        const int q2 = q >> 1;
        binom_fwd_ip(v, q2);
        const int e2 = e << 1;
        fwd_ridge(v, q2, e2, out);
        fwd_tree(v + q, q2, e >> 1, e2, out);   // promoted bin (e/2, 2e), free
    }

    // Pure in-place ascent; leaves already sit in the span (comb ingress).
    void inv_span(double* __restrict v, int w, int d, int e) const {
        if (w >= 32) {
            // radix-4 step: ascend grandchildren, then fused two-level cell.
            const int w4 = w >> 2;
            const int e2 = e << 1;
            const int e4 = e2 << 1;
            inv_span(v, w4, d, e4);
            inv_span(v + (w >> 1), w4, e2 - d, e4);
            inv_span(v + w, w4, e - d, e4);
            inv_span(v + w + (w >> 1), w4, e + d, e4);
            const int k0 = phase_table_index(d, e);
            const int kl = phase_table_index(d, e2);
            const double hcl = 0.5 * cos_[static_cast<std::size_t>(kl)];
            const double hsl = 0.5 * sin_[static_cast<std::size_t>(kl)];
            cell4_inv_ip(v, w4,
                         0.5 * cos_[static_cast<std::size_t>(k0)], 0.5 * sin_[static_cast<std::size_t>(k0)],
                         hcl, hsl, hsl, hcl);
            return;
        }
        if (w == 16) {
            const int e2 = e << 1;
            span8_inv(v, d, e2);
            span8_inv(v + 16, e - d, e2);
            const int k = phase_table_index(d, e);
            cell_inv_ip(v, 8, 0.5 * cos_[static_cast<std::size_t>(k)],
                        0.5 * sin_[static_cast<std::size_t>(k)]);
            return;
        }
        if (w == 8) {
            span8_inv(v, d, e);
            return;
        }
        if (w == 1) {
            return;
        }
        if (w == 2) {
            cell_inv_pair(v, phase_table_index(d, e));
            return;
        }
        const int w2 = w >> 1;
        const int e2 = e << 1;
        inv_span(v, w2, d, e2);
        inv_span(v + w, w2, e - d, e2);
        const int k = phase_table_index(d, e);
        cell_inv_ip(v, w2, 0.5 * cos_[static_cast<std::size_t>(k)],
                    0.5 * sin_[static_cast<std::size_t>(k)]);
    }

    void inv_ridge(double* __restrict v, int q, int e,
                   const Complex64* __restrict in) const {
        if (q == 1) {
            v[0] = in[0].re;
            v[1] = in[half_].re;
            return;
        }
        const int q2 = q >> 1;
        const int e2 = e << 1;
        inv_ridge(v, q2, e2, in);
        inv_tree(v + q, q2, e >> 1, e2, in);
        binom_inv_ip(v, q2);
    }

    // -----------------------------------------------------------------------
    // Subtree drivers with the comb boundary. A subtree (d, e) of row width
    // w <= kEgressW owns the bin combs {j*e +/- d}; its span is cache-
    // resident, so the boundary permutation T_w runs entirely in L1 while
    // the bins move as two monotonic strided streams.
    // -----------------------------------------------------------------------

    static constexpr int kEgressW = dip_egress_width;   // span = 2w doubles = 64 KiB

    const int* tperm(int w) const noexcept { return tperm_ + (w - 1); }

    // rank r of a subtree leaf -> bin (m*e + s*d with m = (r+1)/2, s = +/-).
    static STXLIB_FFT_INLINE int rank_bin(int r, int d, int e) noexcept {
        const int m = (r + 1) >> 1;
        return (r & 1) ? m * e - d : m * e + d;
    }

    // The leaf pair (a, b) and the bin (re, im) are both contiguous 16-byte
    // pairs differing only by the sign of the second lane, so each boundary
    // move is one vector load + NEGHI + one vector store -- no scalar moves,
    // no scalar flops.
    void egress_span(const double* __restrict v, int w, int d, int e,
                     Complex64* __restrict out) const {
        const int* __restrict T = tperm(w);
        for (int r = 0; r < w; ++r) {
            const int p = 2 * T[r];
            const int bin = rank_bin(r, d, e);
#if STXLIB_FFT_LEVEL >= 1
            v2_st(&out[bin].re, v2_neghi(v2_ld(v + p)));
#else
            out[bin].re = v[p];
            out[bin].im = -v[p + 1];
#endif
        }
    }

    void ingress_span(double* __restrict v, int w, int d, int e,
                      const Complex64* __restrict in) const {
        const int* __restrict T = tperm(w);
        for (int r = 0; r < w; ++r) {
            const int p = 2 * T[r];
            const int bin = rank_bin(r, d, e);
#if STXLIB_FFT_LEVEL >= 1
            v2_st(v + p, v2_neghi(v2_ld(&in[bin].re)));
#else
            v[p] = in[bin].re;
            v[p + 1] = -in[bin].im;
#endif
        }
    }

    void fwd_tree(double* __restrict v, int w, int d, int e,
                  Complex64* __restrict out) const {
        if (w > 2 * kEgressW) {
            // radix-4 descent above the boundary granularity.
            const int w4 = w >> 2;
            const int e2 = e << 1;
            const int e4 = e2 << 1;
            const int k0 = phase_table_index(d, e);
            const int kl = phase_table_index(d, e2);
            const double cl = cos_[static_cast<std::size_t>(kl)];
            const double sl = sin_[static_cast<std::size_t>(kl)];
            // theta(e-d, 2e) is the complement: (ch, sh) = (sl, cl) exactly
            cell4_fwd_ip(v, w4,
                         cos_[static_cast<std::size_t>(k0)], sin_[static_cast<std::size_t>(k0)],
                         cl, sl, sl, cl);
            fwd_tree(v, w4, d, e4, out);
            fwd_tree(v + (w >> 1), w4, e2 - d, e4, out);
            fwd_tree(v + w, w4, e - d, e4, out);
            fwd_tree(v + w + (w >> 1), w4, e + d, e4, out);
            return;
        }
        if (w == 2 * kEgressW) {
            // radix-2 parity step so the boundary always lands on kEgressW
            // spans (densest bin combs the L1 budget allows).
            const int w2 = w >> 1;
            const int e2 = e << 1;
            const int k = phase_table_index(d, e);
            cell_fwd_ip(v, w2, cos_[static_cast<std::size_t>(k)],
                        sin_[static_cast<std::size_t>(k)]);
            fwd_tree(v, w2, d, e2, out);
            fwd_tree(v + w, w2, e - d, e2, out);
            return;
        }
        fwd_span(v, w, d, e);
        egress_span(v, w, d, e, out);
    }

    void inv_tree(double* __restrict v, int w, int d, int e,
                  const Complex64* __restrict in) const {
        // No parity step on the inverse (unlike fwd_tree): comb READS
        // prefetch fine at any density, and the ingress scatter prefers the
        // smaller radix-4 landing span (a radix-2 step here measured ~2x
        // slower at 64K).
        if (w > kEgressW) {
            const int w4 = w >> 2;
            const int e2 = e << 1;
            const int e4 = e2 << 1;
            inv_tree(v, w4, d, e4, in);
            inv_tree(v + (w >> 1), w4, e2 - d, e4, in);
            inv_tree(v + w, w4, e - d, e4, in);
            inv_tree(v + w + (w >> 1), w4, e + d, e4, in);
            const int k0 = phase_table_index(d, e);
            const int kl = phase_table_index(d, e2);
            const double hcl = 0.5 * cos_[static_cast<std::size_t>(kl)];
            const double hsl = 0.5 * sin_[static_cast<std::size_t>(kl)];
            cell4_inv_ip(v, w4,
                         0.5 * cos_[static_cast<std::size_t>(k0)], 0.5 * sin_[static_cast<std::size_t>(k0)],
                         hcl, hsl, hsl, hcl);
            return;
        }
        ingress_span(v, w, d, e, in);
        inv_span(v, w, d, e);
    }


    // -----------------------------------------------------------------------
    // Seeds: one streaming pass, comb reads -> contiguous slot-row writes.
    // Slot order is the tree (descent) order of the walk above.
    // -----------------------------------------------------------------------

    void forward_seed4(const double* __restrict input, double* __restrict dst) const {
        const int q = n_ >> 2;
        // level-4 slots: [ dc | ny | a1 | b1 ]
        double* __restrict r0 = dst;
        double* __restrict r1 = dst + q;
        double* __restrict r2 = dst + 2 * static_cast<std::size_t>(q);
        double* __restrict r3 = dst + 3 * static_cast<std::size_t>(q);
        const double* __restrict x0 = input;
        const double* __restrict x1 = input + q;
        const double* __restrict x2 = input + 2 * static_cast<std::size_t>(q);
        const double* __restrict x3 = input + 3 * static_cast<std::size_t>(q);
        for (int i = 0; i < q; ++i) {
            const double a = x0[i];
            const double b = x1[i];
            const double c = x2[i];
            const double d = x3[i];
            r0[i] = a + b + c + d;   // dc
            r1[i] = a - b + c - d;   // ny
            r2[i] = a - c;           // a1 = Re
            r3[i] = b - d;           // b1 = -Im
        }
    }

    // Two-tile column-blocked seed. The naive form runs 8 read + 8 write
    // streams whose power-of-two comb strides all alias one L1 set at
    // n >= 16K: 16 lines fighting 8 ways, each refetched ~4x before its
    // doubles are consumed (measured 3x the pass cost, and allocation-offset
    // dependent). Two stack tiles make the stream discipline deterministic:
    //   copy-in : one input stream at a time  -> tin   (<= 2 active streams)
    //   compute : tin -> tout, entirely L1-resident
    //   copy-out: tout -> one output stream at a time (<= 2 active streams)
    // No phase ever exceeds the associativity. Tiles are automatic storage
    // (thread-local); no shared scratch, concurrency preserved. Bit-identical
    // to the untiled seed; measured 0.60-0.87x the one-phase form.
    void forward_seed8(const double* __restrict input, double* __restrict dst) const {
        const int q = n_ >> 3;
        if (q < 2048) {
            // comb stride q*8B below the L1 set period (16 KiB): the streams
            // spread across sets, no aliasing -- the direct one-phase loop is
            // copy-free and faster here.
            forward_seed8_direct(input, dst);
            return;
        }
        // tout row order = (dc, ny, a1, b1, a2, b2, a3, b3); slot targets:
        double* __restrict rr[8] = {
            dst,                                      // dc
            dst + q,                                  // ny
            dst + 4 * static_cast<std::size_t>(q),    // a1
            dst + 5 * static_cast<std::size_t>(q),    // b1
            dst + 2 * static_cast<std::size_t>(q),    // a2
            dst + 3 * static_cast<std::size_t>(q),    // b2
            dst + 6 * static_cast<std::size_t>(q),    // a3
            dst + 7 * static_cast<std::size_t>(q)     // b3
        };
        constexpr double rt = 0.707106781186547524400844362104849039;
        constexpr int TB = 256;              // columns per tile block
        double tin[8 * TB];                  // 16 KiB each, stack
        double tout[8 * TB];
        for (int base = 0; base < q; base += TB) {
            const int cols = (q - base < TB) ? (q - base) : TB;
            for (int kk = 0; kk < 8; ++kk) {
                const double* __restrict src =
                    input + static_cast<std::size_t>(kk) * q + base;
                double* __restrict trow = tin + static_cast<std::size_t>(kk) * TB;
                for (int i = 0; i < cols; ++i) {
                    trow[i] = src[i];
                }
            }
            for (int i = 0; i < cols; ++i) {
                const double a = tin[i];
                const double b = tin[TB + i];
                const double c = tin[2 * TB + i];
                const double d = tin[3 * TB + i];
                const double e = tin[4 * TB + i];
                const double f = tin[5 * TB + i];
                const double g = tin[6 * TB + i];
                const double h = tin[7 * TB + i];

                const double ae = a - e;
                const double bf = b - f;
                const double cg = c - g;
                const double dh = d - h;
                const double rot_r = rt * (bf - dh);
                const double rot_i = rt * (bf + dh);

                tout[i] = a + b + c + d + e + f + g + h;        // dc
                tout[TB + i] = a - b + c - d + e - f + g - h;   // ny
                tout[2 * TB + i] = ae + rot_r;                  // a1
                tout[3 * TB + i] = cg + rot_i;                  // b1
                tout[4 * TB + i] = a - c + e - g;               // a2
                tout[5 * TB + i] = b - d + f - h;               // b2
                tout[6 * TB + i] = ae - rot_r;                  // a3
                tout[7 * TB + i] = rot_i - cg;                  // b3
            }
            for (int kk = 0; kk < 8; ++kk) {
                const double* __restrict trow = tout + static_cast<std::size_t>(kk) * TB;
                double* __restrict out = rr[kk] + base;
                for (int i = 0; i < cols; ++i) {
                    out[i] = trow[i];
                }
            }
        }
    }

    void forward_seed8_direct(const double* __restrict input, double* __restrict dst) const {
        const int q = n_ >> 3;
        // level-8 slots: [ dc | ny | a2 | b2 | a1 | b1 | a3 | b3 ]
        double* __restrict r0 = dst;                                    // dc
        double* __restrict r1 = dst + q;                                // ny
        double* __restrict r2 = dst + 4 * static_cast<std::size_t>(q);  // a1
        double* __restrict r3 = dst + 5 * static_cast<std::size_t>(q);  // b1
        double* __restrict r4 = dst + 2 * static_cast<std::size_t>(q);  // a2
        double* __restrict r5 = dst + 3 * static_cast<std::size_t>(q);  // b2
        double* __restrict r6 = dst + 6 * static_cast<std::size_t>(q);  // a3
        double* __restrict r7 = dst + 7 * static_cast<std::size_t>(q);  // b3
        const double* __restrict x0 = input;
        const double* __restrict x1 = input + q;
        const double* __restrict x2 = input + 2 * static_cast<std::size_t>(q);
        const double* __restrict x3 = input + 3 * static_cast<std::size_t>(q);
        const double* __restrict x4 = input + 4 * static_cast<std::size_t>(q);
        const double* __restrict x5 = input + 5 * static_cast<std::size_t>(q);
        const double* __restrict x6 = input + 6 * static_cast<std::size_t>(q);
        const double* __restrict x7 = input + 7 * static_cast<std::size_t>(q);
        constexpr double rt = 0.707106781186547524400844362104849039;
        for (int i = 0; i < q; ++i) {
            const double a = x0[i];
            const double b = x1[i];
            const double c = x2[i];
            const double d = x3[i];
            const double e = x4[i];
            const double f = x5[i];
            const double g = x6[i];
            const double h = x7[i];

            const double ae = a - e;
            const double bf = b - f;
            const double cg = c - g;
            const double dh = d - h;
            const double rot_r = rt * (bf - dh);
            const double rot_i = rt * (bf + dh);

            r0[i] = a + b + c + d + e + f + g + h;   // dc
            r1[i] = a - b + c - d + e - f + g - h;   // ny
            r2[i] = ae + rot_r;                      // a1
            r3[i] = cg + rot_i;                      // b1
            r4[i] = a - c + e - g;                   // a2
            r5[i] = b - d + f - h;                   // b2
            r6[i] = ae - rot_r;                      // a3
            r7[i] = rot_i - cg;                      // b3
        }
    }

    // -----------------------------------------------------------------------
    // Inverse tails: level-8 / level-4 slot state -> time, one streaming pass.
    // -----------------------------------------------------------------------

    void inverse_tail4(const double* __restrict src, double* __restrict output, int q) const {
        // level-4 slots: [ dc | ny | a1 | b1 ]; the tail below is written in
        // the (dc, a1, ny, b1) basis, so remap the reads.
        const double* __restrict r0 = src;                                    // dc
        const double* __restrict r1 = src + 2 * static_cast<std::size_t>(q);  // a1
        const double* __restrict r2 = src + q;                                // ny
        const double* __restrict r3 = src + 3 * static_cast<std::size_t>(q);  // b1
        double* __restrict x0 = output;
        double* __restrict x1 = output + q;
        double* __restrict x2 = output + 2 * static_cast<std::size_t>(q);
        double* __restrict x3 = output + 3 * static_cast<std::size_t>(q);
        for (int i = 0; i < q; ++i) {
            const double ac = 0.5 * (r0[i] + r2[i]);
            const double bd = 0.5 * (r0[i] - r2[i]);
            x0[i] = 0.5 * (ac + r1[i]);
            x2[i] = 0.5 * (ac - r1[i]);
            x1[i] = 0.5 * (bd + r3[i]);
            x3[i] = 0.5 * (bd - r3[i]);
        }
    }

    void inverse_tail8(const double* __restrict src, double* __restrict output, int q) const {
        // level-8 slots: [ dc | ny | a2 b2 | a1 b1 | a3 b3 ]; the tail below is
        // written in the (dc, a1, a2, a3, ny, b3, b2, b1) basis, so remap.
        const double* __restrict r0 = src;                                    // dc
        const double* __restrict r1 = src + 4 * static_cast<std::size_t>(q);  // a1
        const double* __restrict r2 = src + 2 * static_cast<std::size_t>(q);  // a2
        const double* __restrict r3 = src + 6 * static_cast<std::size_t>(q);  // a3
        const double* __restrict r4 = src + q;                                // ny
        const double* __restrict r5 = src + 7 * static_cast<std::size_t>(q);  // b3
        const double* __restrict r6 = src + 3 * static_cast<std::size_t>(q);  // b2
        const double* __restrict r7 = src + 5 * static_cast<std::size_t>(q);  // b1
        double* __restrict x0 = output;
        double* __restrict x1 = output + q;
        double* __restrict x2 = output + 2 * static_cast<std::size_t>(q);
        double* __restrict x3 = output + 3 * static_cast<std::size_t>(q);
        double* __restrict x4 = output + 4 * static_cast<std::size_t>(q);
        double* __restrict x5 = output + 5 * static_cast<std::size_t>(q);
        double* __restrict x6 = output + 6 * static_cast<std::size_t>(q);
        double* __restrict x7 = output + 7 * static_cast<std::size_t>(q);
        constexpr double inv_2rt = 0.707106781186547524400844362104849039;

        for (int i = 0; i < q; ++i) {
            const double even_sum = 0.5 * (r0[i] + r4[i]);
            const double odd_sum = 0.5 * (r0[i] - r4[i]);
            const double ae = 0.5 * (r1[i] + r3[i]);
            const double cg = 0.5 * (r7[i] - r5[i]);
            const double bf_minus_dh = inv_2rt * (r1[i] - r3[i]);
            const double bf_plus_dh = inv_2rt * (r5[i] + r7[i]);
            const double bf = 0.5 * (bf_minus_dh + bf_plus_dh);
            const double dh = 0.5 * (bf_plus_dh - bf_minus_dh);

            const double ae_sum = 0.5 * (even_sum + r2[i]);
            const double cg_sum = 0.5 * (even_sum - r2[i]);
            const double bf_sum = 0.5 * (odd_sum + r6[i]);
            const double dh_sum = 0.5 * (odd_sum - r6[i]);

            x0[i] = 0.5 * (ae_sum + ae);
            x4[i] = 0.5 * (ae_sum - ae);
            x2[i] = 0.5 * (cg_sum + cg);
            x6[i] = 0.5 * (cg_sum - cg);
            x1[i] = 0.5 * (bf_sum + bf);
            x5[i] = 0.5 * (bf_sum - bf);
            x3[i] = 0.5 * (dh_sum + dh);
            x7[i] = 0.5 * (dh_sum - dh);
        }
    }


    int n_;
    int half_;
    const double* cos_;
    const double* sin_;
    const double* cs_;     // interleaved [cos, sin] pairs (leaf cells)
    const int* tperm_;
};
