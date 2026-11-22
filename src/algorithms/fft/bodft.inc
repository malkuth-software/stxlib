// stx_fft's shifted mode: BFFT's BODFT kernel (src/detail/bodft_kernel.hpp
// at edf5f01, MIT, the owner's own; NOTICE.md). Included only by
// variant.inc, inside one SIMD tier's namespace (STXLIB_FFT_LEVEL is BFFT's
// BRUUN_LEVEL).
//
// BODFT computes the half-bin-shifted transform
//
//     H[k] = sum_{n=0}^{N-1} x[n] * exp(-2*pi*i*(k+1/2)*n/N),   k = 0..N/2-1
//
// directly: radix-4 decimation n = 4q + r, each child again a BODFT of
// length N/4, and the paired butterfly that lets output k and its partner
// kp = M-1-k share the three child twiddle products t, t^2, t^3. Nothing
// modulates the input. This file keeps that pathway exactly as BFFT has
// it (owner, 2026-11-22: copy it as its own kernel, never substitute
// modulate-then-FFT). The leaf pass, the radix-4 combines (AVX2 and 128-bit
// SoA forward paths, AVX2 inverse paths, the scalar reference), the
// ping-pong schedule and the digit-reversal permutation are BFFT's text,
// changed only by the mechanical transform described in dif.inc (fused
// multiply-adds spelled out, lanes.inc's names) and these:
//   - the twiddles, the permutation and the scratch live in the plan
//     (BodftTables); plan.cpp builds them, the twiddles from sincospi;
//   - BFFT's V4F_UZPRE, V4F_UZPIM and V4F_REV macros are in lanes.inc.

// ---------------------------------------------------------------------------
// Small complex helpers, generic over Complex64 and Complex32.
// ---------------------------------------------------------------------------


template <class CT>
static inline CT cadd(CT a, CT b) { return CT{a.re + b.re, a.im + b.im}; }
template <class CT>
static inline CT csub(CT a, CT b) { return CT{a.re - b.re, a.im - b.im}; }
template <class CT>
static inline CT cmul(CT a, CT b) {
    return CT{a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}
template <class CT, class R>
static inline CT cmul_r(CT a, R s) { return CT{a.re * s, a.im * s}; }
template <class CT>
static inline CT cconj(CT a) { return CT{a.re, -a.im}; }
// i * a   (rotate by +90 degrees)
template <class CT>
static inline CT cmuli(CT a) { return CT{-a.im, a.re}; }


#if STXLIB_FFT_LEVEL >= 2
static inline void bodft_uzp4_pd(const Complex64* __restrict src, int k,
                                 __m256d& re, __m256d& im) {
    const __m256d a = _mm256_loadu_pd(&src[k].re);
    const __m256d b = _mm256_loadu_pd(&src[k + 2].re);
    const __m256d lo = _mm256_unpacklo_pd(a, b);
    const __m256d hi = _mm256_unpackhi_pd(a, b);
    re = _mm256_permute4x64_pd(lo, 0xD8);
    im = _mm256_permute4x64_pd(hi, 0xD8);
}

static inline void bodft_store4_pd(Complex64* __restrict dst, int k,
                                   __m256d re, __m256d im) {
    const __m256d lo = _mm256_unpacklo_pd(re, im);
    const __m256d hi = _mm256_unpackhi_pd(re, im);
    _mm256_storeu_pd(&dst[k].re,     _mm256_permute2f128_pd(lo, hi, 0x20));
    _mm256_storeu_pd(&dst[k + 2].re, _mm256_permute2f128_pd(lo, hi, 0x31));
}

static inline __m256d bodft_rev4_pd(__m256d v) {
    return _mm256_permute4x64_pd(v, 0x1B);
}

static inline int bodft_combine_fwd_avx2_f64(const Complex64* __restrict tab,
                                             const Complex64* __restrict tab2,
                                             const Complex64* __restrict tab3,
                                             const Complex64* __restrict c0,
                                             const Complex64* __restrict c1,
                                             const Complex64* __restrict c2,
                                             const Complex64* __restrict c3,
                                             Complex64* __restrict out,
                                             int M, int half) {
    const __m256d zero = _mm256_setzero_pd();
    int k = 0;
    for (; k + 4 <= half; k += 4) {
        __m256d tre, tim, t2re, t2im, t3re, t3im;
        __m256d c0re, c0im, c1re, c1im, c2re, c2im, c3re, c3im;
        bodft_uzp4_pd(tab,  k, tre, tim);
        bodft_uzp4_pd(tab2, k, t2re, t2im);
        bodft_uzp4_pd(tab3, k, t3re, t3im);
        bodft_uzp4_pd(c0, k, c0re, c0im);
        bodft_uzp4_pd(c1, k, c1re, c1im);
        bodft_uzp4_pd(c2, k, c2re, c2im);
        bodft_uzp4_pd(c3, k, c3re, c3im);

        const __m256d b0re = c0re, b0im = c0im;
        const __m256d b1re = _mm256_sub_pd(_mm256_mul_pd(c1re, tre), _mm256_mul_pd(c1im, tim));
        const __m256d b1im = _mm256_add_pd(_mm256_mul_pd(c1re, tim), _mm256_mul_pd(c1im, tre));
        const __m256d b2re = _mm256_sub_pd(_mm256_mul_pd(c2re, t2re), _mm256_mul_pd(c2im, t2im));
        const __m256d b2im = _mm256_add_pd(_mm256_mul_pd(c2re, t2im), _mm256_mul_pd(c2im, t2re));
        const __m256d b3re = _mm256_sub_pd(_mm256_mul_pd(c3re, t3re), _mm256_mul_pd(c3im, t3im));
        const __m256d b3im = _mm256_add_pd(_mm256_mul_pd(c3re, t3im), _mm256_mul_pd(c3im, t3re));

        const __m256d e0re = _mm256_add_pd(b0re, b2re), e0im = _mm256_add_pd(b0im, b2im);
        const __m256d e1re = _mm256_sub_pd(b0re, b2re), e1im = _mm256_sub_pd(b0im, b2im);
        const __m256d o0re = _mm256_add_pd(b1re, b3re), o0im = _mm256_add_pd(b1im, b3im);
        const __m256d o1re = _mm256_sub_pd(b1re, b3re), o1im = _mm256_sub_pd(b1im, b3im);

        const __m256d yk_re = _mm256_add_pd(e0re, o0re), yk_im = _mm256_add_pd(e0im, o0im);
        const __m256d ykM_re = _mm256_add_pd(e1re, o1im), ykM_im = _mm256_sub_pd(e1im, o1re);
        const __m256d ykp_re = _mm256_sub_pd(e1re, o1im);
        const __m256d ykp_im = _mm256_sub_pd(zero, _mm256_add_pd(e1im, o1re));
        const __m256d ykpM_re = _mm256_sub_pd(e0re, o0re);
        const __m256d ykpM_im = _mm256_sub_pd(o0im, e0im);

        bodft_store4_pd(out, k, yk_re, yk_im);
        bodft_store4_pd(out, k + M, ykM_re, ykM_im);
        bodft_store4_pd(out, M - 4 - k, bodft_rev4_pd(ykp_re), bodft_rev4_pd(ykp_im));
        bodft_store4_pd(out, M - 4 - k + M, bodft_rev4_pd(ykpM_re), bodft_rev4_pd(ykpM_im));
    }
    return k;
}


static inline void bodft_uzp8_ps(const Complex32* __restrict src, int k,
                                 __m256& re, __m256& im) {
    const __m256 a = _mm256_loadu_ps(&src[k].re);
    const __m256 b = _mm256_loadu_ps(&src[k + 4].re);
    const __m256 lo = _mm256_unpacklo_ps(a, b);
    const __m256 hi = _mm256_unpackhi_ps(a, b);
    const __m256 re_pairs = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 im_pairs = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 2, 3, 2));
    re = _mm256_permutevar8x32_ps(re_pairs, _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7));
    im = _mm256_permutevar8x32_ps(im_pairs, _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7));
}

