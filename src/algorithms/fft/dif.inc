// stx_fft's plain mode: BFFT's DIF kernel (src/detail/bruun_dif_kernel.hpp
// at edf5f01, MIT, the owner's own; NOTICE.md), the residue engine for both
// precisions. Included only by variant.inc, inside one SIMD tier's
// namespace; the tier is STXLIB_FFT_LEVEL (BFFT's BRUUN_LEVEL).
//
// Copied, not redesigned (brief, step 2). The streaming kernels (binomial
// splits, normalised-quadratic rotations, their two-level fusions) and the
// residue walks are BFFT's text, with BFFT's names, so a diff against BFFT
// shows only these changes, made by a mechanical transform
// (dev xf/transform.pl):
//   - every fused multiply-add is spelled as the multiply and the add or
//     subtract it stands for, in BFFT's operand order (A1 rule 1), so the
//     vector paths round as the scalar paths do;
//   - BFFT's vector macros are lanes.inc's functions (V2_ADD is v2_add);
//   - BRUUN_LEVEL is STXLIB_FFT_LEVEL, RESTRICT is __restrict.
// What the port leaves out: BFFT's native (heap-optimised) output order,
// its fused scatter codelets for the float64 forward, its precomputed op
// schedules and its magnitude/phase front end. They reorder or fuse the
// same arithmetic; this file keeps the residue engine BFFT uses for its
// float32 forward, its large float64 forward ("two-phase") and both
// inverses, at every size.
//
// Layout. A forward fills the work buffer with residues: leaf m holds
// (v[2m], v[2m+1]) = (Re X[IDX[m]], -Im X[IDX[m]]), and v[0] +- v[1] are DC
// and Nyquist. The pack then writes X in bin order through KINV. The
// inverse unpacks X into the output buffer through IDX and runs the walk
// backwards there.

// ---------------------------------------------------------------------------
// Streaming kernels. Each has an optional 256 block,
// a 2-lane block for the 128-bit backend, and an exact scalar tail.
// ---------------------------------------------------------------------------