static inline void bodft_store8_ps(Complex32* __restrict dst, int k,
                                   __m256 re, __m256 im) {
    const __m256 lo = _mm256_unpacklo_ps(re, im);
    const __m256 hi = _mm256_unpackhi_ps(re, im);
    _mm256_storeu_ps(&dst[k].re,     _mm256_permute2f128_ps(lo, hi, 0x20));
    _mm256_storeu_ps(&dst[k + 4].re, _mm256_permute2f128_ps(lo, hi, 0x31));
}

static inline __m256 bodft_rev8_ps(__m256 v) {
    return _mm256_permutevar8x32_ps(v, _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0));
}

static inline int bodft_combine_fwd_avx2_f32(const Complex32* __restrict tab,
                                             const Complex32* __restrict tab2,
                                             const Complex32* __restrict tab3,
                                             const Complex32* __restrict c0,
                                             const Complex32* __restrict c1,
                                             const Complex32* __restrict c2,
                                             const Complex32* __restrict c3,
                                             Complex32* __restrict out,
                                             int M, int half) {
    const __m256 zero = _mm256_setzero_ps();
    int k = 0;
    for (; k + 8 <= half; k += 8) {
        __m256 tre, tim, t2re, t2im, t3re, t3im;
        __m256 c0re, c0im, c1re, c1im, c2re, c2im, c3re, c3im;
        bodft_uzp8_ps(tab,  k, tre, tim);
        bodft_uzp8_ps(tab2, k, t2re, t2im);
        bodft_uzp8_ps(tab3, k, t3re, t3im);
        bodft_uzp8_ps(c0, k, c0re, c0im);
        bodft_uzp8_ps(c1, k, c1re, c1im);
        bodft_uzp8_ps(c2, k, c2re, c2im);
        bodft_uzp8_ps(c3, k, c3re, c3im);

        const __m256 b0re = c0re, b0im = c0im;
        const __m256 b1re = _mm256_sub_ps(_mm256_mul_ps(c1re, tre), _mm256_mul_ps(c1im, tim));
        const __m256 b1im = _mm256_add_ps(_mm256_mul_ps(c1re, tim), _mm256_mul_ps(c1im, tre));
        const __m256 b2re = _mm256_sub_ps(_mm256_mul_ps(c2re, t2re), _mm256_mul_ps(c2im, t2im));
        const __m256 b2im = _mm256_add_ps(_mm256_mul_ps(c2re, t2im), _mm256_mul_ps(c2im, t2re));
        const __m256 b3re = _mm256_sub_ps(_mm256_mul_ps(c3re, t3re), _mm256_mul_ps(c3im, t3im));
        const __m256 b3im = _mm256_add_ps(_mm256_mul_ps(c3re, t3im), _mm256_mul_ps(c3im, t3re));

        const __m256 e0re = _mm256_add_ps(b0re, b2re), e0im = _mm256_add_ps(b0im, b2im);
        const __m256 e1re = _mm256_sub_ps(b0re, b2re), e1im = _mm256_sub_ps(b0im, b2im);
        const __m256 o0re = _mm256_add_ps(b1re, b3re), o0im = _mm256_add_ps(b1im, b3im);
        const __m256 o1re = _mm256_sub_ps(b1re, b3re), o1im = _mm256_sub_ps(b1im, b3im);

        const __m256 yk_re = _mm256_add_ps(e0re, o0re), yk_im = _mm256_add_ps(e0im, o0im);
        const __m256 ykM_re = _mm256_add_ps(e1re, o1im), ykM_im = _mm256_sub_ps(e1im, o1re);
        const __m256 ykp_re = _mm256_sub_ps(e1re, o1im);
        const __m256 ykp_im = _mm256_sub_ps(zero, _mm256_add_ps(e1im, o1re));
        const __m256 ykpM_re = _mm256_sub_ps(e0re, o0re);
        const __m256 ykpM_im = _mm256_sub_ps(o0im, e0im);

        bodft_store8_ps(out, k, yk_re, yk_im);
        bodft_store8_ps(out, k + M, ykM_re, ykM_im);
        bodft_store8_ps(out, M - 8 - k, bodft_rev8_ps(ykp_re), bodft_rev8_ps(ykp_im));
        bodft_store8_ps(out, M - 8 - k + M, bodft_rev8_ps(ykpM_re), bodft_rev8_ps(ykpM_im));
    }
    return k;
}


static inline int bodft_combine_inv_avx2_f32(const Complex32* __restrict tab,
                                             const Complex32* __restrict tab2,
                                             const Complex32* __restrict tab3,
                                             const Complex32* __restrict in,
                                             Complex32* __restrict c0,
                                             Complex32* __restrict c1,
                                             Complex32* __restrict c2,
                                             Complex32* __restrict c3,
                                             int M, int half) {
    const __m256 halfv = _mm256_set1_ps(0.5f);
    const __m256 zero = _mm256_setzero_ps();
    int k = 0;
    for (; k + 8 <= half; k += 8) {
        __m256 tre, tim, t2re, t2im, t3re, t3im;
        __m256 ykre, ykim, ykMre, ykMim;
        __m256 ykpre, ykpim, ykpMre, ykpMim;
        bodft_uzp8_ps(tab, k, tre, tim);
        bodft_uzp8_ps(tab2, k, t2re, t2im);
        bodft_uzp8_ps(tab3, k, t3re, t3im);
        bodft_uzp8_ps(in, k, ykre, ykim);
        bodft_uzp8_ps(in, k + M, ykMre, ykMim);
        bodft_uzp8_ps(in, M - 8 - k, ykpre, ykpim);
        bodft_uzp8_ps(in, M - 8 - k + M, ykpMre, ykpMim);
        ykpre = bodft_rev8_ps(ykpre);
        ykpim = bodft_rev8_ps(ykpim);
        ykpMre = bodft_rev8_ps(ykpMre);
        ykpMim = bodft_rev8_ps(ykpMim);

        const __m256 e0re = _mm256_mul_ps(halfv, _mm256_add_ps(ykre, ykpMre));
        const __m256 e0im = _mm256_mul_ps(halfv, _mm256_sub_ps(ykim, ykpMim));
        const __m256 o0re = _mm256_mul_ps(halfv, _mm256_sub_ps(ykre, ykpMre));
        const __m256 o0im = _mm256_mul_ps(halfv, _mm256_add_ps(ykim, ykpMim));
        const __m256 e1re = _mm256_mul_ps(halfv, _mm256_add_ps(ykMre, ykpre));
        const __m256 e1im = _mm256_mul_ps(halfv, _mm256_sub_ps(ykMim, ykpim));
        const __m256 diff_re = _mm256_sub_ps(ykMre, ykpre);
        const __m256 diff_im = _mm256_add_ps(ykMim, ykpim);
        const __m256 o1re = _mm256_mul_ps(halfv, _mm256_sub_ps(zero, diff_im));
        const __m256 o1im = _mm256_mul_ps(halfv, diff_re);

        const __m256 b0re = _mm256_mul_ps(halfv, _mm256_add_ps(e0re, e1re));
        const __m256 b0im = _mm256_mul_ps(halfv, _mm256_add_ps(e0im, e1im));
        const __m256 b2re = _mm256_mul_ps(halfv, _mm256_sub_ps(e0re, e1re));
        const __m256 b2im = _mm256_mul_ps(halfv, _mm256_sub_ps(e0im, e1im));
        const __m256 b1re = _mm256_mul_ps(halfv, _mm256_add_ps(o0re, o1re));
        const __m256 b1im = _mm256_mul_ps(halfv, _mm256_add_ps(o0im, o1im));
        const __m256 b3re = _mm256_mul_ps(halfv, _mm256_sub_ps(o0re, o1re));
        const __m256 b3im = _mm256_mul_ps(halfv, _mm256_sub_ps(o0im, o1im));

        const __m256 c1re = _mm256_add_ps(_mm256_mul_ps(tim, b1im), _mm256_mul_ps(tre, b1re));
        const __m256 c1im = _mm256_sub_ps(_mm256_mul_ps(tre, b1im), _mm256_mul_ps(tim, b1re));
        const __m256 c2re = _mm256_add_ps(_mm256_mul_ps(t2im, b2im), _mm256_mul_ps(t2re, b2re));
        const __m256 c2im = _mm256_sub_ps(_mm256_mul_ps(t2re, b2im), _mm256_mul_ps(t2im, b2re));
        const __m256 c3re = _mm256_add_ps(_mm256_mul_ps(t3im, b3im), _mm256_mul_ps(t3re, b3re));
        const __m256 c3im = _mm256_sub_ps(_mm256_mul_ps(t3re, b3im), _mm256_mul_ps(t3im, b3re));

        bodft_store8_ps(c0, k, b0re, b0im);
        bodft_store8_ps(c1, k, c1re, c1im);
        bodft_store8_ps(c2, k, c2re, c2im);
        bodft_store8_ps(c3, k, c3re, c3im);
    }
    return k;
}