static inline void binomial_fwd(double* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    for (; i + 3 < h; i += 4) {
        const V4D a = v4d_ld(v + i);
        const V4D b = v4d_ld(v + h + i);
        v4d_st(v + i, v4d_add(a, b));
        v4d_st(v + h + i, v4d_sub(a, b));
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    for (; i + 1 < h; i += 2) {
        const V2 a = v2_ld(v + i);
        const V2 b = v2_ld(v + h + i);
        v2_st(v + i, v2_add(a, b));
        v2_st(v + h + i, v2_sub(a, b));
    }
#endif

    for (; i < h; ++i) {
        const double a = v[i];
        const double b = v[h + i];
        v[i] = a + b;
        v[h + i] = a - b;
    }
}

// Fused "copy input + first binomial split": v[i] = in[i] + in[h+i], v[h+i] = in[i] - in[h+i].
static inline void binomial_oop(const double* __restrict in, double* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    for (; i + 3 < h; i += 4) {
        const V4D a = v4d_ld(in + i);
        const V4D b = v4d_ld(in + h + i);
        v4d_st(v + i, v4d_add(a, b));
        v4d_st(v + h + i, v4d_sub(a, b));
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    for (; i + 1 < h; i += 2) {
        const V2 a = v2_ld(in + i);
        const V2 b = v2_ld(in + h + i);
        v2_st(v + i, v2_add(a, b));
        v2_st(v + h + i, v2_sub(a, b));
    }
#endif

    for (; i < h; ++i) {
        const double a = in[i];
        const double b = in[h + i];
        v[i] = a + b;
        v[h + i] = a - b;
    }
}

static inline void binomial_inv(double* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    const V4D half4 = v4d_set1(0.5);
    for (; i + 3 < h; i += 4) {
        const V4D a = v4d_ld(v + i);
        const V4D b = v4d_ld(v + h + i);
        v4d_st(v + i, v4d_mul(half4, v4d_add(a, b)));
        v4d_st(v + h + i, v4d_mul(half4, v4d_sub(a, b)));
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    const V2 half2 = v2_set1(0.5);
    for (; i + 1 < h; i += 2) {
        const V2 a = v2_ld(v + i);
        const V2 b = v2_ld(v + h + i);
        v2_st(v + i, v2_mul(half2, v2_add(a, b)));
        v2_st(v + h + i, v2_mul(half2, v2_sub(a, b)));
    }
#endif

    for (; i < h; ++i) {
        const double a = v[i];
        const double b = v[h + i];
        v[i] = 0.5 * (a + b);
        v[h + i] = 0.5 * (a - b);
    }
}

// One normalized-quadratic split: block [A0|B0|A1|B1] of quarters q.
static inline void norm_q_fwd(double* __restrict p, int q, double c_scalar, double s_scalar) {
    double* __restrict A0p = p;
    double* __restrict B0p = p + q;
    double* __restrict A1p = p + 2*q;
    double* __restrict B1p = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const V4D vc = v4d_set1(c_scalar);
        const V4D vs = v4d_set1(s_scalar);

        for (; n + 3 < q; n += 4) {
            const V4D A0 = v4d_ld(A0p + n);
            const V4D B0 = v4d_ld(B0p + n);
            const V4D A1 = v4d_ld(A1p + n);
            const V4D B1 = v4d_ld(B1p + n);

            const V4D R = v4d_sub(v4d_mul(vc, B0), v4d_mul(vs, B1));
            const V4D I = v4d_add(v4d_mul(vs, B0), v4d_mul(vc, B1));

            v4d_st(A0p + n, v4d_add(A0, R));
            v4d_st(B0p + n, v4d_add(A1, I));
            v4d_st(A1p + n, v4d_sub(A0, R));
            v4d_st(B1p + n, v4d_sub(I, A1));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V2 vc = v2_set1(c_scalar);
        const V2 vs = v2_set1(s_scalar);

        for (; n + 1 < q; n += 2) {
            const V2 A0 = v2_ld(A0p + n);
            const V2 B0 = v2_ld(B0p + n);
            const V2 A1 = v2_ld(A1p + n);
            const V2 B1 = v2_ld(B1p + n);

            const V2 R = v2_sub(v2_mul(vc, B0), v2_mul(vs, B1));
            const V2 I = v2_add(v2_mul(vs, B0), v2_mul(vc, B1));

            v2_st(A0p + n, v2_add(A0, R));
            v2_st(B0p + n, v2_add(A1, I));
            v2_st(A1p + n, v2_sub(A0, R));
            v2_st(B1p + n, v2_sub(I, A1));
        }
    }
#endif

    for (; n < q; ++n) {
        const double A0 = A0p[n];
        const double B0 = B0p[n];
        const double A1 = A1p[n];
        const double B1 = B1p[n];

        const double R = c_scalar * B0 - s_scalar * B1;
        const double I = s_scalar * B0 + c_scalar * B1;

        A0p[n] = A0 + R;
        B0p[n] = A1 + I;
        A1p[n] = A0 - R;
        B1p[n] = -A1 + I;
    }
}

// Two tree levels in one pass: parent rotation (c,s) plus both child rotations
// (c0,s0), (c1,s1) applied while the data is in registers. Halves the load/store
// traffic of the norm cascade. valid for q2 spine fusion
static inline void norm2_fused(double* __restrict p, int q,
                               double c, double s,
                               double c0, double s0,
                               double c1, double s1) {
    const int qh = q >> 1;
    double* __restrict A0 = p;
    double* __restrict B0 = p + q;
    double* __restrict A1 = p + 2*q;
    double* __restrict B1 = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const V4D vc  = v4d_set1(c),  vs  = v4d_set1(s);
        const V4D vc0 = v4d_set1(c0), vs0 = v4d_set1(s0);
        const V4D vc1 = v4d_set1(c1), vs1 = v4d_set1(s1);

        for (; n + 3 < qh; n += 4) {
            const V4D a0n = v4d_ld(A0 + n);
            const V4D a0h = v4d_ld(A0 + qh + n);
            const V4D b0n = v4d_ld(B0 + n);
            const V4D b0h = v4d_ld(B0 + qh + n);
            const V4D a1n = v4d_ld(A1 + n);
            const V4D a1h = v4d_ld(A1 + qh + n);
            const V4D b1n = v4d_ld(B1 + n);
            const V4D b1h = v4d_ld(B1 + qh + n);

            const V4D Rn = v4d_sub(v4d_mul(vc, b0n), v4d_mul(vs, b1n));
            const V4D In = v4d_add(v4d_mul(vs, b0n), v4d_mul(vc, b1n));
            const V4D Rh = v4d_sub(v4d_mul(vc, b0h), v4d_mul(vs, b1h));
            const V4D Ih = v4d_add(v4d_mul(vs, b0h), v4d_mul(vc, b1h));

            const V4D u0 = v4d_add(a0n, Rn);
            const V4D uh = v4d_add(a0h, Rh);
            const V4D w0 = v4d_add(a1n, In);
            const V4D wh = v4d_add(a1h, Ih);
            const V4D v0 = v4d_sub(a0n, Rn);
            const V4D vh = v4d_sub(a0h, Rh);
            const V4D x0 = v4d_sub(In, a1n);
            const V4D xh = v4d_sub(Ih, a1h);

            const V4D R0 = v4d_sub(v4d_mul(vc0, uh), v4d_mul(vs0, wh));
            const V4D I0 = v4d_add(v4d_mul(vs0, uh), v4d_mul(vc0, wh));
            const V4D R1 = v4d_sub(v4d_mul(vc1, vh), v4d_mul(vs1, xh));
            const V4D I1 = v4d_add(v4d_mul(vs1, vh), v4d_mul(vc1, xh));

            v4d_st(A0 + n,      v4d_add(u0, R0));
            v4d_st(A0 + qh + n, v4d_add(w0, I0));
            v4d_st(B0 + n,      v4d_sub(u0, R0));
            v4d_st(B0 + qh + n, v4d_sub(I0, w0));
            v4d_st(A1 + n,      v4d_add(v0, R1));
            v4d_st(A1 + qh + n, v4d_add(x0, I1));
            v4d_st(B1 + n,      v4d_sub(v0, R1));
            v4d_st(B1 + qh + n, v4d_sub(I1, x0));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V2 vc  = v2_set1(c),  vs  = v2_set1(s);
        const V2 vc0 = v2_set1(c0), vs0 = v2_set1(s0);
        const V2 vc1 = v2_set1(c1), vs1 = v2_set1(s1);

        for (; n + 1 < qh; n += 2) {
            const V2 a0n = v2_ld(A0 + n);
            const V2 a0h = v2_ld(A0 + qh + n);
            const V2 b0n = v2_ld(B0 + n);
            const V2 b0h = v2_ld(B0 + qh + n);
            const V2 a1n = v2_ld(A1 + n);
            const V2 a1h = v2_ld(A1 + qh + n);
            const V2 b1n = v2_ld(B1 + n);
            const V2 b1h = v2_ld(B1 + qh + n);

            const V2 Rn = v2_sub(v2_mul(vc, b0n), v2_mul(vs, b1n));
            const V2 In = v2_add(v2_mul(vs, b0n), v2_mul(vc, b1n));
            const V2 Rh = v2_sub(v2_mul(vc, b0h), v2_mul(vs, b1h));
            const V2 Ih = v2_add(v2_mul(vs, b0h), v2_mul(vc, b1h));

            const V2 u0 = v2_add(a0n, Rn);
            const V2 uh = v2_add(a0h, Rh);
            const V2 w0 = v2_add(a1n, In);
            const V2 wh = v2_add(a1h, Ih);
            const V2 v0 = v2_sub(a0n, Rn);
            const V2 vh = v2_sub(a0h, Rh);
            const V2 x0 = v2_sub(In, a1n);
            const V2 xh = v2_sub(Ih, a1h);

            const V2 R0 = v2_sub(v2_mul(vc0, uh), v2_mul(vs0, wh));
            const V2 I0 = v2_add(v2_mul(vs0, uh), v2_mul(vc0, wh));
            const V2 R1 = v2_sub(v2_mul(vc1, vh), v2_mul(vs1, xh));
            const V2 I1 = v2_add(v2_mul(vs1, vh), v2_mul(vc1, xh));

            v2_st(A0 + n,      v2_add(u0, R0));
            v2_st(A0 + qh + n, v2_add(w0, I0));
            v2_st(B0 + n,      v2_sub(u0, R0));
            v2_st(B0 + qh + n, v2_sub(I0, w0));
            v2_st(A1 + n,      v2_add(v0, R1));
            v2_st(A1 + qh + n, v2_add(x0, I1));
            v2_st(B1 + n,      v2_sub(v0, R1));
            v2_st(B1 + qh + n, v2_sub(I1, x0));
        }
    }
#endif

    for (; n < qh; ++n) {
        const double a0n = A0[n],      a0h = A0[qh + n];
        const double b0n = B0[n],      b0h = B0[qh + n];
        const double a1n = A1[n],      a1h = A1[qh + n];
        const double b1n = B1[n],      b1h = B1[qh + n];

        const double Rn = c * b0n - s * b1n;
        const double In = s * b0n + c * b1n;
        const double Rh = c * b0h - s * b1h;
        const double Ih = s * b0h + c * b1h;

        const double u0 = a0n + Rn, uh = a0h + Rh;
        const double w0 = a1n + In, wh = a1h + Ih;
        const double v0 = a0n - Rn, vh = a0h - Rh;
        const double x0 = In - a1n, xh = Ih - a1h;

        const double R0 = c0 * uh - s0 * wh;
        const double I0 = s0 * uh + c0 * wh;
        const double R1 = c1 * vh - s1 * xh;
        const double I1 = s1 * vh + c1 * xh;

        A0[n] = u0 + R0;
        A0[qh + n] = w0 + I0;
        B0[n] = u0 - R0;
        B0[qh + n] = I0 - w0;
        A1[n] = v0 + R1;
        A1[qh + n] = x0 + I1;
        B1[n] = v0 - R1;
        B1[qh + n] = I1 - x0;
    }
}

static inline void norm_q_inv(double* __restrict p, int q, double c_scalar, double s_scalar) {
    double* __restrict C0p = p;
    double* __restrict C1p = p + q;
    double* __restrict D0p = p + 2*q;
    double* __restrict D1p = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const V4D half = v4d_set1(0.5);
        const V4D vc = v4d_set1(c_scalar);
        const V4D vs = v4d_set1(s_scalar);

        for (; n + 3 < q; n += 4) {
            const V4D C0v = v4d_ld(C0p + n);
            const V4D C1v = v4d_ld(C1p + n);
            const V4D D0v = v4d_ld(D0p + n);
            const V4D D1v = v4d_ld(D1p + n);

            const V4D A0 = v4d_mul(half, v4d_add(C0v, D0v));
            const V4D R  = v4d_mul(half, v4d_sub(C0v, D0v));
            const V4D I  = v4d_mul(half, v4d_add(C1v, D1v));
            const V4D A1 = v4d_mul(half, v4d_sub(C1v, D1v));

            const V4D B0 = v4d_add(v4d_mul(vc, R), v4d_mul(vs, I));
            const V4D B1 = v4d_sub(v4d_mul(vc, I), v4d_mul(vs, R));

            v4d_st(C0p + n, A0);
            v4d_st(C1p + n, B0);
            v4d_st(D0p + n, A1);
            v4d_st(D1p + n, B1);
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V2 half = v2_set1(0.5);
        const V2 vc = v2_set1(c_scalar);
        const V2 vs = v2_set1(s_scalar);

        for (; n + 1 < q; n += 2) {
            const V2 C0v = v2_ld(C0p + n);
            const V2 C1v = v2_ld(C1p + n);
            const V2 D0v = v2_ld(D0p + n);
            const V2 D1v = v2_ld(D1p + n);

            const V2 A0 = v2_mul(half, v2_add(C0v, D0v));
            const V2 R  = v2_mul(half, v2_sub(C0v, D0v));
            const V2 I  = v2_mul(half, v2_add(C1v, D1v));
            const V2 A1 = v2_mul(half, v2_sub(C1v, D1v));

            const V2 B0 = v2_add(v2_mul(vc, R), v2_mul(vs, I));
            const V2 B1 = v2_sub(v2_mul(vc, I), v2_mul(vs, R));

            v2_st(C0p + n, A0);
            v2_st(C1p + n, B0);
            v2_st(D0p + n, A1);
            v2_st(D1p + n, B1);
        }
    }
#endif

    for (; n < q; ++n) {
        const double C0v = C0p[n];
        const double C1v = C1p[n];
        const double D0v = D0p[n];
        const double D1v = D1p[n];

        const double A0 = 0.5 * (C0v + D0v);
        const double R  = 0.5 * (C0v - D0v);
        const double I  = 0.5 * (C1v + D1v);
        const double A1 = 0.5 * (C1v - D1v);

        C0p[n] = A0;
        C1p[n] = c_scalar * R + s_scalar * I;
        D0p[n] = A1;
        D1p[n] = c_scalar * I - s_scalar * R;
    }
}

// Exact inverse of norm2_fused: undo both child rotations and the parent
// rotation in one pass over the block. Caller guarantees q >= 16 so qh >= 8.
static inline void norm2_inv_fused(double* __restrict p, int q,
                                   double c, double s,
                                   double c0, double s0,
                                   double c1, double s1) {
    const int qh = q >> 1;
    double* __restrict A0 = p;
    double* __restrict B0 = p + q;
    double* __restrict A1 = p + 2*q;
    double* __restrict B1 = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const V4D hf  = v4d_set1(0.5);
        const V4D vc  = v4d_set1(c),  vs  = v4d_set1(s);
        const V4D vc0 = v4d_set1(c0), vs0 = v4d_set1(s0);
        const V4D vc1 = v4d_set1(c1), vs1 = v4d_set1(s1);

        for (; n + 3 < qh; n += 4) {
            const V4D A0n = v4d_ld(A0 + n);
            const V4D A0h = v4d_ld(A0 + qh + n);
            const V4D B0n = v4d_ld(B0 + n);
            const V4D B0h = v4d_ld(B0 + qh + n);
            const V4D A1n = v4d_ld(A1 + n);
            const V4D A1h = v4d_ld(A1 + qh + n);
            const V4D B1n = v4d_ld(B1 + n);
            const V4D B1h = v4d_ld(B1 + qh + n);

            const V4D u0 = v4d_mul(hf, v4d_add(A0n, B0n));
            const V4D R0 = v4d_mul(hf, v4d_sub(A0n, B0n));
            const V4D I0 = v4d_mul(hf, v4d_add(A0h, B0h));
            const V4D w0 = v4d_mul(hf, v4d_sub(A0h, B0h));
            const V4D uh = v4d_add(v4d_mul(vc0, R0), v4d_mul(vs0, I0));
            const V4D wh = v4d_sub(v4d_mul(vc0, I0), v4d_mul(vs0, R0));

            const V4D v0 = v4d_mul(hf, v4d_add(A1n, B1n));
            const V4D R1 = v4d_mul(hf, v4d_sub(A1n, B1n));
            const V4D I1 = v4d_mul(hf, v4d_add(A1h, B1h));
            const V4D x0 = v4d_mul(hf, v4d_sub(A1h, B1h));
            const V4D vh = v4d_add(v4d_mul(vc1, R1), v4d_mul(vs1, I1));
            const V4D xh = v4d_sub(v4d_mul(vc1, I1), v4d_mul(vs1, R1));

            const V4D a0n = v4d_mul(hf, v4d_add(u0, v0));
            const V4D Rn  = v4d_mul(hf, v4d_sub(u0, v0));
            const V4D In  = v4d_mul(hf, v4d_add(w0, x0));
            const V4D a1n = v4d_mul(hf, v4d_sub(w0, x0));
            const V4D a0h = v4d_mul(hf, v4d_add(uh, vh));
            const V4D Rh  = v4d_mul(hf, v4d_sub(uh, vh));
            const V4D Ih  = v4d_mul(hf, v4d_add(wh, xh));
            const V4D a1h = v4d_mul(hf, v4d_sub(wh, xh));

            v4d_st(A0 + n,      a0n);
            v4d_st(A0 + qh + n, a0h);
            v4d_st(B0 + n,      v4d_add(v4d_mul(vc, Rn), v4d_mul(vs, In)));
            v4d_st(B0 + qh + n, v4d_add(v4d_mul(vc, Rh), v4d_mul(vs, Ih)));
            v4d_st(A1 + n,      a1n);
            v4d_st(A1 + qh + n, a1h);
            v4d_st(B1 + n,      v4d_sub(v4d_mul(vc, In), v4d_mul(vs, Rn)));
            v4d_st(B1 + qh + n, v4d_sub(v4d_mul(vc, Ih), v4d_mul(vs, Rh)));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V2 hf  = v2_set1(0.5);
        const V2 vc  = v2_set1(c),  vs  = v2_set1(s);
        const V2 vc0 = v2_set1(c0), vs0 = v2_set1(s0);
        const V2 vc1 = v2_set1(c1), vs1 = v2_set1(s1);

        for (; n + 1 < qh; n += 2) {
            const V2 A0n = v2_ld(A0 + n);
            const V2 A0h = v2_ld(A0 + qh + n);
            const V2 B0n = v2_ld(B0 + n);
            const V2 B0h = v2_ld(B0 + qh + n);
            const V2 A1n = v2_ld(A1 + n);
            const V2 A1h = v2_ld(A1 + qh + n);
            const V2 B1n = v2_ld(B1 + n);
            const V2 B1h = v2_ld(B1 + qh + n);

            const V2 u0 = v2_mul(hf, v2_add(A0n, B0n));
            const V2 R0 = v2_mul(hf, v2_sub(A0n, B0n));
            const V2 I0 = v2_mul(hf, v2_add(A0h, B0h));
            const V2 w0 = v2_mul(hf, v2_sub(A0h, B0h));
            const V2 uh = v2_add(v2_mul(vc0, R0), v2_mul(vs0, I0));
            const V2 wh = v2_sub(v2_mul(vc0, I0), v2_mul(vs0, R0));

            const V2 v0 = v2_mul(hf, v2_add(A1n, B1n));
            const V2 R1 = v2_mul(hf, v2_sub(A1n, B1n));
            const V2 I1 = v2_mul(hf, v2_add(A1h, B1h));
            const V2 x0 = v2_mul(hf, v2_sub(A1h, B1h));
            const V2 vh = v2_add(v2_mul(vc1, R1), v2_mul(vs1, I1));
            const V2 xh = v2_sub(v2_mul(vc1, I1), v2_mul(vs1, R1));

            const V2 a0n = v2_mul(hf, v2_add(u0, v0));
            const V2 Rn  = v2_mul(hf, v2_sub(u0, v0));
            const V2 In  = v2_mul(hf, v2_add(w0, x0));
            const V2 a1n = v2_mul(hf, v2_sub(w0, x0));
            const V2 a0h = v2_mul(hf, v2_add(uh, vh));
            const V2 Rh  = v2_mul(hf, v2_sub(uh, vh));
            const V2 Ih  = v2_mul(hf, v2_add(wh, xh));
            const V2 a1h = v2_mul(hf, v2_sub(wh, xh));

            v2_st(A0 + n,      a0n);
            v2_st(A0 + qh + n, a0h);
            v2_st(B0 + n,      v2_add(v2_mul(vc, Rn), v2_mul(vs, In)));
            v2_st(B0 + qh + n, v2_add(v2_mul(vc, Rh), v2_mul(vs, Ih)));
            v2_st(A1 + n,      a1n);
            v2_st(A1 + qh + n, a1h);
            v2_st(B1 + n,      v2_sub(v2_mul(vc, In), v2_mul(vs, Rn)));
            v2_st(B1 + qh + n, v2_sub(v2_mul(vc, Ih), v2_mul(vs, Rh)));
        }
    }
#endif

    for (; n < qh; ++n) {
        const double A0n = A0[n], A0h = A0[qh + n];
        const double B0n = B0[n], B0h = B0[qh + n];
        const double A1n = A1[n], A1h = A1[qh + n];
        const double B1n = B1[n], B1h = B1[qh + n];

        const double u0 = 0.5 * (A0n + B0n);
        const double R0 = 0.5 * (A0n - B0n);
        const double I0 = 0.5 * (A0h + B0h);
        const double w0 = 0.5 * (A0h - B0h);
        const double uh = c0 * R0 + s0 * I0;
        const double wh = c0 * I0 - s0 * R0;

        const double v0 = 0.5 * (A1n + B1n);
        const double R1 = 0.5 * (A1n - B1n);
        const double I1 = 0.5 * (A1h + B1h);
        const double x0 = 0.5 * (A1h - B1h);
        const double vh = c1 * R1 + s1 * I1;
        const double xh = c1 * I1 - s1 * R1;

        const double a0n = 0.5 * (u0 + v0);
        const double Rn  = 0.5 * (u0 - v0);
        const double In  = 0.5 * (w0 + x0);
        const double a1n = 0.5 * (w0 - x0);
        const double a0h = 0.5 * (uh + vh);
        const double Rh  = 0.5 * (uh - vh);
        const double Ih  = 0.5 * (wh + xh);
        const double a1h = 0.5 * (wh - xh);

        A0[n] = a0n;
        A0[qh + n] = a0h;
        B0[n] = c * Rn + s * In;
        B0[qh + n] = c * Rh + s * Ih;
        A1[n] = a1n;
        A1[qh + n] = a1h;
        B1[n] = c * In - s * Rn;
        B1[qh + n] = c * Ih - s * Rh;
    }
}

// ---------------------------------------------------------------------------
// Float32 streaming kernels. Same arithmetic as the double kernels above with
// twice the scalar lane count on supported SIMD paths: 8-wide AVX2,
// 4-wide SSE2/NEON, exact scalar tail.
// ---------------------------------------------------------------------------

static inline void binomial_fwd_f32(float* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    for (; i + 7 < h; i += 8) {
        const __m256 a = _mm256_loadu_ps(v + i);
        const __m256 b = _mm256_loadu_ps(v + h + i);
        _mm256_storeu_ps(v + i, _mm256_add_ps(a, b));
        _mm256_storeu_ps(v + h + i, _mm256_sub_ps(a, b));
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    for (; i + 3 < h; i += 4) {
        const V4F a = v4f_ld(v + i);
        const V4F b = v4f_ld(v + h + i);
        v4f_st(v + i, v4f_add(a, b));
        v4f_st(v + h + i, v4f_sub(a, b));
    }
#endif

    for (; i < h; ++i) {
        const float a = v[i];
        const float b = v[h + i];
        v[i] = a + b;
        v[h + i] = a - b;
    }
}

static inline void binomial_oop_f32(const float* __restrict in, float* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    for (; i + 7 < h; i += 8) {
        const __m256 a = _mm256_loadu_ps(in + i);
        const __m256 b = _mm256_loadu_ps(in + h + i);
        _mm256_storeu_ps(v + i, _mm256_add_ps(a, b));
        _mm256_storeu_ps(v + h + i, _mm256_sub_ps(a, b));
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    for (; i + 3 < h; i += 4) {
        const V4F a = v4f_ld(in + i);
        const V4F b = v4f_ld(in + h + i);
        v4f_st(v + i, v4f_add(a, b));
        v4f_st(v + h + i, v4f_sub(a, b));
    }
#endif

    for (; i < h; ++i) {
        const float a = in[i];
        const float b = in[h + i];
        v[i] = a + b;
        v[h + i] = a - b;
    }
}

static inline void binomial_inv_f32(float* __restrict v, int h) {
    int i = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const __m256 half8 = _mm256_set1_ps(0.5f);
        for (; i + 7 < h; i += 8) {
            const __m256 a = _mm256_loadu_ps(v + i);
            const __m256 b = _mm256_loadu_ps(v + h + i);
            _mm256_storeu_ps(v + i, _mm256_mul_ps(half8, _mm256_add_ps(a, b)));
            _mm256_storeu_ps(v + h + i, _mm256_mul_ps(half8, _mm256_sub_ps(a, b)));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V4F half4 = v4f_set1(0.5f);
        for (; i + 3 < h; i += 4) {
            const V4F a = v4f_ld(v + i);
            const V4F b = v4f_ld(v + h + i);
            v4f_st(v + i, v4f_mul(half4, v4f_add(a, b)));
            v4f_st(v + h + i, v4f_mul(half4, v4f_sub(a, b)));
        }
    }
#endif

    for (; i < h; ++i) {
        const float a = v[i];
        const float b = v[h + i];
        v[i] = 0.5f * (a + b);
        v[h + i] = 0.5f * (a - b);
    }
}

static inline void norm_q_fwd_f32(float* __restrict p, int q, float c_scalar, float s_scalar) {
    float* __restrict A0p = p;
    float* __restrict B0p = p + q;
    float* __restrict A1p = p + 2*q;
    float* __restrict B1p = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const __m256 vc = _mm256_set1_ps(c_scalar);
        const __m256 vs = _mm256_set1_ps(s_scalar);

        for (; n + 7 < q; n += 8) {
            const __m256 B0 = _mm256_loadu_ps(B0p + n);
            const __m256 B1 = _mm256_loadu_ps(B1p + n);
            const __m256 R = _mm256_sub_ps(_mm256_mul_ps(vc, B0), _mm256_mul_ps(vs, B1));
            const __m256 I = _mm256_add_ps(_mm256_mul_ps(vs, B0), _mm256_mul_ps(vc, B1));

            const __m256 A0 = _mm256_loadu_ps(A0p + n);
            const __m256 A1 = _mm256_loadu_ps(A1p + n);

            _mm256_storeu_ps(A0p + n, _mm256_add_ps(A0, R));
            _mm256_storeu_ps(B0p + n, _mm256_add_ps(A1, I));
            _mm256_storeu_ps(A1p + n, _mm256_sub_ps(A0, R));
            _mm256_storeu_ps(B1p + n, _mm256_sub_ps(I, A1));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V4F vc = v4f_set1(c_scalar);
        const V4F vs = v4f_set1(s_scalar);

        for (; n + 3 < q; n += 4) {
            const V4F B0 = v4f_ld(B0p + n);
            const V4F B1 = v4f_ld(B1p + n);
            const V4F R = v4f_sub(v4f_mul(vc, B0), v4f_mul(vs, B1));
            const V4F I = v4f_add(v4f_mul(vs, B0), v4f_mul(vc, B1));

            const V4F A0 = v4f_ld(A0p + n);
            const V4F A1 = v4f_ld(A1p + n);

            v4f_st(A0p + n, v4f_add(A0, R));
            v4f_st(B0p + n, v4f_add(A1, I));
            v4f_st(A1p + n, v4f_sub(A0, R));
            v4f_st(B1p + n, v4f_sub(I, A1));
        }
    }
#endif

    for (; n < q; ++n) {
        const float A0 = A0p[n];
        const float B0 = B0p[n];
        const float A1 = A1p[n];
        const float B1 = B1p[n];

        const float R = c_scalar * B0 - s_scalar * B1;
        const float I = s_scalar * B0 + c_scalar * B1;

        A0p[n] = A0 + R;
        B0p[n] = A1 + I;
        A1p[n] = A0 - R;
        B1p[n] = -A1 + I;
    }
}

// Two fused tree levels, float lane-doubled twin of norm2_fused.
// Caller guarantees q >= 16 so qh >= 8.
static inline void norm2_fused_f32(float* __restrict p, int q,
                                   float c, float s,
                                   float c0, float s0,
                                   float c1, float s1) {
    const int qh = q >> 1;
    float* __restrict A0 = p;
    float* __restrict B0 = p + q;
    float* __restrict A1 = p + 2*q;
    float* __restrict B1 = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const __m256 vc  = _mm256_set1_ps(c),  vs  = _mm256_set1_ps(s);
        const __m256 vc0 = _mm256_set1_ps(c0), vs0 = _mm256_set1_ps(s0);
        const __m256 vc1 = _mm256_set1_ps(c1), vs1 = _mm256_set1_ps(s1);

        for (; n + 7 < qh; n += 8) {
            const __m256 b0n = _mm256_loadu_ps(B0 + n);
            const __m256 b1n = _mm256_loadu_ps(B1 + n);
            const __m256 b0h = _mm256_loadu_ps(B0 + qh + n);
            const __m256 b1h = _mm256_loadu_ps(B1 + qh + n);

            const __m256 Rn = _mm256_sub_ps(_mm256_mul_ps(vc, b0n), _mm256_mul_ps(vs, b1n));
            const __m256 In = _mm256_add_ps(_mm256_mul_ps(vs, b0n), _mm256_mul_ps(vc, b1n));
            const __m256 Rh = _mm256_sub_ps(_mm256_mul_ps(vc, b0h), _mm256_mul_ps(vs, b1h));
            const __m256 Ih = _mm256_add_ps(_mm256_mul_ps(vs, b0h), _mm256_mul_ps(vc, b1h));

            const __m256 a0n = _mm256_loadu_ps(A0 + n);
            const __m256 a0h = _mm256_loadu_ps(A0 + qh + n);
            const __m256 a1n = _mm256_loadu_ps(A1 + n);
            const __m256 a1h = _mm256_loadu_ps(A1 + qh + n);

            const __m256 u0 = _mm256_add_ps(a0n, Rn);
            const __m256 uh = _mm256_add_ps(a0h, Rh);
            const __m256 w0 = _mm256_add_ps(a1n, In);
            const __m256 wh = _mm256_add_ps(a1h, Ih);
            const __m256 v0 = _mm256_sub_ps(a0n, Rn);
            const __m256 vh = _mm256_sub_ps(a0h, Rh);
            const __m256 x0 = _mm256_sub_ps(In, a1n);
            const __m256 xh = _mm256_sub_ps(Ih, a1h);

            const __m256 R0 = _mm256_sub_ps(_mm256_mul_ps(vc0, uh), _mm256_mul_ps(vs0, wh));
            const __m256 I0 = _mm256_add_ps(_mm256_mul_ps(vs0, uh), _mm256_mul_ps(vc0, wh));
            const __m256 R1 = _mm256_sub_ps(_mm256_mul_ps(vc1, vh), _mm256_mul_ps(vs1, xh));
            const __m256 I1 = _mm256_add_ps(_mm256_mul_ps(vs1, vh), _mm256_mul_ps(vc1, xh));

            _mm256_storeu_ps(A0 + n,      _mm256_add_ps(u0, R0));
            _mm256_storeu_ps(A0 + qh + n, _mm256_add_ps(w0, I0));
            _mm256_storeu_ps(B0 + n,      _mm256_sub_ps(u0, R0));
            _mm256_storeu_ps(B0 + qh + n, _mm256_sub_ps(I0, w0));
            _mm256_storeu_ps(A1 + n,      _mm256_add_ps(v0, R1));
            _mm256_storeu_ps(A1 + qh + n, _mm256_add_ps(x0, I1));
            _mm256_storeu_ps(B1 + n,      _mm256_sub_ps(v0, R1));
            _mm256_storeu_ps(B1 + qh + n, _mm256_sub_ps(I1, x0));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V4F vc  = v4f_set1(c),  vs  = v4f_set1(s);
        const V4F vc0 = v4f_set1(c0), vs0 = v4f_set1(s0);
        const V4F vc1 = v4f_set1(c1), vs1 = v4f_set1(s1);

        for (; n + 3 < qh; n += 4) {
            const V4F b0n = v4f_ld(B0 + n);
            const V4F b1n = v4f_ld(B1 + n);
            const V4F b0h = v4f_ld(B0 + qh + n);
            const V4F b1h = v4f_ld(B1 + qh + n);

            const V4F Rn = v4f_sub(v4f_mul(vc, b0n), v4f_mul(vs, b1n));
            const V4F In = v4f_add(v4f_mul(vs, b0n), v4f_mul(vc, b1n));
            const V4F Rh = v4f_sub(v4f_mul(vc, b0h), v4f_mul(vs, b1h));
            const V4F Ih = v4f_add(v4f_mul(vs, b0h), v4f_mul(vc, b1h));

            const V4F a0n = v4f_ld(A0 + n);
            const V4F a0h = v4f_ld(A0 + qh + n);
            const V4F a1n = v4f_ld(A1 + n);
            const V4F a1h = v4f_ld(A1 + qh + n);

            const V4F u0 = v4f_add(a0n, Rn);
            const V4F uh = v4f_add(a0h, Rh);
            const V4F w0 = v4f_add(a1n, In);
            const V4F wh = v4f_add(a1h, Ih);
            const V4F v0 = v4f_sub(a0n, Rn);
            const V4F vh = v4f_sub(a0h, Rh);
            const V4F x0 = v4f_sub(In, a1n);
            const V4F xh = v4f_sub(Ih, a1h);

            const V4F R0 = v4f_sub(v4f_mul(vc0, uh), v4f_mul(vs0, wh));
            const V4F I0 = v4f_add(v4f_mul(vs0, uh), v4f_mul(vc0, wh));
            const V4F R1 = v4f_sub(v4f_mul(vc1, vh), v4f_mul(vs1, xh));
            const V4F I1 = v4f_add(v4f_mul(vs1, vh), v4f_mul(vc1, xh));

            v4f_st(A0 + n,      v4f_add(u0, R0));
            v4f_st(A0 + qh + n, v4f_add(w0, I0));
            v4f_st(B0 + n,      v4f_sub(u0, R0));
            v4f_st(B0 + qh + n, v4f_sub(I0, w0));
            v4f_st(A1 + n,      v4f_add(v0, R1));
            v4f_st(A1 + qh + n, v4f_add(x0, I1));
            v4f_st(B1 + n,      v4f_sub(v0, R1));
            v4f_st(B1 + qh + n, v4f_sub(I1, x0));
        }
    }
#endif

    for (; n < qh; ++n) {
        const float a0n = A0[n],      a0h = A0[qh + n];
        const float b0n = B0[n],      b0h = B0[qh + n];
        const float a1n = A1[n],      a1h = A1[qh + n];
        const float b1n = B1[n],      b1h = B1[qh + n];

        const float Rn = c * b0n - s * b1n;
        const float In = s * b0n + c * b1n;
        const float Rh = c * b0h - s * b1h;
        const float Ih = s * b0h + c * b1h;

        const float u0 = a0n + Rn, uh = a0h + Rh;
        const float w0 = a1n + In, wh = a1h + Ih;
        const float v0 = a0n - Rn, vh = a0h - Rh;
        const float x0 = In - a1n, xh = Ih - a1h;

        const float R0 = c0 * uh - s0 * wh;
        const float I0 = s0 * uh + c0 * wh;
        const float R1 = c1 * vh - s1 * xh;
        const float I1 = s1 * vh + c1 * xh;

        A0[n] = u0 + R0;
        A0[qh + n] = w0 + I0;
        B0[n] = u0 - R0;
        B0[qh + n] = I0 - w0;
        A1[n] = v0 + R1;
        A1[qh + n] = x0 + I1;
        B1[n] = v0 - R1;
        B1[qh + n] = I1 - x0;
    }
}

static inline void norm_q_inv_f32(float* __restrict p, int q, float c_scalar, float s_scalar) {
    float* __restrict C0p = p;
    float* __restrict C1p = p + q;
    float* __restrict D0p = p + 2*q;
    float* __restrict D1p = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const __m256 half = _mm256_set1_ps(0.5f);
        const __m256 hvc = _mm256_set1_ps(0.5f * c_scalar);
        const __m256 hvs = _mm256_set1_ps(0.5f * s_scalar);

        for (; n + 7 < q; n += 8) {
            const __m256 C0v = _mm256_loadu_ps(C0p + n);
            const __m256 C1v = _mm256_loadu_ps(C1p + n);
            const __m256 D0v = _mm256_loadu_ps(D0p + n);
            const __m256 D1v = _mm256_loadu_ps(D1p + n);

            const __m256 t0 = _mm256_add_ps(C0v, D0v);
            const __m256 r  = _mm256_sub_ps(C0v, D0v);
            const __m256 i  = _mm256_add_ps(C1v, D1v);
            const __m256 t1 = _mm256_sub_ps(C1v, D1v);

            const __m256 A0 = _mm256_mul_ps(half, t0);
            const __m256 A1 = _mm256_mul_ps(half, t1);
            const __m256 B0 = _mm256_add_ps(_mm256_mul_ps(hvc, r), _mm256_mul_ps(hvs, i));
            const __m256 B1 = _mm256_sub_ps(_mm256_mul_ps(hvc, i), _mm256_mul_ps(hvs, r));

            _mm256_storeu_ps(C0p + n, A0);
            _mm256_storeu_ps(C1p + n, B0);
            _mm256_storeu_ps(D0p + n, A1);
            _mm256_storeu_ps(D1p + n, B1);
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V4F half = v4f_set1(0.5f);
        const V4F hvc = v4f_set1(0.5f * c_scalar);
        const V4F hvs = v4f_set1(0.5f * s_scalar);

        for (; n + 3 < q; n += 4) {
            const V4F C0v = v4f_ld(C0p + n);
            const V4F C1v = v4f_ld(C1p + n);
            const V4F D0v = v4f_ld(D0p + n);
            const V4F D1v = v4f_ld(D1p + n);

            const V4F t0 = v4f_add(C0v, D0v);
            const V4F r  = v4f_sub(C0v, D0v);
            const V4F i  = v4f_add(C1v, D1v);
            const V4F t1 = v4f_sub(C1v, D1v);

            const V4F A0 = v4f_mul(half, t0);
            const V4F A1 = v4f_mul(half, t1);
            const V4F B0 = v4f_add(v4f_mul(hvc, r), v4f_mul(hvs, i));
            const V4F B1 = v4f_sub(v4f_mul(hvc, i), v4f_mul(hvs, r));

            v4f_st(C0p + n, A0);
            v4f_st(C1p + n, B0);
            v4f_st(D0p + n, A1);
            v4f_st(D1p + n, B1);
        }
    }
#endif

    for (; n < q; ++n) {
        const float C0v = C0p[n];
        const float C1v = C1p[n];
        const float D0v = D0p[n];
        const float D1v = D1p[n];

        const float A0 = 0.5f * (C0v + D0v);
        const float R  = 0.5f * (C0v - D0v);
        const float I  = 0.5f * (C1v + D1v);
        const float A1 = 0.5f * (C1v - D1v);

        C0p[n] = A0;
        C1p[n] = c_scalar * R + s_scalar * I;
        D0p[n] = A1;
        D1p[n] = c_scalar * I - s_scalar * R;
    }
}

// Exact inverse of norm2_fused_f32. Caller guarantees q >= 16 so qh >= 8.
static inline void norm2_inv_fused_f32(float* __restrict p, int q,
                                       float c, float s,
                                       float c0, float s0,
                                       float c1, float s1) {
    const int qh = q >> 1;
    float* __restrict A0 = p;
    float* __restrict B0 = p + q;
    float* __restrict A1 = p + 2*q;
    float* __restrict B1 = p + 3*q;

    int n = 0;

#if STXLIB_FFT_LEVEL >= 2
    {
        const __m256 hf  = _mm256_set1_ps(0.5f);
        const __m256 vc  = _mm256_set1_ps(c),  vs  = _mm256_set1_ps(s);
        const __m256 vc0 = _mm256_set1_ps(c0), vs0 = _mm256_set1_ps(s0);
        const __m256 vc1 = _mm256_set1_ps(c1), vs1 = _mm256_set1_ps(s1);

        for (; n + 7 < qh; n += 8) {
            const __m256 A0n = _mm256_loadu_ps(A0 + n);
            const __m256 A0h = _mm256_loadu_ps(A0 + qh + n);
            const __m256 B0n = _mm256_loadu_ps(B0 + n);
            const __m256 B0h = _mm256_loadu_ps(B0 + qh + n);
            const __m256 A1n = _mm256_loadu_ps(A1 + n);
            const __m256 A1h = _mm256_loadu_ps(A1 + qh + n);
            const __m256 B1n = _mm256_loadu_ps(B1 + n);
            const __m256 B1h = _mm256_loadu_ps(B1 + qh + n);

            const __m256 u0 = _mm256_mul_ps(hf, _mm256_add_ps(A0n, B0n));
            const __m256 R0 = _mm256_mul_ps(hf, _mm256_sub_ps(A0n, B0n));
            const __m256 I0 = _mm256_mul_ps(hf, _mm256_add_ps(A0h, B0h));
            const __m256 w0 = _mm256_mul_ps(hf, _mm256_sub_ps(A0h, B0h));
            const __m256 uh = _mm256_add_ps(_mm256_mul_ps(vc0, R0), _mm256_mul_ps(vs0, I0));
            const __m256 wh = _mm256_sub_ps(_mm256_mul_ps(vc0, I0), _mm256_mul_ps(vs0, R0));

            const __m256 v0 = _mm256_mul_ps(hf, _mm256_add_ps(A1n, B1n));
            const __m256 R1 = _mm256_mul_ps(hf, _mm256_sub_ps(A1n, B1n));
            const __m256 I1 = _mm256_mul_ps(hf, _mm256_add_ps(A1h, B1h));
            const __m256 x0 = _mm256_mul_ps(hf, _mm256_sub_ps(A1h, B1h));
            const __m256 vh = _mm256_add_ps(_mm256_mul_ps(vc1, R1), _mm256_mul_ps(vs1, I1));
            const __m256 xh = _mm256_sub_ps(_mm256_mul_ps(vc1, I1), _mm256_mul_ps(vs1, R1));

            const __m256 a0n = _mm256_mul_ps(hf, _mm256_add_ps(u0, v0));
            const __m256 Rn  = _mm256_mul_ps(hf, _mm256_sub_ps(u0, v0));
            const __m256 In  = _mm256_mul_ps(hf, _mm256_add_ps(w0, x0));
            const __m256 a1n = _mm256_mul_ps(hf, _mm256_sub_ps(w0, x0));
            const __m256 a0h = _mm256_mul_ps(hf, _mm256_add_ps(uh, vh));
            const __m256 Rh  = _mm256_mul_ps(hf, _mm256_sub_ps(uh, vh));
            const __m256 Ih  = _mm256_mul_ps(hf, _mm256_add_ps(wh, xh));
            const __m256 a1h = _mm256_mul_ps(hf, _mm256_sub_ps(wh, xh));

            _mm256_storeu_ps(A0 + n,      a0n);
            _mm256_storeu_ps(A0 + qh + n, a0h);
            _mm256_storeu_ps(B0 + n,      _mm256_add_ps(_mm256_mul_ps(vc, Rn), _mm256_mul_ps(vs, In)));
            _mm256_storeu_ps(B0 + qh + n, _mm256_add_ps(_mm256_mul_ps(vc, Rh), _mm256_mul_ps(vs, Ih)));
            _mm256_storeu_ps(A1 + n,      a1n);
            _mm256_storeu_ps(A1 + qh + n, a1h);
            _mm256_storeu_ps(B1 + n,      _mm256_sub_ps(_mm256_mul_ps(vc, In), _mm256_mul_ps(vs, Rn)));
            _mm256_storeu_ps(B1 + qh + n, _mm256_sub_ps(_mm256_mul_ps(vc, Ih), _mm256_mul_ps(vs, Rh)));
        }
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    {
        const V4F hf  = v4f_set1(0.5f);
        const V4F vc  = v4f_set1(c),  vs  = v4f_set1(s);
        const V4F vc0 = v4f_set1(c0), vs0 = v4f_set1(s0);
        const V4F vc1 = v4f_set1(c1), vs1 = v4f_set1(s1);

        for (; n + 3 < qh; n += 4) {
            const V4F A0n = v4f_ld(A0 + n);
            const V4F A0h = v4f_ld(A0 + qh + n);
            const V4F B0n = v4f_ld(B0 + n);
            const V4F B0h = v4f_ld(B0 + qh + n);
            const V4F A1n = v4f_ld(A1 + n);
            const V4F A1h = v4f_ld(A1 + qh + n);
            const V4F B1n = v4f_ld(B1 + n);
            const V4F B1h = v4f_ld(B1 + qh + n);

            const V4F u0 = v4f_mul(hf, v4f_add(A0n, B0n));
            const V4F R0 = v4f_mul(hf, v4f_sub(A0n, B0n));
            const V4F I0 = v4f_mul(hf, v4f_add(A0h, B0h));
            const V4F w0 = v4f_mul(hf, v4f_sub(A0h, B0h));
            const V4F uh = v4f_add(v4f_mul(vc0, R0), v4f_mul(vs0, I0));
            const V4F wh = v4f_sub(v4f_mul(vc0, I0), v4f_mul(vs0, R0));

            const V4F v0 = v4f_mul(hf, v4f_add(A1n, B1n));
            const V4F R1 = v4f_mul(hf, v4f_sub(A1n, B1n));
            const V4F I1 = v4f_mul(hf, v4f_add(A1h, B1h));
            const V4F x0 = v4f_mul(hf, v4f_sub(A1h, B1h));
            const V4F vh = v4f_add(v4f_mul(vc1, R1), v4f_mul(vs1, I1));
            const V4F xh = v4f_sub(v4f_mul(vc1, I1), v4f_mul(vs1, R1));

            const V4F a0n = v4f_mul(hf, v4f_add(u0, v0));
            const V4F Rn  = v4f_mul(hf, v4f_sub(u0, v0));
            const V4F In  = v4f_mul(hf, v4f_add(w0, x0));
            const V4F a1n = v4f_mul(hf, v4f_sub(w0, x0));
            const V4F a0h = v4f_mul(hf, v4f_add(uh, vh));
            const V4F Rh  = v4f_mul(hf, v4f_sub(uh, vh));
            const V4F Ih  = v4f_mul(hf, v4f_add(wh, xh));
            const V4F a1h = v4f_mul(hf, v4f_sub(wh, xh));

            v4f_st(A0 + n,      a0n);
            v4f_st(A0 + qh + n, a0h);
            v4f_st(B0 + n,      v4f_add(v4f_mul(vc, Rn), v4f_mul(vs, In)));
            v4f_st(B0 + qh + n, v4f_add(v4f_mul(vc, Rh), v4f_mul(vs, Ih)));
            v4f_st(A1 + n,      a1n);
            v4f_st(A1 + qh + n, a1h);
            v4f_st(B1 + n,      v4f_sub(v4f_mul(vc, In), v4f_mul(vs, Rn)));
            v4f_st(B1 + qh + n, v4f_sub(v4f_mul(vc, Ih), v4f_mul(vs, Rh)));
        }
    }
#endif

    for (; n < qh; ++n) {
        const float A0n = A0[n], A0h = A0[qh + n];
        const float B0n = B0[n], B0h = B0[qh + n];
        const float A1n = A1[n], A1h = A1[qh + n];
        const float B1n = B1[n], B1h = B1[qh + n];

        const float u0 = 0.5f * (A0n + B0n);
        const float R0 = 0.5f * (A0n - B0n);
        const float I0 = 0.5f * (A0h + B0h);
        const float w0 = 0.5f * (A0h - B0h);
        const float uh = c0 * R0 + s0 * I0;
        const float wh = c0 * I0 - s0 * R0;

        const float v0 = 0.5f * (A1n + B1n);
        const float R1 = 0.5f * (A1n - B1n);
        const float I1 = 0.5f * (A1h + B1h);
        const float x0 = 0.5f * (A1h - B1h);
        const float vh = c1 * R1 + s1 * I1;
        const float xh = c1 * I1 - s1 * R1;

        const float a0n = 0.5f * (u0 + v0);
        const float Rn  = 0.5f * (u0 - v0);
        const float In  = 0.5f * (w0 + x0);
        const float a1n = 0.5f * (w0 - x0);
        const float a0h = 0.5f * (uh + vh);
        const float Rh  = 0.5f * (uh - vh);
        const float Ih  = 0.5f * (wh + xh);
        const float a1h = 0.5f * (wh - xh);

        A0[n] = a0n;
        A0[qh + n] = a0h;
        B0[n] = c * Rn + s * In;
        B0[qh + n] = c * Rh + s * Ih;
        A1[n] = a1n;
        A1[qh + n] = a1h;
        B1[n] = c * In - s * Rn;
        B1[qh + n] = c * Ih - s * Rh;
    }
}

class DifKernel final {
public:
    explicit DifKernel(const DifTables& tables) noexcept
        : N(tables.n), L(tables.log2n), IDX(tables.idx), KINV(tables.kinv), C(tables.c), CF(tables.cf),
          TW(tables.tw), TWF(tables.twf) {}

    // float64 forward: residues into work, then X in bin order through KINV
    // (BFFT's forward_standard_two_phase, at every size).
    void forward_standard(const double* __restrict input, Complex64* __restrict X, double* __restrict v) const {
        forward_residues(input, v);

        X[0].re = v[0] + v[1];
        X[0].im = 0.0;
        X[N / 2].re = v[0] - v[1];
        X[N / 2].im = 0.0;

        const int* __restrict kin = KINV;
        int k = 1;
#if STXLIB_FFT_LEVEL >= 1
        for (; k < N / 2; ++k) {
            const V2 r = v2_ld(v + 2*kin[k]);
            v2_st(&X[k].re, v2_neghi(r));
        }
#else
        for (; k < N / 2; ++k) {
            const int m = kin[k];
            X[k].re = v[2*m];
            X[k].im = -v[2*m + 1];
        }
#endif
    }

    // float64 inverse, in the output buffer (BFFT's inverse).
    void inverse(const Complex64* __restrict X, double* __restrict out) const {
        complex_to_residues(X, out);
        inverse_residues_inplace(out);
    }

    // float32 forward (BFFT's forward_standard_f32).
    void forward_standard_f32(const float* __restrict input, Complex32* __restrict X,
                              float* __restrict work) const {
        forward_residues_f32(input, work);

        X[0].re = work[0] + work[1];
        X[0].im = 0.0f;
        X[N / 2].re = work[0] - work[1];
        X[N / 2].im = 0.0f;

        const int* __restrict kin = KINV;
        int k = 1;
        for (; k + 3 < N / 2; k += 4) {
            pack4_residues_to_complex_f32(X + k, work,
                                          kin[k], kin[k + 1], kin[k + 2], kin[k + 3]);
        }
        for (; k < N / 2; ++k) {
            const int m = kin[k];
            X[k].re = work[2*m];
            X[k].im = -work[2*m + 1];
        }
    }

    // float32 inverse, in the output buffer (BFFT's inverse_f32).
    void inverse_f32(const Complex32* __restrict X, float* __restrict out) const {
        out[0] = 0.5f * (X[0].re + X[N / 2].re);
        out[1] = 0.5f * (X[0].re - X[N / 2].re);
        int m = 1;
        for (; m + 3 < N / 2; m += 4) {
            unpack4_complex_to_residues_f32(out + 2*m, X,
                                            IDX[m], IDX[m + 1], IDX[m + 2], IDX[m + 3]);
        }
        for (; m < N / 2; ++m) {
            const int k = IDX[m];
            out[2 * m] = X[k].re;
            out[2 * m + 1] = -X[k].im;
        }
        inverse_residues_inplace_f32(out);
    }

private:
    using LeafTw = LeafTwiddles64;
    using LeafTwF = LeafTwiddles32;

    int N;
    int L;
    const int* IDX;
    const int* KINV;
    const double* C;
    const float* CF;
    const LeafTw* TW;
    const LeafTwF* TWF;

    // The sine of leaf m's angle is its sibling's cosine (m > 1); leaf 1's
    // angle is pi/4.
    inline double s_twiddle(int m) const {
        const unsigned u = static_cast<unsigned>(m);
        const unsigned flip = static_cast<unsigned>(-(m > 1)) & 1u;
        return C[u ^ flip];
    }

    inline float sf_twiddle(int m) const {
        const unsigned u = static_cast<unsigned>(m);
        const unsigned flip = static_cast<unsigned>(-(m > 1)) & 1u;
        return CF[u ^ flip];
    }


    // ---- float32 residue engine (BFFT lines 2269-2731) ----

    struct FloatSegment {
        float* data;
        int q;
        int m;
    };

    static inline void store4_interleaved_f32(float* __restrict dst,
                                             float re0, float im0,
                                             float re1, float im1,
                                             float re2, float im2,
                                             float re3, float im3) {
#if STXLIB_FFT_LEVEL >= 1
        const V4F re = v4f_set4(re0, re1, re2, re3);
        const V4F im = v4f_set4(im0, im1, im2, im3);
        v4f_st(dst, v4f_ziplo(re, im));
        v4f_st(dst + 4, v4f_ziphi(re, im));
#else
        dst[0] = re0;
        dst[1] = im0;
        dst[2] = re1;
        dst[3] = im1;
        dst[4] = re2;
        dst[5] = im2;
        dst[6] = re3;
        dst[7] = im3;
#endif
    }

    static inline void store4_complex_f32(Complex32* __restrict dst,
                                         float re0, float im0,
                                         float re1, float im1,
                                         float re2, float im2,
                                         float re3, float im3) {
        store4_interleaved_f32(reinterpret_cast<float*>(dst),
                               re0, im0, re1, im1, re2, im2, re3, im3);
    }

    static inline void pack4_residues_to_complex_f32(Complex32* __restrict dst,
                                                     const float* __restrict residues,
                                                     int m0, int m1, int m2, int m3) {
        store4_complex_f32(dst,
                           residues[2*m0], -residues[2*m0 + 1],
                           residues[2*m1], -residues[2*m1 + 1],
                           residues[2*m2], -residues[2*m2 + 1],
                           residues[2*m3], -residues[2*m3 + 1]);
    }

    static inline void copy4_complex_f32(Complex32* __restrict dst,
                                        const Complex32* __restrict src,
                                        int i0, int i1, int i2, int i3) {
        store4_complex_f32(dst,
                           src[i0].re, src[i0].im,
                           src[i1].re, src[i1].im,
                           src[i2].re, src[i2].im,
                           src[i3].re, src[i3].im);
    }

    static inline void unpack4_complex_to_residues_f32(float* __restrict dst,
                                                       const Complex32* __restrict src,
                                                       int i0, int i1, int i2, int i3) {
        store4_interleaved_f32(dst,
                               src[i0].re, -src[i0].im,
                               src[i1].re, -src[i1].im,
                               src[i2].re, -src[i2].im,
                               src[i3].re, -src[i3].im);
    }

    static inline void norm_q1_fwd_f32(float* __restrict p, float c, float s) {
        const float A0 = p[0];
        const float B0 = p[1];
        const float A1 = p[2];
        const float B1 = p[3];

        const float R = c * B0 - s * B1;
        const float I = s * B0 + c * B1;

        p[0] = A0 + R;
        p[1] = A1 + I;
        p[2] = A0 - R;
        p[3] = -A1 + I;
    }

    static inline void norm_q2_fwd_f32(float* __restrict p, float c, float s) {
        for (int n = 0; n < 2; ++n) {
            const float A0 = p[n];
            const float B0 = p[2 + n];
            const float A1 = p[4 + n];
            const float B1 = p[6 + n];

            const float R = c * B0 - s * B1;
            const float I = s * B0 + c * B1;

            p[n] = A0 + R;
            p[2 + n] = A1 + I;
            p[4 + n] = A0 - R;
            p[6 + n] = -A1 + I;
        }
    }

    // Residue-writing float leaf codelet: three tree levels on one 16-float
    // block, fused in 128-bit registers. Lane-for-lane twin of the AVX2 double
    // codelet (4 doubles per __m256d there, 4 floats per 128-bit vector here).
    inline void codelet_d3_tw_res_f32(float* __restrict p, const LeafTwF& t) const {
#if STXLIB_FFT_LEVEL >= 1
        const V4F A0 = v4f_ld(p);
        const V4F B0 = v4f_ld(p + 4);
        const V4F A1 = v4f_ld(p + 8);
        const V4F B1 = v4f_ld(p + 12);

        const V4F c1 = v4f_set1(t.c1);
        const V4F s1 = v4f_set1(t.s1);
        const V4F R1 = v4f_sub(v4f_mul(c1, B0), v4f_mul(s1, B1));
        const V4F I1 = v4f_add(v4f_mul(s1, B0), v4f_mul(c1, B1));

        const V4F c0a = v4f_add(A0, R1);
        const V4F c0b = v4f_add(A1, I1);
        const V4F c1a = v4f_sub(A0, R1);
        const V4F c1b = v4f_sub(I1, A1);

        const V4F A0v = v4f_catlo(c0a, c1a);
        const V4F B0v = v4f_cathi(c0a, c1a);
        const V4F A1v = v4f_catlo(c0b, c1b);
        const V4F B1v = v4f_cathi(c0b, c1b);

        const V4F c2 = v4f_ld(t.c2d);
        const V4F s2 = v4f_swap_halves(c2);
        const V4F R2 = v4f_sub(v4f_mul(c2, B0v), v4f_mul(s2, B1v));
        const V4F I2 = v4f_add(v4f_mul(s2, B0v), v4f_mul(c2, B1v));

        const V4F P = v4f_add(A0v, R2);
        const V4F Q = v4f_add(A1v, I2);
        const V4F M = v4f_sub(A0v, R2);
        const V4F W = v4f_sub(I2, A1v);

        const V4F pmL = v4f_ziplo(P, M);
        const V4F pmH = v4f_ziphi(P, M);
        const V4F qwL = v4f_ziplo(Q, W);
        const V4F qwH = v4f_ziphi(Q, W);
        const V4F A0w = v4f_catlo(pmL, pmH); // [P0,M0,P2,M2]
        const V4F B0w = v4f_cathi(pmL, pmH); // [P1,M1,P3,M3]
        const V4F A1w = v4f_catlo(qwL, qwH);
        const V4F B1w = v4f_cathi(qwL, qwH);

        const V4F c4 = v4f_ld(t.c4);
        const V4F s4 = v4f_swap_pairs(c4);
        const V4F R3 = v4f_sub(v4f_mul(c4, B0w), v4f_mul(s4, B1w));
        const V4F I3 = v4f_add(v4f_mul(s4, B0w), v4f_mul(c4, B1w));

        const V4F E0 = v4f_add(A0w, R3); // leaf even residue r0
        const V4F E1 = v4f_add(A1w, I3); // leaf even residue r1
        const V4F O0 = v4f_sub(A0w, R3); // leaf odd residue r0
        const V4F O1 = v4f_sub(I3, A1w); // leaf odd residue r1

        // 4x4 transpose to leaf-residue order: p[4g..4g+3] = [E0[g], E1[g], O0[g], O1[g]].
        const V4F eL = v4f_ziplo(E0, E1);
        const V4F eH = v4f_ziphi(E0, E1);
        const V4F oL = v4f_ziplo(O0, O1);
        const V4F oH = v4f_ziphi(O0, O1);
        const V4F t0 = v4f_catlo(eL, eH); // [E0_0,E1_0,E0_2,E1_2]
        const V4F t1 = v4f_cathi(eL, eH); // [E0_1,E1_1,E0_3,E1_3]
        const V4F t2 = v4f_catlo(oL, oH);
        const V4F t3 = v4f_cathi(oL, oH);

        v4f_st(p,      v4f_catlo(t0, t2));
        v4f_st(p + 4,  v4f_catlo(t1, t3));
        v4f_st(p + 8,  v4f_cathi(t0, t2));
        v4f_st(p + 12, v4f_cathi(t1, t3));
#else
        norm_q_fwd_f32(p, 4, t.c1, t.s1);
        norm_q2_fwd_f32(p, t.c2d[0], t.c2d[2]);
        norm_q2_fwd_f32(p + 8, t.c2d[2], t.c2d[0]);
        norm_q1_fwd_f32(p, t.c4[0], t.c4[1]);
        norm_q1_fwd_f32(p + 4, t.c4[1], t.c4[0]);
        norm_q1_fwd_f32(p + 8, t.c4[2], t.c4[3]);
        norm_q1_fwd_f32(p + 12, t.c4[3], t.c4[2]);
#endif
    }

    // Exact inverse of codelet_d3_tw_res_f32.
    inline void codelet_d3_tw_res_inv_f32(float* __restrict p, const LeafTwF& t) const {
#if STXLIB_FFT_LEVEL >= 1
        const V4F out0 = v4f_ld(p);
        const V4F out1 = v4f_ld(p + 4);
        const V4F out2 = v4f_ld(p + 8);
        const V4F out3 = v4f_ld(p + 12);

        // Inverse of the forward 4x4 transpose.
        const V4F aL = v4f_ziplo(out0, out1);
        const V4F aH = v4f_ziphi(out0, out1);
        const V4F bL = v4f_ziplo(out2, out3);
        const V4F bH = v4f_ziphi(out2, out3);
        const V4F t0 = v4f_catlo(aL, aH);
        const V4F t1 = v4f_cathi(aL, aH);
        const V4F t2 = v4f_catlo(bL, bH);
        const V4F t3 = v4f_cathi(bL, bH);
        const V4F E0 = v4f_catlo(t0, t2);
        const V4F O0 = v4f_cathi(t0, t2);
        const V4F E1 = v4f_catlo(t1, t3);
        const V4F O1 = v4f_cathi(t1, t3);

        const V4F hf = v4f_set1(0.5f);

        // Level 3 inverse.
        const V4F A0w = v4f_mul(hf, v4f_add(E0, O0));
        const V4F R3  = v4f_mul(hf, v4f_sub(E0, O0));
        const V4F I3  = v4f_mul(hf, v4f_add(E1, O1));
        const V4F A1w = v4f_mul(hf, v4f_sub(E1, O1));
        const V4F c4 = v4f_ld(t.c4);
        const V4F s4 = v4f_swap_pairs(c4);
        const V4F B0w = v4f_add(v4f_mul(c4, R3), v4f_mul(s4, I3));
        const V4F B1w = v4f_sub(v4f_mul(c4, I3), v4f_mul(s4, R3));

        // Level 2 inverse.
        const V4F abL = v4f_ziplo(A0w, B0w);
        const V4F abH = v4f_ziphi(A0w, B0w);
        const V4F cdL = v4f_ziplo(A1w, B1w);
        const V4F cdH = v4f_ziphi(A1w, B1w);
        const V4F P = v4f_catlo(abL, abH);
        const V4F M = v4f_cathi(abL, abH);
        const V4F Q = v4f_catlo(cdL, cdH);
        const V4F W = v4f_cathi(cdL, cdH);

        const V4F A0v = v4f_mul(hf, v4f_add(P, M));
        const V4F R2  = v4f_mul(hf, v4f_sub(P, M));
        const V4F I2  = v4f_mul(hf, v4f_add(Q, W));
        const V4F A1v = v4f_mul(hf, v4f_sub(Q, W));
        const V4F c2 = v4f_ld(t.c2d);
        const V4F s2 = v4f_swap_halves(c2);
        const V4F B0v = v4f_add(v4f_mul(c2, R2), v4f_mul(s2, I2));
        const V4F B1v = v4f_sub(v4f_mul(c2, I2), v4f_mul(s2, R2));

        // Level 1 inverse.
        const V4F c0a = v4f_catlo(A0v, B0v);
        const V4F c1a = v4f_cathi(A0v, B0v);
        const V4F c0b = v4f_catlo(A1v, B1v);
        const V4F c1b = v4f_cathi(A1v, B1v);

        const V4F A0 = v4f_mul(hf, v4f_add(c0a, c1a));
        const V4F R1 = v4f_mul(hf, v4f_sub(c0a, c1a));
        const V4F I1 = v4f_mul(hf, v4f_add(c0b, c1b));
        const V4F A1 = v4f_mul(hf, v4f_sub(c0b, c1b));
        const V4F c1v = v4f_set1(t.c1);
        const V4F s1v = v4f_set1(t.s1);
        const V4F B0 = v4f_add(v4f_mul(c1v, R1), v4f_mul(s1v, I1));
        const V4F B1 = v4f_sub(v4f_mul(c1v, I1), v4f_mul(s1v, R1));

        v4f_st(p,      A0);
        v4f_st(p + 4,  B0);
        v4f_st(p + 8,  A1);
        v4f_st(p + 12, B1);
#else
        norm_q_inv_f32(p,      1, t.c4[0], t.c4[1]);
        norm_q_inv_f32(p + 4,  1, t.c4[1], t.c4[0]);
        norm_q_inv_f32(p + 8,  1, t.c4[2], t.c4[3]);
        norm_q_inv_f32(p + 12, 1, t.c4[3], t.c4[2]);
        norm_q_inv_f32(p,      2, t.c2d[0], t.c2d[2]);
        norm_q_inv_f32(p + 8,  2, t.c2d[2], t.c2d[0]);
        norm_q_inv_f32(p,      4, t.c1,    t.s1);
#endif
    }

    void run_fwd_res_segments_f32(float* __restrict root, int root_q, int root_m) const {
        FloatSegment stack[96];
        int stack_size = 0;
        stack[stack_size++] = FloatSegment{root, root_q, root_m};

        while (stack_size > 0) {
            --stack_size;
            const FloatSegment segment = stack[stack_size];
            float* __restrict v = segment.data;
            const int q = segment.q;
            const int m = segment.m;

            if (q >= 16) {
                norm2_fused_f32(v, q, CF[m], sf_twiddle(m), CF[2*m], sf_twiddle(2*m), CF[2*m+1], sf_twiddle(2*m+1));
                const int qq = q >> 2;
                const int child_m = 4 * m;

                stack[stack_size++] = FloatSegment{v + 3*q, qq, child_m + 3};
                stack[stack_size++] = FloatSegment{v + 2*q, qq, child_m + 2};
                stack[stack_size++] = FloatSegment{v + q,   qq, child_m + 1};
                stack[stack_size++] = FloatSegment{v,       qq, child_m};
                continue;
            }

            if (q == 8) {
                norm_q_fwd_f32(v, 8, CF[m], sf_twiddle(m));
                codelet_d3_tw_res_f32(v, TWF[2*m]);
                codelet_d3_tw_res_f32(v + 16, TWF[2*m + 1]);
                continue;
            }

            codelet_d3_tw_res_f32(v, TWF[m]);
        }
    }

    void residue_spine_tail_fwd_f32(float* __restrict v) const {
        codelet_d3_tw_res_f32(v + 16, TWF[1]);
        binomial_fwd_f32(v, 8);
        norm_q_fwd_f32(v + 8, 2, CF[1], sf_twiddle(1));
        norm_q1_fwd_f32(v + 8, CF[2], sf_twiddle(2));
        norm_q1_fwd_f32(v + 12, CF[3], sf_twiddle(3));
        binomial_fwd_f32(v, 4);
        norm_q1_fwd_f32(v + 4, CF[1], sf_twiddle(1));
        binomial_fwd_f32(v, 2);
    }

    // Fast depth-first float residue forward with fused input copy. Requires N >= 64.
    void forward_residues_recursive_f32(const float* __restrict input, float* __restrict v) const {
        binomial_oop_f32(input, v, N / 2);

        for (int h = N / 2; h >= 32; h >>= 1) {
            run_fwd_res_segments_f32(v + h, h >> 2, 1);
            binomial_fwd_f32(v, h >> 1);
        }

        residue_spine_tail_fwd_f32(v);
    }

    void forward_stage_f32(float* __restrict v, int jj) const {
        const int s = N >> jj;
        const int h = s >> 1;
        const int q = s >> 2;
        const int m_end = 1 << jj;

        binomial_fwd_f32(v, h);

        if (q == 1) {
            for (int m = 1; m < m_end; ++m) norm_q1_fwd_f32(v + m*s, CF[m], sf_twiddle(m));
        } else if (q == 2) {
            for (int m = 1; m < m_end; ++m) norm_q2_fwd_f32(v + m*s, CF[m], sf_twiddle(m));
        } else {
            for (int m = 1; m < m_end; ++m) norm_q_fwd_f32(v + m*s, q, CF[m], sf_twiddle(m));
        }
    }

    void forward_residues_inplace_f32(float* __restrict v) const {
        for (int jj = 0; jj < L - 1; ++jj) {
            forward_stage_f32(v, jj);
        }
    }

    // Time -> residues, float. Leaf m lands in (v[2m], v[2m+1]); v[0], v[1]
    // carry DC and Nyquist content exactly as in the double residue domain.
    void forward_residues_f32(const float* __restrict input, float* __restrict v) const {
        if (N >= 64) {
            forward_residues_recursive_f32(input, v);
            return;
        }
        __builtin_memcpy(v, input, sizeof(float) * N);
        forward_residues_inplace_f32(v);
    }

    void rec_inv_res_f32(float* __restrict v, int q, int m) const {
        if (q >= 16) {
            const int qq = q >> 2;
            rec_inv_res_f32(v,       qq, 4*m);
            rec_inv_res_f32(v + q,   qq, 4*m + 1);
            rec_inv_res_f32(v + 2*q, qq, 4*m + 2);
            rec_inv_res_f32(v + 3*q, qq, 4*m + 3);
            norm2_inv_fused_f32(v, q, CF[m], sf_twiddle(m), CF[2*m], sf_twiddle(2*m), CF[2*m+1], sf_twiddle(2*m+1));
            return;
        }
        if (q == 8) {
            codelet_d3_tw_res_inv_f32(v, TWF[2*m]);
            codelet_d3_tw_res_inv_f32(v + 16, TWF[2*m + 1]);
            norm_q_inv_f32(v, 8, CF[m], sf_twiddle(m));
            return;
        }
        codelet_d3_tw_res_inv_f32(v, TWF[m]);
    }

    void residue_spine_tail_inv_f32(float* __restrict v) const {
        binomial_inv_f32(v, 2);
        norm_q_inv_f32(v + 4, 1, CF[1], sf_twiddle(1));
        binomial_inv_f32(v, 4);
        norm_q_inv_f32(v + 8, 1, CF[2], sf_twiddle(2));
        norm_q_inv_f32(v + 12, 1, CF[3], sf_twiddle(3));
        norm_q_inv_f32(v + 8, 2, CF[1], sf_twiddle(1));
        binomial_inv_f32(v, 8);
        codelet_d3_tw_res_inv_f32(v + 16, TWF[1]);
    }

    // Exact reverse of forward_residues_recursive_f32. Requires N >= 64.
    // (BFFT runs its precomputed inverse schedule at BRUUN_LEVEL 1 instead;
    // the schedule applies the same operations to the same disjoint blocks,
    // so the values are the same.)
    void inverse_residues_recursive_f32(float* __restrict v) const {
        residue_spine_tail_inv_f32(v);

        for (int h = 32; h <= N / 2; h <<= 1) {
            binomial_inv_f32(v, h >> 1);
            rec_inv_res_f32(v + h, h >> 2, 1);
        }

        binomial_inv_f32(v, N / 2);
    }

    void inverse_residues_inplace_f32(float* __restrict v) const {
        if (N >= 64) {
            inverse_residues_recursive_f32(v);
            return;
        }

        for (int jj = L - 2; jj >= 0; --jj) {
            const int s = N >> jj;
            const int q = s >> 2;
            const int m_end = 1 << jj;

            for (int m = m_end - 1; m > 0; --m) {
                norm_q_inv_f32(v + m*s, q, CF[m], sf_twiddle(m));
            }

            binomial_inv_f32(v, s >> 1);
        }
    }

    // ---- float64 residue engine ----

    static inline void norm_q1_fwd(double* __restrict p, double c, double s) {
        const double A0 = p[0];
        const double B0 = p[1];
        const double A1 = p[2];
        const double B1 = p[3];

        const double R = c * B0 - s * B1;
        const double I = s * B0 + c * B1;

        p[0] = A0 + R;
        p[1] = A1 + I;
        p[2] = A0 - R;
        p[3] = -A1 + I;
    }

    static inline void norm_q2_fwd(double* __restrict p, double c, double s) {
        for (int n = 0; n < 2; ++n) {
            const double A0 = p[n];
            const double B0 = p[2 + n];
            const double A1 = p[4 + n];
            const double B1 = p[6 + n];

            const double R = c * B0 - s * B1;
            const double I = s * B0 + c * B1;

            p[n] = A0 + R;
            p[2 + n] = A1 + I;
            p[4 + n] = A0 - R;
            p[6 + n] = -A1 + I;
        }
    }

    // Residue-writing leaf codelet: identical math to the packing codelets,
    // but the 16 results are written back into p in leaf-residue order. This is
    // the engine for the residue-domain interfaces (filtering, custom CRT layouts)
    // and for the optional two-phase pack.
    inline void codelet_d3_tw_res(double* __restrict p, const LeafTw& t) const {
#if STXLIB_FFT_LEVEL >= 2
        const V4D A0 = v4d_ld(p);
        const V4D B0 = v4d_ld(p + 4);
        const V4D A1 = v4d_ld(p + 8);
        const V4D B1 = v4d_ld(p + 12);

        const V4D c1 = v4d_set1(t.c1);
        const V4D s1 = v4d_set1(t.s1);
        const V4D R1 = v4d_sub(v4d_mul(c1, B0), v4d_mul(s1, B1));
        const V4D I1 = v4d_add(v4d_mul(s1, B0), v4d_mul(c1, B1));

        const V4D c0a = v4d_add(A0, R1);
        const V4D c0b = v4d_add(A1, I1);
        const V4D c1a = v4d_sub(A0, R1);
        const V4D c1b = v4d_sub(I1, A1);

        const V4D A0v = v4d_cat128_lohi(c0a, c1a);
        const V4D B0v = v4d_cat128_hihi(c0a, c1a);
        const V4D A1v = v4d_cat128_lohi(c0b, c1b);
        const V4D B1v = v4d_cat128_hihi(c0b, c1b);

        const V4D c2 = v4d_ld2_dup(t.c2);
        const V4D s2 = v4d_swap_halves(c2);
        const V4D R2 = v4d_sub(v4d_mul(c2, B0v), v4d_mul(s2, B1v));
        const V4D I2 = v4d_add(v4d_mul(s2, B0v), v4d_mul(c2, B1v));

        const V4D P = v4d_add(A0v, R2);
        const V4D Q = v4d_add(A1v, I2);
        const V4D M = v4d_sub(A0v, R2);
        const V4D W = v4d_sub(I2, A1v);

        const V4D A0w = v4d_unplo(P, M);
        const V4D B0w = v4d_unphi(P, M);
        const V4D A1w = v4d_unplo(Q, W);
        const V4D B1w = v4d_unphi(Q, W);

        const V4D c4 = v4d_ld(t.c4);
        const V4D s4 = v4d_swap_pairs(c4);
        const V4D R3 = v4d_sub(v4d_mul(c4, B0w), v4d_mul(s4, B1w));
        const V4D I3 = v4d_add(v4d_mul(s4, B0w), v4d_mul(c4, B1w));

        const __m256d E0 = _mm256_add_pd(A0w, R3); // leaf even residue r0
        const __m256d E1 = _mm256_add_pd(A1w, I3); // leaf even residue r1
        const __m256d O0 = _mm256_sub_pd(A0w, R3); // leaf odd residue r0
        const __m256d O1 = _mm256_sub_pd(I3, A1w); // leaf odd residue r1

        // 4x4 transpose to leaf-residue order: p[4g..4g+3] = [E0[g], E1[g], O0[g], O1[g]].
        const __m256d t0 = _mm256_unpacklo_pd(E0, E1);
        const __m256d t1 = _mm256_unpackhi_pd(E0, E1);
        const __m256d t2 = _mm256_unpacklo_pd(O0, O1);
        const __m256d t3 = _mm256_unpackhi_pd(O0, O1);

        _mm256_storeu_pd(p,      _mm256_permute2f128_pd(t0, t2, 0x20));
        _mm256_storeu_pd(p + 4,  _mm256_permute2f128_pd(t1, t3, 0x20));
        _mm256_storeu_pd(p + 8,  _mm256_permute2f128_pd(t0, t2, 0x31));
        _mm256_storeu_pd(p + 12, _mm256_permute2f128_pd(t1, t3, 0x31));
#else
        norm_q_fwd(p, 4, t.c1, t.s1);
        norm_q2_fwd(p, t.c2[0], t.c2[1]);
        norm_q2_fwd(p + 8, t.c2[1], t.c2[0]);
        norm_q1_fwd(p, t.c4[0], t.c4[1]);
        norm_q1_fwd(p + 4, t.c4[1], t.c4[0]);
        norm_q1_fwd(p + 8, t.c4[2], t.c4[3]);
        norm_q1_fwd(p + 12, t.c4[3], t.c4[2]);
#endif
    }

    // Exact inverse of codelet_d3_tw_res: three inverse levels on one 16-double
    // leaf block, fused in registers on AVX2, generic norm_q_inv ladder otherwise.
    inline void codelet_d3_tw_res_inv(double* __restrict p, const LeafTw& t) const {
#if STXLIB_FFT_LEVEL >= 2
        const __m256d out0 = _mm256_loadu_pd(p);
        const __m256d out1 = _mm256_loadu_pd(p + 4);
        const __m256d out2 = _mm256_loadu_pd(p + 8);
        const __m256d out3 = _mm256_loadu_pd(p + 12);

        // Inverse of the forward 4x4 transpose.
        const __m256d t0 = _mm256_unpacklo_pd(out0, out1);
        const __m256d t1 = _mm256_unpackhi_pd(out0, out1);
        const __m256d t2 = _mm256_unpacklo_pd(out2, out3);
        const __m256d t3 = _mm256_unpackhi_pd(out2, out3);
        const __m256d E0 = _mm256_permute2f128_pd(t0, t2, 0x20);
        const __m256d O0 = _mm256_permute2f128_pd(t0, t2, 0x31);
        const __m256d E1 = _mm256_permute2f128_pd(t1, t3, 0x20);
        const __m256d O1 = _mm256_permute2f128_pd(t1, t3, 0x31);

        const __m256d hf = _mm256_set1_pd(0.5);

        // Level 3 inverse.
        const __m256d A0w = _mm256_mul_pd(hf, _mm256_add_pd(E0, O0));
        const __m256d R3  = _mm256_mul_pd(hf, _mm256_sub_pd(E0, O0));
        const __m256d I3  = _mm256_mul_pd(hf, _mm256_add_pd(E1, O1));
        const __m256d A1w = _mm256_mul_pd(hf, _mm256_sub_pd(E1, O1));
        const V4D c4 = v4d_ld(t.c4);
        const V4D s4 = v4d_swap_pairs(c4);
        const __m256d B0w = _mm256_add_pd(_mm256_mul_pd(c4, R3), _mm256_mul_pd(s4, I3));
        const __m256d B1w = _mm256_sub_pd(_mm256_mul_pd(c4, I3), _mm256_mul_pd(s4, R3));

        // Level 2 inverse.
        const __m256d P = _mm256_unpacklo_pd(A0w, B0w);
        const __m256d M = _mm256_unpackhi_pd(A0w, B0w);
        const __m256d Q = _mm256_unpacklo_pd(A1w, B1w);
        const __m256d W = _mm256_unpackhi_pd(A1w, B1w);

        const __m256d A0v = _mm256_mul_pd(hf, _mm256_add_pd(P, M));
        const __m256d R2  = _mm256_mul_pd(hf, _mm256_sub_pd(P, M));
        const __m256d I2  = _mm256_mul_pd(hf, _mm256_add_pd(Q, W));
        const __m256d A1v = _mm256_mul_pd(hf, _mm256_sub_pd(Q, W));
        const V4D c2 = v4d_ld2_dup(t.c2);
        const V4D s2 = v4d_swap_halves(c2);
        const __m256d B0v = _mm256_add_pd(_mm256_mul_pd(c2, R2), _mm256_mul_pd(s2, I2));
        const __m256d B1v = _mm256_sub_pd(_mm256_mul_pd(c2, I2), _mm256_mul_pd(s2, R2));

        // Level 1 inverse.
        const __m256d c0a = _mm256_permute2f128_pd(A0v, B0v, 0x20);
        const __m256d c1a = _mm256_permute2f128_pd(A0v, B0v, 0x31);
        const __m256d c0b = _mm256_permute2f128_pd(A1v, B1v, 0x20);
        const __m256d c1b = _mm256_permute2f128_pd(A1v, B1v, 0x31);

        const __m256d A0 = _mm256_mul_pd(hf, _mm256_add_pd(c0a, c1a));
        const __m256d R1 = _mm256_mul_pd(hf, _mm256_sub_pd(c0a, c1a));
        const __m256d I1 = _mm256_mul_pd(hf, _mm256_add_pd(c0b, c1b));
        const __m256d A1 = _mm256_mul_pd(hf, _mm256_sub_pd(c0b, c1b));
        const __m256d c1v = _mm256_set1_pd(t.c1);
        const __m256d s1v = _mm256_set1_pd(t.s1);
        const __m256d B0 = _mm256_add_pd(_mm256_mul_pd(c1v, R1), _mm256_mul_pd(s1v, I1));
        const __m256d B1 = _mm256_sub_pd(_mm256_mul_pd(c1v, I1), _mm256_mul_pd(s1v, R1));

        _mm256_storeu_pd(p,      A0);
        _mm256_storeu_pd(p + 4,  B0);
        _mm256_storeu_pd(p + 8,  A1);
        _mm256_storeu_pd(p + 12, B1);
#else
        norm_q_inv(p,      1, t.c4[0], t.c4[1]);
        norm_q_inv(p + 4,  1, t.c4[1], t.c4[0]);
        norm_q_inv(p + 8,  1, t.c4[2], t.c4[3]);
        norm_q_inv(p + 12, 1, t.c4[3], t.c4[2]);
        norm_q_inv(p,      2, t.c2[0], t.c2[1]);
        norm_q_inv(p + 8,  2, t.c2[1], t.c2[0]);
        norm_q_inv(p,      4, t.c1,    t.s1);
#endif
    }

    // The float64 depth-first walk: the float32 walk above (BFFT lines
    // 2530-2650) in double. BFFT runs its float64 forward through a
    // precomputed schedule of the same operations on the same blocks.

    struct DoubleSegment {
        double* data;
        int q;
        int m;
    };

    void run_fwd_res_segments(double* __restrict root, int root_q, int root_m) const {
        DoubleSegment stack[96];
        int stack_size = 0;
        stack[stack_size++] = DoubleSegment{root, root_q, root_m};

        while (stack_size > 0) {
            --stack_size;
            const DoubleSegment segment = stack[stack_size];
            double* __restrict v = segment.data;
            const int q = segment.q;
            const int m = segment.m;

            if (q >= 16) {
                norm2_fused(v, q, C[m], s_twiddle(m), C[2*m], s_twiddle(2*m), C[2*m+1], s_twiddle(2*m+1));
                const int qq = q >> 2;
                const int child_m = 4 * m;

                stack[stack_size++] = DoubleSegment{v + 3*q, qq, child_m + 3};
                stack[stack_size++] = DoubleSegment{v + 2*q, qq, child_m + 2};
                stack[stack_size++] = DoubleSegment{v + q,   qq, child_m + 1};
                stack[stack_size++] = DoubleSegment{v,       qq, child_m};
                continue;
            }

            if (q == 8) {
                norm_q_fwd(v, 8, C[m], s_twiddle(m));
                codelet_d3_tw_res(v, TW[2*m]);
                codelet_d3_tw_res(v + 16, TW[2*m + 1]);
                continue;
            }

            codelet_d3_tw_res(v, TW[m]);
        }
    }

    void residue_spine_tail_fwd(double* __restrict v) const {
        codelet_d3_tw_res(v + 16, TW[1]);
        binomial_fwd(v, 8);
        norm_q_fwd(v + 8, 2, C[1], s_twiddle(1));
        norm_q1_fwd(v + 8, C[2], s_twiddle(2));
        norm_q1_fwd(v + 12, C[3], s_twiddle(3));
        binomial_fwd(v, 4);
        norm_q1_fwd(v + 4, C[1], s_twiddle(1));
        binomial_fwd(v, 2);
    }

    // Fast depth-first double residue forward with fused input copy. Requires N >= 64.
    void forward_residues_recursive(const double* __restrict input, double* __restrict v) const {
        binomial_oop(input, v, N / 2);

        for (int h = N / 2; h >= 32; h >>= 1) {
            run_fwd_res_segments(v + h, h >> 2, 1);
            binomial_fwd(v, h >> 1);
        }

        residue_spine_tail_fwd(v);
    }

    // Time -> residues, double. Leaf m lands in (v[2m], v[2m+1]); v[0], v[1]
    // carry DC and Nyquist content exactly as in the double residue domain.
    void forward_residues(const double* __restrict input, double* __restrict v) const {
        if (N >= 64) {
            forward_residues_recursive(input, v);
            return;
        }
        __builtin_memcpy(v, input, sizeof(double) * N);
        forward_residues_inplace(v);
    }

    void rec_inv_res(double* __restrict v, int q, int m) const {
        if (q >= 16) {
            const int qq = q >> 2;
            rec_inv_res(v,       qq, 4*m);
            rec_inv_res(v + q,   qq, 4*m + 1);
            rec_inv_res(v + 2*q, qq, 4*m + 2);
            rec_inv_res(v + 3*q, qq, 4*m + 3);
            norm2_inv_fused(v, q, C[m], s_twiddle(m), C[2*m], s_twiddle(2*m), C[2*m+1], s_twiddle(2*m+1));
            return;
        }
        if (q == 8) {
            codelet_d3_tw_res_inv(v, TW[2*m]);
            codelet_d3_tw_res_inv(v + 16, TW[2*m + 1]);
            norm_q_inv(v, 8, C[m], s_twiddle(m));
            return;
        }
        codelet_d3_tw_res_inv(v, TW[m]);
    }

    void residue_spine_tail_inv(double* __restrict v) const {
        binomial_inv(v, 2);
        norm_q_inv(v + 4, 1, C[1], s_twiddle(1));
        binomial_inv(v, 4);
        norm_q_inv(v + 8, 1, C[2], s_twiddle(2));
        norm_q_inv(v + 12, 1, C[3], s_twiddle(3));
        norm_q_inv(v + 8, 2, C[1], s_twiddle(1));
        binomial_inv(v, 8);
        codelet_d3_tw_res_inv(v + 16, TW[1]);
    }

    // Exact reverse of forward_residues_recursive_f32. Requires N >= 64.
    // (BFFT runs its precomputed inverse schedule instead;
    // the schedule applies the same operations to the same disjoint blocks,
    // so the values are the same.)
    void inverse_residues_recursive(double* __restrict v) const {
        residue_spine_tail_inv(v);

        for (int h = 32; h <= N / 2; h <<= 1) {
            binomial_inv(v, h >> 1);
            rec_inv_res(v + h, h >> 2, 1);
        }

        binomial_inv(v, N / 2);
    }

    void forward_stage(double* __restrict v, int jj) const {
        const int s = N >> jj;
        const int h = s >> 1;
        const int q = s >> 2;
        const int m_end = 1 << jj;

        binomial_fwd(v, h);

        if (q == 1) {
            for (int m = 1; m < m_end; ++m) norm_q1_fwd(v + m*s, C[m], s_twiddle(m));
        } else if (q == 2) {
            for (int m = 1; m < m_end; ++m) norm_q2_fwd(v + m*s, C[m], s_twiddle(m));
        } else {
            for (int m = 1; m < m_end; ++m) norm_q_fwd(v + m*s, q, C[m], s_twiddle(m));
        }
    }

    void forward_residues_inplace(double* __restrict v) const {
        for (int jj = 0; jj < L - 1; ++jj) {
            forward_stage(v, jj);
        }
    }

    void complex_to_residues(const Complex64* __restrict X, double* __restrict v) const {
        v[0] = 0.5 * (X[0].re + X[N / 2].re);
        v[1] = 0.5 * (X[0].re - X[N / 2].re);

        for (int m = 1; m < N / 2; ++m) {
            const int k = IDX[m];
            v[2*m] = X[k].re;
            v[2*m + 1] = -X[k].im;
        }
    }

    void inverse_residues_inplace(double* __restrict v) const {
        if (N >= 64) {
            inverse_residues_recursive(v);
            return;
        }

        for (int jj = L - 2; jj >= 0; --jj) {
            const int s = N >> jj;
            const int h = s >> 1;
            const int q = s >> 2;
            const int m_end = 1 << jj;

            for (int m = m_end - 1; m > 0; --m) {
                norm_q_inv(v + m*s, q, C[m], s_twiddle(m));
            }

            binomial_inv(v, h);
        }
    }

};