static inline int bodft_combine_inv_avx2_f64(const Complex64* __restrict tab,
                                             const Complex64* __restrict tab2,
                                             const Complex64* __restrict tab3,
                                             const Complex64* __restrict in,
                                             Complex64* __restrict c0,
                                             Complex64* __restrict c1,
                                             Complex64* __restrict c2,
                                             Complex64* __restrict c3,
                                             int M, int half) {
    const __m256d halfv = _mm256_set1_pd(0.5);
    int k = 0;
    for (; k + 4 <= half; k += 4) {
        __m256d tre, tim, t2re, t2im, t3re, t3im;
        __m256d ykre, ykim, ykMre, ykMim;
        __m256d ykpre, ykpim, ykpMre, ykpMim;
        bodft_uzp4_pd(tab, k, tre, tim);
        bodft_uzp4_pd(tab2, k, t2re, t2im);
        bodft_uzp4_pd(tab3, k, t3re, t3im);
        bodft_uzp4_pd(in, k, ykre, ykim);
        bodft_uzp4_pd(in, k + M, ykMre, ykMim);
        bodft_uzp4_pd(in, M - 4 - k, ykpre, ykpim);
        bodft_uzp4_pd(in, M - 4 - k + M, ykpMre, ykpMim);
        ykpre = bodft_rev4_pd(ykpre);
        ykpim = bodft_rev4_pd(ykpim);
        ykpMre = bodft_rev4_pd(ykpMre);
        ykpMim = bodft_rev4_pd(ykpMim);

        const __m256d e0re = _mm256_mul_pd(halfv, _mm256_add_pd(ykre, ykpMre));
        const __m256d e0im = _mm256_mul_pd(halfv, _mm256_sub_pd(ykim, ykpMim));
        const __m256d o0re = _mm256_mul_pd(halfv, _mm256_sub_pd(ykre, ykpMre));
        const __m256d o0im = _mm256_mul_pd(halfv, _mm256_add_pd(ykim, ykpMim));
        const __m256d e1re = _mm256_mul_pd(halfv, _mm256_add_pd(ykMre, ykpre));
        const __m256d e1im = _mm256_mul_pd(halfv, _mm256_sub_pd(ykMim, ykpim));
        const __m256d diff_re = _mm256_sub_pd(ykMre, ykpre);
        const __m256d diff_im = _mm256_add_pd(ykMim, ykpim);
        const __m256d o1re = _mm256_mul_pd(halfv, _mm256_sub_pd(_mm256_setzero_pd(), diff_im));
        const __m256d o1im = _mm256_mul_pd(halfv, diff_re);

        const __m256d b0re = _mm256_mul_pd(halfv, _mm256_add_pd(e0re, e1re));
        const __m256d b0im = _mm256_mul_pd(halfv, _mm256_add_pd(e0im, e1im));
        const __m256d b2re = _mm256_mul_pd(halfv, _mm256_sub_pd(e0re, e1re));
        const __m256d b2im = _mm256_mul_pd(halfv, _mm256_sub_pd(e0im, e1im));
        const __m256d b1re = _mm256_mul_pd(halfv, _mm256_add_pd(o0re, o1re));
        const __m256d b1im = _mm256_mul_pd(halfv, _mm256_add_pd(o0im, o1im));
        const __m256d b3re = _mm256_mul_pd(halfv, _mm256_sub_pd(o0re, o1re));
        const __m256d b3im = _mm256_mul_pd(halfv, _mm256_sub_pd(o0im, o1im));

        const __m256d c1re = _mm256_add_pd(_mm256_mul_pd(tim, b1im), _mm256_mul_pd(tre, b1re));
        const __m256d c1im = _mm256_sub_pd(_mm256_mul_pd(tre, b1im), _mm256_mul_pd(tim, b1re));
        const __m256d c2re = _mm256_add_pd(_mm256_mul_pd(t2im, b2im), _mm256_mul_pd(t2re, b2re));
        const __m256d c2im = _mm256_sub_pd(_mm256_mul_pd(t2re, b2im), _mm256_mul_pd(t2im, b2re));
        const __m256d c3re = _mm256_add_pd(_mm256_mul_pd(t3im, b3im), _mm256_mul_pd(t3re, b3re));
        const __m256d c3im = _mm256_sub_pd(_mm256_mul_pd(t3re, b3im), _mm256_mul_pd(t3im, b3re));

        bodft_store4_pd(c0, k, b0re, b0im);
        bodft_store4_pd(c1, k, c1re, c1im);
        bodft_store4_pd(c2, k, c2re, c2im);
        bodft_store4_pd(c3, k, c3re, c3im);
    }
    return k;
}
#endif

// ---------------------------------------------------------------------------
// Forward double combine, 128-bit SoA SIMD specialization. Two output positions
// k, k+1 per iteration; children and twiddles transpose into real/imag lane
// registers so the three products are pure lanewise FMA. Partner positions kp,
// kp-1 are written by address arithmetic. Returns the (even) count handled.
// ---------------------------------------------------------------------------
#if STXLIB_FFT_LEVEL >= 1
static inline int bodft_combine_fwd_simd_f64(const Complex64* __restrict tab,
                                             const Complex64* __restrict tab2,
                                             const Complex64* __restrict tab3,
                                             const Complex64* __restrict c0,
                                             const Complex64* __restrict c1,
                                             const Complex64* __restrict c2,
                                             const Complex64* __restrict c3,
                                             Complex64* __restrict out,
                                             int M, int half) {
    const V2 zero = v2_set1(0.0);
    int k = 0;
    for (; k + 2 <= half; k += 2) {
        V2 a, b;
        a = v2_ld(&tab[k].re);  b = v2_ld(&tab[k + 1].re);
        const V2 tre = v2_unplo(a, b), tim = v2_unphi(a, b);
        a = v2_ld(&tab2[k].re); b = v2_ld(&tab2[k + 1].re);
        const V2 t2re = v2_unplo(a, b), t2im = v2_unphi(a, b);
        a = v2_ld(&tab3[k].re); b = v2_ld(&tab3[k + 1].re);
        const V2 t3re = v2_unplo(a, b), t3im = v2_unphi(a, b);

        a = v2_ld(&c0[k].re); b = v2_ld(&c0[k + 1].re);
        const V2 c0re = v2_unplo(a, b), c0im = v2_unphi(a, b);
        a = v2_ld(&c1[k].re); b = v2_ld(&c1[k + 1].re);
        const V2 c1re = v2_unplo(a, b), c1im = v2_unphi(a, b);
        a = v2_ld(&c2[k].re); b = v2_ld(&c2[k + 1].re);
        const V2 c2re = v2_unplo(a, b), c2im = v2_unphi(a, b);
        a = v2_ld(&c3[k].re); b = v2_ld(&c3[k + 1].re);
        const V2 c3re = v2_unplo(a, b), c3im = v2_unphi(a, b);

        const V2 b0re = c0re, b0im = c0im;
        const V2 b1re = v2_sub(v2_mul(c1re, tre), v2_mul(c1im, tim));
        const V2 b1im = v2_add(v2_mul(c1re, tim), v2_mul(c1im, tre));
        const V2 b2re = v2_sub(v2_mul(c2re, t2re), v2_mul(c2im, t2im));
        const V2 b2im = v2_add(v2_mul(c2re, t2im), v2_mul(c2im, t2re));
        const V2 b3re = v2_sub(v2_mul(c3re, t3re), v2_mul(c3im, t3im));
        const V2 b3im = v2_add(v2_mul(c3re, t3im), v2_mul(c3im, t3re));

        const V2 e0re = v2_add(b0re, b2re), e0im = v2_add(b0im, b2im);
        const V2 e1re = v2_sub(b0re, b2re), e1im = v2_sub(b0im, b2im);
        const V2 o0re = v2_add(b1re, b3re), o0im = v2_add(b1im, b3im);
        const V2 o1re = v2_sub(b1re, b3re), o1im = v2_sub(b1im, b3im);

        const V2 yk_re = v2_add(e0re, o0re), yk_im = v2_add(e0im, o0im);
        const V2 ykM_re = v2_add(e1re, o1im), ykM_im = v2_sub(e1im, o1re);
        const V2 ykp_re = v2_sub(e1re, o1im);
        const V2 ykp_im = v2_sub(zero, v2_add(e1im, o1re));
        const V2 ykpM_re = v2_sub(e0re, o0re);
        const V2 ykpM_im = v2_sub(o0im, e0im);

        v2_st(&out[k].re,         v2_unplo(yk_re, yk_im));
        v2_st(&out[k + 1].re,     v2_unphi(yk_re, yk_im));
        v2_st(&out[k + M].re,     v2_unplo(ykM_re, ykM_im));
        v2_st(&out[k + 1 + M].re, v2_unphi(ykM_re, ykM_im));
        v2_st(&out[M - 1 - k].re,     v2_unplo(ykp_re, ykp_im));
        v2_st(&out[M - 2 - k].re,     v2_unphi(ykp_re, ykp_im));
        v2_st(&out[M - 1 - k + M].re, v2_unplo(ykpM_re, ykpM_im));
        v2_st(&out[M - 2 - k + M].re, v2_unphi(ykpM_re, ykpM_im));
    }
    return k;
}

// Forward float combine: same SoA transport flow, four positions per iteration
// through the 128-bit float vector. Structurally identical to the f64 path.
static inline int bodft_combine_fwd_simd_f32(const Complex32* __restrict tab,
                                             const Complex32* __restrict tab2,
                                             const Complex32* __restrict tab3,
                                             const Complex32* __restrict c0,
                                             const Complex32* __restrict c1,
                                             const Complex32* __restrict c2,
                                             const Complex32* __restrict c3,
                                             Complex32* __restrict out,
                                             int M, int half) {
    const V4F zero = v4f_zero();
    int k = 0;
    for (; k + 4 <= half; k += 4) {
        V4F A, B;
        A = v4f_ld(&tab[k].re);  B = v4f_ld(&tab[k + 2].re);
        const V4F tre = v4f_uzpre(A, B), tim = v4f_uzpim(A, B);
        A = v4f_ld(&tab2[k].re); B = v4f_ld(&tab2[k + 2].re);
        const V4F t2re = v4f_uzpre(A, B), t2im = v4f_uzpim(A, B);
        A = v4f_ld(&tab3[k].re); B = v4f_ld(&tab3[k + 2].re);
        const V4F t3re = v4f_uzpre(A, B), t3im = v4f_uzpim(A, B);

        A = v4f_ld(&c0[k].re); B = v4f_ld(&c0[k + 2].re);
        const V4F c0re = v4f_uzpre(A, B), c0im = v4f_uzpim(A, B);
        A = v4f_ld(&c1[k].re); B = v4f_ld(&c1[k + 2].re);
        const V4F c1re = v4f_uzpre(A, B), c1im = v4f_uzpim(A, B);
        A = v4f_ld(&c2[k].re); B = v4f_ld(&c2[k + 2].re);
        const V4F c2re = v4f_uzpre(A, B), c2im = v4f_uzpim(A, B);
        A = v4f_ld(&c3[k].re); B = v4f_ld(&c3[k + 2].re);
        const V4F c3re = v4f_uzpre(A, B), c3im = v4f_uzpim(A, B);

        const V4F b0re = c0re, b0im = c0im;
        const V4F b1re = v4f_sub(v4f_mul(c1re, tre), v4f_mul(c1im, tim));
        const V4F b1im = v4f_add(v4f_mul(c1re, tim), v4f_mul(c1im, tre));
        const V4F b2re = v4f_sub(v4f_mul(c2re, t2re), v4f_mul(c2im, t2im));
        const V4F b2im = v4f_add(v4f_mul(c2re, t2im), v4f_mul(c2im, t2re));
        const V4F b3re = v4f_sub(v4f_mul(c3re, t3re), v4f_mul(c3im, t3im));
        const V4F b3im = v4f_add(v4f_mul(c3re, t3im), v4f_mul(c3im, t3re));

        const V4F e0re = v4f_add(b0re, b2re), e0im = v4f_add(b0im, b2im);
        const V4F e1re = v4f_sub(b0re, b2re), e1im = v4f_sub(b0im, b2im);
        const V4F o0re = v4f_add(b1re, b3re), o0im = v4f_add(b1im, b3im);
        const V4F o1re = v4f_sub(b1re, b3re), o1im = v4f_sub(b1im, b3im);

        const V4F yk_re = v4f_add(e0re, o0re), yk_im = v4f_add(e0im, o0im);
        const V4F ykM_re = v4f_add(e1re, o1im), ykM_im = v4f_sub(e1im, o1re);
        const V4F ykp_re = v4f_sub(e1re, o1im);
        const V4F ykp_im = v4f_sub(zero, v4f_add(e1im, o1re));
        const V4F ykpM_re = v4f_sub(e0re, o0re);
        const V4F ykpM_im = v4f_sub(o0im, e0im);

        v4f_st(&out[k].re,         v4f_ziplo(yk_re, yk_im));
        v4f_st(&out[k + 2].re,     v4f_ziphi(yk_re, yk_im));
        v4f_st(&out[k + M].re,     v4f_ziplo(ykM_re, ykM_im));
        v4f_st(&out[k + 2 + M].re, v4f_ziphi(ykM_re, ykM_im));
        const V4F pr = v4f_rev(ykp_re),  pi = v4f_rev(ykp_im);
        const V4F qr = v4f_rev(ykpM_re), qi = v4f_rev(ykpM_im);
        v4f_st(&out[M - 4 - k].re,     v4f_ziplo(pr, pi));
        v4f_st(&out[M - 2 - k].re,     v4f_ziphi(pr, pi));
        v4f_st(&out[M - 4 - k + M].re, v4f_ziplo(qr, qi));
        v4f_st(&out[M - 2 - k + M].re, v4f_ziphi(qr, qi));
    }
    return k;
}
#endif

// One forward radix-4 combine: 4 child blocks c0..c3 (each `half` complex) ->
// one parent block out (2*M complex). SIMD bulk for double/float plus the exact
// scalar tail/reference.
template <class CT, class RT>
static inline void combine_fwd(const CT* __restrict tab, const CT* __restrict tab2,
                               const CT* __restrict tab3, const CT* __restrict c0,
                               const CT* __restrict c1, const CT* __restrict c2,
                               const CT* __restrict c3, CT* __restrict out,
                               int M, int half) {
    int k = 0;
#if STXLIB_FFT_LEVEL >= 2
    if constexpr (sizeof(RT) == 8) {
        k = bodft_combine_fwd_avx2_f64(
            reinterpret_cast<const Complex64*>(tab),
            reinterpret_cast<const Complex64*>(tab2),
            reinterpret_cast<const Complex64*>(tab3),
            reinterpret_cast<const Complex64*>(c0),
            reinterpret_cast<const Complex64*>(c1),
            reinterpret_cast<const Complex64*>(c2),
            reinterpret_cast<const Complex64*>(c3),
            reinterpret_cast<Complex64*>(out), M, half);
    }
#endif
#if STXLIB_FFT_LEVEL >= 2
    if constexpr (sizeof(RT) == 4) {
        k = bodft_combine_fwd_avx2_f32(
            reinterpret_cast<const Complex32*>(tab),
            reinterpret_cast<const Complex32*>(tab2),
            reinterpret_cast<const Complex32*>(tab3),
            reinterpret_cast<const Complex32*>(c0),
            reinterpret_cast<const Complex32*>(c1),
            reinterpret_cast<const Complex32*>(c2),
            reinterpret_cast<const Complex32*>(c3),
            reinterpret_cast<Complex32*>(out), M, half);
    }
#endif
#if STXLIB_FFT_LEVEL >= 1
    if constexpr (sizeof(RT) == 8) {
        if (k == 0) k = bodft_combine_fwd_simd_f64(
            reinterpret_cast<const Complex64*>(tab),
            reinterpret_cast<const Complex64*>(tab2),
            reinterpret_cast<const Complex64*>(tab3),
            reinterpret_cast<const Complex64*>(c0),
            reinterpret_cast<const Complex64*>(c1),
            reinterpret_cast<const Complex64*>(c2),
            reinterpret_cast<const Complex64*>(c3),
            reinterpret_cast<Complex64*>(out), M, half);
    } else {
        if (k == 0) {
            k = bodft_combine_fwd_simd_f32(
                reinterpret_cast<const Complex32*>(tab),
                reinterpret_cast<const Complex32*>(tab2),
                reinterpret_cast<const Complex32*>(tab3),
                reinterpret_cast<const Complex32*>(c0),
                reinterpret_cast<const Complex32*>(c1),
                reinterpret_cast<const Complex32*>(c2),
                reinterpret_cast<const Complex32*>(c3),
                reinterpret_cast<Complex32*>(out), M, half);
        }
    }
#endif
    for (; k < half; ++k) {
        const CT t = tab[k];
        const CT b0 = c0[k];
        const CT b1 = cmul(t, c1[k]);
        const CT b2 = cmul(tab2[k], c2[k]);
        const CT b3 = cmul(tab3[k], c3[k]);
        const CT e0 = cadd(b0, b2);
        const CT e1 = csub(b0, b2);
        const CT o0 = cadd(b1, b3);
        const CT o1 = csub(b1, b3);
        const int kp = M - 1 - k;
        const CT io1 = cmuli(o1);
        out[k]      = cadd(e0, o0);
        out[k + M]  = csub(e1, io1);
        out[kp]     = cconj(cadd(e1, io1));
        out[kp + M] = cconj(csub(e0, o0));
    }
}

// One inverse radix-4 combine: parent block in (2*M complex) -> 4 child blocks
// c0..c3 (each `half`). Exact algebraic inverse of combine_fwd. Scalar in both
// precisions.
template <class CT, class RT>
static inline void combine_inv(const CT* __restrict tab, const CT* __restrict tab2,
                               const CT* __restrict tab3, const CT* __restrict in,
                               CT* __restrict c0, CT* __restrict c1, CT* __restrict c2,
                               CT* __restrict c3, int M, int half) {
    int k = 0;
#if STXLIB_FFT_LEVEL >= 2
    if constexpr (sizeof(RT) == 8) {
        k = bodft_combine_inv_avx2_f64(
            reinterpret_cast<const Complex64*>(tab),
            reinterpret_cast<const Complex64*>(tab2),
            reinterpret_cast<const Complex64*>(tab3),
            reinterpret_cast<const Complex64*>(in),
            reinterpret_cast<Complex64*>(c0),
            reinterpret_cast<Complex64*>(c1),
            reinterpret_cast<Complex64*>(c2),
            reinterpret_cast<Complex64*>(c3), M, half);
    }
    if constexpr (sizeof(RT) == 4) {
        k = bodft_combine_inv_avx2_f32(
            reinterpret_cast<const Complex32*>(tab),
            reinterpret_cast<const Complex32*>(tab2),
            reinterpret_cast<const Complex32*>(tab3),
            reinterpret_cast<const Complex32*>(in),
            reinterpret_cast<Complex32*>(c0),
            reinterpret_cast<Complex32*>(c1),
            reinterpret_cast<Complex32*>(c2),
            reinterpret_cast<Complex32*>(c3), M, half);
    }
#endif
    const RT h = static_cast<RT>(0.5);
    for (; k < half; ++k) {
        const int kp = M - 1 - k;
        const CT Yk   = in[k];
        const CT YkM  = in[k + M];
        const CT Ykp  = in[kp];
        const CT YkpM = in[kp + M];

        const CT e0 = cmul_r(cadd(Yk, cconj(YkpM)), h);
        const CT o0 = cmul_r(csub(Yk, cconj(YkpM)), h);
        const CT e1 = cmul_r(cadd(YkM, cconj(Ykp)), h);
        const CT o1 = cmul_r(cmuli(csub(YkM, cconj(Ykp))), h);

        const CT b0 = cmul_r(cadd(e0, e1), h);
        const CT b2 = cmul_r(csub(e0, e1), h);
        const CT b1 = cmul_r(cadd(o0, o1), h);
        const CT b3 = cmul_r(csub(o0, o1), h);

        c0[k] = b0;
        c1[k] = cmul(cconj(tab[k]), b1);
        c2[k] = cmul(cconj(tab2[k]), b2);
        c3[k] = cmul(cconj(tab3[k]), b3);
    }
}


// ---------------------------------------------------------------------------
// BodftKernel: BFFT's plan_t (a reusable BODFT of one power-of-two size,
// N >= 2) with its tables in the plan (plan_state.hpp, BodftTables; built
// by plan.cpp from sincospi) instead of members. Forward maps N real samples
// to N/2 packed complex bins; inverse maps them back exactly (the packed
// transform is its own exact inverse, no 1/N scaling). CT is the complex
// element type (Complex64 or Complex32), RT its scalar, LevelT the plan's
// per-level twiddle record. BFFT's mutable scratch is the plan's work
// buffer, so forward and inverse write only through the scratch pointer.
// ---------------------------------------------------------------------------
template <class CT, class RT, class LevelT>
class BodftKernel final {
public:
    BodftKernel(const BodftTables& tables, const LevelT* levels, CT root4, CT* scratch) noexcept
        : n_(tables.n), leafsize_(tables.leafsize), leafbins_(tables.leafbins), numleaves_(tables.numleaves),
          passes_(tables.passes), root4_(root4), perm_(tables.perm), levels_(levels), scratch_(scratch) {}

    int size() const noexcept { return n_; }
    int bins() const noexcept { return n_ / 2; }


    // Forward: real input (length N) -> packed complex (length N/2).
    void forward(const RT* __restrict input, CT* __restrict output) const {
        const int bins = n_ / 2;
        if (passes_ == 0) {                 // N == leafsize: leaf is the whole transform
            leaf_forward(input, output);
            return;
        }
        CT* bufA = scratch_;
        CT* bufB = scratch_ + bins;
        leaf_forward(input, bufA);

        const CT* cur_in = bufA;
        CT* cur_out = bufB;
        for (int s = leafsize_ * 4; s <= n_; s <<= 2) {
            int next_s = s << 2;
            if (next_s > n_) {
                cur_out = output;
            }
            combine_pass_fwd(cur_in, cur_out, s);
            cur_in = cur_out;
            if (cur_out == output) {
                break;
            }
            if (cur_out == bufA) {
                cur_out = bufB;
            } else {
                cur_out = bufA;
            }
        }
    }

    // Inverse: packed complex (length N/2) -> real output (length N).
    void inverse(const CT* __restrict input, RT* __restrict output) const {
        const int bins = n_ / 2;
        if (passes_ == 0) {
            leaf_inverse(input, output);
            return;
        }
        CT* bufA = scratch_;
        CT* bufB = scratch_ + bins;

        const CT* cur_in = input;
        CT* cur_out = bufA;
        for (int s = n_; s >= leafsize_ * 4; s >>= 2) {
            combine_pass_inv(cur_in, cur_out, s);
            cur_in = cur_out;
            if (cur_out == bufA) {
                cur_out = bufB;
            } else {
                cur_out = bufA;
            }
        }
        leaf_inverse(cur_in, output);
    }

private:
    void leaf_forward(const RT* __restrict x, CT* __restrict dst) const {
        if (leafbins_ == 1) {               // radix-2 leaf: H[0] = x0 - i*x1
            for (int L = 0; L < numleaves_; ++L) {
                const int i0 = perm_[static_cast<std::size_t>(2 * L)];
                const int i1 = perm_[static_cast<std::size_t>(2 * L + 1)];
                dst[L] = CT{x[i0], -x[i1]};
            }
        } else {                            // radix-4 leaf
            const CT t = root4_, t2 = cmul(t, t), t3 = cmul(t2, t);
            for (int L = 0; L < numleaves_; ++L) {
                const int* p = &perm_[static_cast<std::size_t>(4 * L)];
                const CT b0 = CT{x[p[0]], static_cast<RT>(0)};
                const CT b1 = cmul_r(t, x[p[1]]);
                const CT b2 = cmul_r(t2, x[p[2]]);
                const CT b3 = cmul_r(t3, x[p[3]]);
                dst[2 * L]     = cadd(cadd(b0, b1), cadd(b2, b3));
                dst[2 * L + 1] = cadd(csub(b0, b2), cmuli(csub(b3, b1)));
            }
        }
    }

    void leaf_inverse(const CT* __restrict src, RT* __restrict x) const {
        if (leafbins_ == 1) {
            for (int L = 0; L < numleaves_; ++L) {
                x[perm_[static_cast<std::size_t>(2 * L)]] = src[L].re;
                x[perm_[static_cast<std::size_t>(2 * L + 1)]] = -src[L].im;
            }
        } else {
            const CT t = root4_, t2 = cmul(t, t), t3 = cmul(t2, t);
            const RT h = static_cast<RT>(0.5);
            for (int L = 0; L < numleaves_; ++L) {
                const CT Yk = src[2 * L], YkM = src[2 * L + 1];
                const CT e0 = cmul_r(cadd(Yk, cconj(YkM)), h);
                const CT o0 = cmul_r(csub(Yk, cconj(YkM)), h);
                const CT e1 = cmul_r(cadd(YkM, cconj(Yk)), h);
                const CT o1 = cmul_r(cmuli(csub(YkM, cconj(Yk))), h);
                const CT b0 = cmul_r(cadd(e0, e1), h);
                const CT b2 = cmul_r(csub(e0, e1), h);
                const CT b1 = cmul_r(cadd(o0, o1), h);
                const CT b3 = cmul_r(csub(o0, o1), h);
                const int* p = &perm_[static_cast<std::size_t>(4 * L)];
                x[p[0]] = b0.re;
                x[p[1]] = cmul(cconj(t), b1).re;
                x[p[2]] = cmul(cconj(t2), b2).re;
                x[p[3]] = cmul(cconj(t3), b3).re;
            }
        }
    }

    // Forward combine pass at parent size s: src holds child blocks, dst parents.
    void combine_pass_fwd(const CT* __restrict src, CT* __restrict dst, int s) const {
        const int lg = ilog2_pow2(s);
        const int M = s / 4;
        const int cb = s / 8;               // child packed length = half
        const CT* tab = levels_[lg].t1;
        const CT* tab2 = levels_[lg].t2;
        const CT* tab3 = levels_[lg].t3;
        const int nump = n_ / s;
        const int pblk = s / 2;
        for (int g = 0; g < nump; ++g) {
            const int off = g * pblk;
            combine_fwd<CT, RT>(tab, tab2, tab3, src + off, src + off + cb,
                                src + off + 2 * cb, src + off + 3 * cb,
                                dst + off, M, cb);
        }
    }

    // Inverse combine pass at parent size s: src holds parents, dst child blocks.
    void combine_pass_inv(const CT* __restrict src, CT* __restrict dst, int s) const {
        const int lg = ilog2_pow2(s);
        const int M = s / 4;
        const int cb = s / 8;
        const CT* tab = levels_[lg].t1;
        const CT* tab2 = levels_[lg].t2;
        const CT* tab3 = levels_[lg].t3;
        const int nump = n_ / s;
        const int pblk = s / 2;
        for (int g = 0; g < nump; ++g) {
            const int off = g * pblk;
            combine_inv<CT, RT>(tab, tab2, tab3, src + off, dst + off,
                                dst + off + cb, dst + off + 2 * cb,
                                dst + off + 3 * cb, M, cb);
        }
    }


    int n_;
    int leafsize_;
    int leafbins_;
    int numleaves_;
    int passes_;
    CT root4_;
    const int* perm_;
    const LevelT* levels_;
    CT* scratch_;
};
