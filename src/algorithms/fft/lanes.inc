// stx_fft's vector vocabulary for one SIMD tier: BFFT's macros of
// src/detail/bruun_simd_backend.hpp and bodft_kernel.hpp at edf5f01 (V2_*,
// V4F_*, V4D_*), as inline functions with the same meaning, lower-cased
// (V2_ADD is v2_add). Included only by variant.inc, inside the tier's
// namespace and its target attribute; the intrinsic headers are included
// there first.
//
//   V2   two doubles  (SSE2 __m128d, NEON float64x2_t; scalar: two lanes)
//   V4F  four floats  (SSE __m128,   NEON float32x4_t;  scalar: four lanes)
//   V4D  four doubles (AVX __m256d)                      STXLIB_FFT_LEVEL == 2
//
// The scalar tier (STXLIB_FFT_LEVEL 0) has V2 and V4F too, as plain arrays
// of lanes with one operation per lane. The DIT kernel (dit.inc) is BFFT's
// mirrored-lane walk, written only in V2 and V4F; at the scalar tier it runs
// on these lane arrays, so each lane computes exactly what the vector lane
// computes and the tiers agree by construction. The DIF and BODFT kernels
// keep BFFT's own scalar text at that tier and never use them.
//
// BFFT's fused V2_MADD, V2_MSUB (and V4F_, V4D_) are not here: the kernels
// spell the multiply and the add or subtract (A1 rule 1), so every tier
// rounds as the scalar code does. Its compare and select macros are unused
// by the kernels and not copied.
//
// The fused measuring variant (STXLIB_FFT_FUSED, compiled only into
// stxlib_fft_bench, never into the library): v2_mul, v4f_mul and v4d_mul
// return a product not yet rounded, and an add or subtract that takes one
// is the fused multiply-add BFFT spells there (V2_MADD(a, b, c) is
// v2_add(a, v2_mul(b, c)) in the kernels' text, so it fuses back into
// exactly BFFT's operation). A product used any other way is rounded first.
// The variant measures what A1 rule 1 costs; its results differ from the
// scalar tier's in the last bits and are never a plan's.

#if STXLIB_FFT_LEVEL == 0

struct V2 final {
    double lane[2];
};
inline V2 v2_ld(const double* p) noexcept { return V2{{p[0], p[1]}}; }
inline void v2_st(double* p, V2 a) noexcept {
    p[0] = a.lane[0];
    p[1] = a.lane[1];
}
inline V2 v2_add(V2 a, V2 b) noexcept { return V2{{a.lane[0] + b.lane[0], a.lane[1] + b.lane[1]}}; }
inline V2 v2_sub(V2 a, V2 b) noexcept { return V2{{a.lane[0] - b.lane[0], a.lane[1] - b.lane[1]}}; }
inline V2 v2_mul(V2 a, V2 b) noexcept { return V2{{a.lane[0] * b.lane[0], a.lane[1] * b.lane[1]}}; }
inline V2 v2_set1(double x) noexcept { return V2{{x, x}}; }
inline V2 v2_setlh(double l, double h) noexcept { return V2{{l, h}}; }
inline V2 v2_unplo(V2 a, V2 b) noexcept { return V2{{a.lane[0], b.lane[0]}}; }
inline V2 v2_unphi(V2 a, V2 b) noexcept { return V2{{a.lane[1], b.lane[1]}}; }
inline double v2_lane0(V2 a) noexcept { return a.lane[0]; }
inline double v2_lane1(V2 a) noexcept { return a.lane[1]; }

struct V4F final {
    float lane[4];
};
inline V4F v4f_ld(const float* p) noexcept { return V4F{{p[0], p[1], p[2], p[3]}}; }
inline void v4f_st(float* p, V4F a) noexcept {
    p[0] = a.lane[0];
    p[1] = a.lane[1];
    p[2] = a.lane[2];
    p[3] = a.lane[3];
}
inline V4F v4f_add(V4F a, V4F b) noexcept {
    return V4F{{a.lane[0] + b.lane[0], a.lane[1] + b.lane[1], a.lane[2] + b.lane[2], a.lane[3] + b.lane[3]}};
}
inline V4F v4f_sub(V4F a, V4F b) noexcept {
    return V4F{{a.lane[0] - b.lane[0], a.lane[1] - b.lane[1], a.lane[2] - b.lane[2], a.lane[3] - b.lane[3]}};
}
inline V4F v4f_mul(V4F a, V4F b) noexcept {
    return V4F{{a.lane[0] * b.lane[0], a.lane[1] * b.lane[1], a.lane[2] * b.lane[2], a.lane[3] * b.lane[3]}};
}
inline V4F v4f_set1(float x) noexcept { return V4F{{x, x, x, x}}; }
inline V4F v4f_set4(float a, float b, float c, float d) noexcept { return V4F{{a, b, c, d}}; }
inline V4F v4f_catlo(V4F a, V4F b) noexcept { return V4F{{a.lane[0], a.lane[1], b.lane[0], b.lane[1]}}; }
inline V4F v4f_cathi(V4F a, V4F b) noexcept { return V4F{{a.lane[2], a.lane[3], b.lane[2], b.lane[3]}}; }
inline V4F v4f_ziplo(V4F a, V4F b) noexcept { return V4F{{a.lane[0], b.lane[0], a.lane[1], b.lane[1]}}; }
inline V4F v4f_ziphi(V4F a, V4F b) noexcept { return V4F{{a.lane[2], b.lane[2], a.lane[3], b.lane[3]}}; }

#endif

#if STXLIB_FFT_LEVEL >= 1 && defined(STXLIB_FFT_X86)

using V2 = __m128d;
inline V2 v2_ld(const double* p) noexcept { return _mm_loadu_pd(p); }
inline void v2_st(double* p, V2 a) noexcept { _mm_storeu_pd(p, a); }
inline V2 v2_add(V2 a, V2 b) noexcept { return _mm_add_pd(a, b); }
inline V2 v2_sub(V2 a, V2 b) noexcept { return _mm_sub_pd(a, b); }
#if !defined(STXLIB_FFT_FUSED)
inline V2 v2_mul(V2 a, V2 b) noexcept { return _mm_mul_pd(a, b); }
#endif
inline V2 v2_set1(double x) noexcept { return _mm_set1_pd(x); }
inline V2 v2_setlh(double l, double h) noexcept { return _mm_set_pd(h, l); }
inline V2 v2_unplo(V2 a, V2 b) noexcept { return _mm_unpacklo_pd(a, b); }
inline V2 v2_unphi(V2 a, V2 b) noexcept { return _mm_unpackhi_pd(a, b); }
inline V2 v2_dup0(V2 a) noexcept { return _mm_unpacklo_pd(a, a); }
inline V2 v2_dup1(V2 a) noexcept { return _mm_unpackhi_pd(a, a); }
inline V2 v2_neghi(V2 a) noexcept { return _mm_xor_pd(a, _mm_set_pd(-0.0, 0.0)); }
inline V2 v2_swap(V2 a) noexcept { return _mm_shuffle_pd(a, a, 1); }
inline double v2_lane0(V2 a) noexcept { return _mm_cvtsd_f64(a); }
inline double v2_lane1(V2 a) noexcept { return _mm_cvtsd_f64(_mm_unpackhi_pd(a, a)); }

using V4F = __m128;
inline V4F v4f_ld(const float* p) noexcept { return _mm_loadu_ps(p); }
inline void v4f_st(float* p, V4F a) noexcept { _mm_storeu_ps(p, a); }
inline V4F v4f_add(V4F a, V4F b) noexcept { return _mm_add_ps(a, b); }
inline V4F v4f_sub(V4F a, V4F b) noexcept { return _mm_sub_ps(a, b); }
#if !defined(STXLIB_FFT_FUSED)
inline V4F v4f_mul(V4F a, V4F b) noexcept { return _mm_mul_ps(a, b); }
#endif
inline V4F v4f_set1(float x) noexcept { return _mm_set1_ps(x); }
inline V4F v4f_set4(float a, float b, float c, float d) noexcept { return _mm_setr_ps(a, b, c, d); }
inline V4F v4f_zero() noexcept { return _mm_setzero_ps(); }
inline V4F v4f_catlo(V4F a, V4F b) noexcept { return _mm_movelh_ps(a, b); }
inline V4F v4f_cathi(V4F a, V4F b) noexcept { return _mm_movehl_ps(b, a); }
inline V4F v4f_ziplo(V4F a, V4F b) noexcept { return _mm_unpacklo_ps(a, b); }
inline V4F v4f_ziphi(V4F a, V4F b) noexcept { return _mm_unpackhi_ps(a, b); }
inline V4F v4f_swap_pairs(V4F a) noexcept { return _mm_shuffle_ps(a, a, 0xB1); }
inline V4F v4f_swap_halves(V4F a) noexcept { return _mm_shuffle_ps(a, a, 0x4E); }
// BODFT's deinterleave and reverse (bodft_kernel.hpp, V4F_UZPRE/UZPIM/REV).
inline V4F v4f_uzpre(V4F a, V4F b) noexcept { return _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)); }
inline V4F v4f_uzpim(V4F a, V4F b) noexcept { return _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)); }
inline V4F v4f_rev(V4F a) noexcept { return _mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 1, 2, 3)); }

#elif STXLIB_FFT_LEVEL >= 1 && defined(STXLIB_FFT_NEON)

using V2 = float64x2_t;
inline V2 v2_ld(const double* p) noexcept { return vld1q_f64(p); }
inline void v2_st(double* p, V2 a) noexcept { vst1q_f64(p, a); }
inline V2 v2_add(V2 a, V2 b) noexcept { return vaddq_f64(a, b); }
inline V2 v2_sub(V2 a, V2 b) noexcept { return vsubq_f64(a, b); }
#if !defined(STXLIB_FFT_FUSED)
inline V2 v2_mul(V2 a, V2 b) noexcept { return vmulq_f64(a, b); }
#endif
inline V2 v2_set1(double x) noexcept { return vdupq_n_f64(x); }
inline V2 v2_setlh(double l, double h) noexcept { return vcombine_f64(vdup_n_f64(l), vdup_n_f64(h)); }
inline V2 v2_unplo(V2 a, V2 b) noexcept { return vtrn1q_f64(a, b); }
inline V2 v2_unphi(V2 a, V2 b) noexcept { return vtrn2q_f64(a, b); }
inline V2 v2_dup0(V2 a) noexcept { return vdupq_laneq_f64(a, 0); }
inline V2 v2_dup1(V2 a) noexcept { return vdupq_laneq_f64(a, 1); }
inline V2 v2_neghi(V2 a) noexcept {
    // BFFT's bruun_neghi: flip the sign bit of the high lane.
    const uint64x2_t sign = vcombine_u64(vdup_n_u64(0), vdup_n_u64(0x8000000000000000ULL));
    return vreinterpretq_f64_u64(veorq_u64(vreinterpretq_u64_f64(a), sign));
}
inline V2 v2_swap(V2 a) noexcept { return vextq_f64(a, a, 1); }
inline double v2_lane0(V2 a) noexcept { return vgetq_lane_f64(a, 0); }
inline double v2_lane1(V2 a) noexcept { return vgetq_lane_f64(a, 1); }

using V4F = float32x4_t;
inline V4F v4f_ld(const float* p) noexcept { return vld1q_f32(p); }
inline void v4f_st(float* p, V4F a) noexcept { vst1q_f32(p, a); }
inline V4F v4f_add(V4F a, V4F b) noexcept { return vaddq_f32(a, b); }
inline V4F v4f_sub(V4F a, V4F b) noexcept { return vsubq_f32(a, b); }
#if !defined(STXLIB_FFT_FUSED)
inline V4F v4f_mul(V4F a, V4F b) noexcept { return vmulq_f32(a, b); }
#endif
inline V4F v4f_set1(float x) noexcept { return vdupq_n_f32(x); }
inline V4F v4f_set4(float a, float b, float c, float d) noexcept {
    const float lanes[4] = {a, b, c, d};
    return vld1q_f32(lanes);
}
inline V4F v4f_zero() noexcept { return vdupq_n_f32(0.0F); }
inline V4F v4f_catlo(V4F a, V4F b) noexcept { return vcombine_f32(vget_low_f32(a), vget_low_f32(b)); }
inline V4F v4f_cathi(V4F a, V4F b) noexcept { return vcombine_f32(vget_high_f32(a), vget_high_f32(b)); }
inline V4F v4f_ziplo(V4F a, V4F b) noexcept { return vzip1q_f32(a, b); }
inline V4F v4f_ziphi(V4F a, V4F b) noexcept { return vzip2q_f32(a, b); }
inline V4F v4f_swap_pairs(V4F a) noexcept { return vrev64q_f32(a); }
inline V4F v4f_swap_halves(V4F a) noexcept { return vextq_f32(a, a, 2); }
inline V4F v4f_uzpre(V4F a, V4F b) noexcept { return vuzp1q_f32(a, b); }
inline V4F v4f_uzpim(V4F a, V4F b) noexcept { return vuzp2q_f32(a, b); }
inline V4F v4f_rev(V4F a) noexcept { return vextq_f32(vrev64q_f32(a), vrev64q_f32(a), 2); }

#endif

#if STXLIB_FFT_LEVEL >= 2

using V4D = __m256d;
inline V4D v4d_ld(const double* p) noexcept { return _mm256_loadu_pd(p); }
inline void v4d_st(double* p, V4D a) noexcept { _mm256_storeu_pd(p, a); }
inline V4D v4d_add(V4D a, V4D b) noexcept { return _mm256_add_pd(a, b); }
inline V4D v4d_sub(V4D a, V4D b) noexcept { return _mm256_sub_pd(a, b); }
#if !defined(STXLIB_FFT_FUSED)
inline V4D v4d_mul(V4D a, V4D b) noexcept { return _mm256_mul_pd(a, b); }
#endif
inline V4D v4d_set1(double x) noexcept { return _mm256_set1_pd(x); }
inline V4D v4d_unplo(V4D a, V4D b) noexcept { return _mm256_unpacklo_pd(a, b); }
inline V4D v4d_unphi(V4D a, V4D b) noexcept { return _mm256_unpackhi_pd(a, b); }
inline V4D v4d_swap_pairs(V4D a) noexcept { return _mm256_permute_pd(a, 0x5); }
inline V4D v4d_swap_halves(V4D a) noexcept { return _mm256_permute4x64_pd(a, 0x4E); }
inline V4D v4d_cat128_lohi(V4D a, V4D b) noexcept { return _mm256_permute2f128_pd(a, b, 0x20); }
inline V4D v4d_cat128_hihi(V4D a, V4D b) noexcept { return _mm256_permute2f128_pd(a, b, 0x31); }
inline V4D v4d_ld2_dup(const double* p) noexcept {
    return _mm256_permute4x64_pd(_mm256_castpd128_pd256(_mm_loadu_pd(p)), 0x50);
}
inline V4D v4d_xor(V4D a, V4D b) noexcept { return _mm256_xor_pd(a, b); }
inline V4D v4d_neg_zero() noexcept { return _mm256_set1_pd(-0.0); }

#endif

#if defined(STXLIB_FFT_FUSED)

// The fused multiply-add of each vector type: fma(x, a, b) = x + a*b and
// fms(x, a, b) = x - a*b, rounded once.
#if STXLIB_FFT_LEVEL >= 1 && defined(STXLIB_FFT_NEON)
inline V2 v2_fma(V2 x, V2 a, V2 b) noexcept { return vfmaq_f64(x, a, b); }
inline V2 v2_fms(V2 x, V2 a, V2 b) noexcept { return vfmsq_f64(x, a, b); }
inline V2 v2_negate(V2 a) noexcept { return vnegq_f64(a); }
inline V2 v2_product(V2 a, V2 b) noexcept { return vmulq_f64(a, b); }
inline V4F v4f_fma(V4F x, V4F a, V4F b) noexcept { return vfmaq_f32(x, a, b); }
inline V4F v4f_fms(V4F x, V4F a, V4F b) noexcept { return vfmsq_f32(x, a, b); }
inline V4F v4f_negate(V4F a) noexcept { return vnegq_f32(a); }
inline V4F v4f_product(V4F a, V4F b) noexcept { return vmulq_f32(a, b); }
#elif STXLIB_FFT_LEVEL >= 2
inline V2 v2_fma(V2 x, V2 a, V2 b) noexcept { return _mm_fmadd_pd(a, b, x); }
inline V2 v2_fms(V2 x, V2 a, V2 b) noexcept { return _mm_fnmadd_pd(a, b, x); }
inline V2 v2_negate(V2 a) noexcept { return _mm_xor_pd(a, _mm_set1_pd(-0.0)); }
inline V2 v2_product(V2 a, V2 b) noexcept { return _mm_mul_pd(a, b); }
inline V4F v4f_fma(V4F x, V4F a, V4F b) noexcept { return _mm_fmadd_ps(a, b, x); }
inline V4F v4f_fms(V4F x, V4F a, V4F b) noexcept { return _mm_fnmadd_ps(a, b, x); }
inline V4F v4f_negate(V4F a) noexcept { return _mm_xor_ps(a, _mm_set1_ps(-0.0F)); }
inline V4F v4f_product(V4F a, V4F b) noexcept { return _mm_mul_ps(a, b); }
#else
#error "STXLIB_FFT_FUSED needs the NEON vector128 tier or the avx2_fma tier"
#endif

struct V2Product final {
    V2 a;
    V2 b;
    // A product used as a value is rounded.
    operator V2() const noexcept { return v2_product(a, b); }
};
inline V2Product v2_mul(V2 a, V2 b) noexcept { return V2Product{a, b}; }
inline V2 v2_add(V2 x, V2Product p) noexcept { return v2_fma(x, p.a, p.b); }
inline V2 v2_add(V2Product p, V2 x) noexcept { return v2_fma(x, p.a, p.b); }
// BFFT's MADD(MUL(a, b), c, d): the first product rounded, the second fused.
inline V2 v2_add(V2Product p, V2Product q) noexcept { return v2_fma(v2_product(p.a, p.b), q.a, q.b); }
inline V2 v2_sub(V2 x, V2Product p) noexcept { return v2_fms(x, p.a, p.b); }
inline V2 v2_sub(V2Product p, V2 x) noexcept { return v2_negate(v2_fms(x, p.a, p.b)); }
inline V2 v2_sub(V2Product p, V2Product q) noexcept { return v2_fms(v2_product(p.a, p.b), q.a, q.b); }

struct V4FProduct final {
    V4F a;
    V4F b;
    operator V4F() const noexcept { return v4f_product(a, b); }
};
inline V4FProduct v4f_mul(V4F a, V4F b) noexcept { return V4FProduct{a, b}; }
inline V4F v4f_add(V4F x, V4FProduct p) noexcept { return v4f_fma(x, p.a, p.b); }
inline V4F v4f_add(V4FProduct p, V4F x) noexcept { return v4f_fma(x, p.a, p.b); }
inline V4F v4f_add(V4FProduct p, V4FProduct q) noexcept { return v4f_fma(v4f_product(p.a, p.b), q.a, q.b); }
inline V4F v4f_sub(V4F x, V4FProduct p) noexcept { return v4f_fms(x, p.a, p.b); }
inline V4F v4f_sub(V4FProduct p, V4F x) noexcept { return v4f_negate(v4f_fms(x, p.a, p.b)); }
inline V4F v4f_sub(V4FProduct p, V4FProduct q) noexcept { return v4f_fms(v4f_product(p.a, p.b), q.a, q.b); }

#if STXLIB_FFT_LEVEL >= 2
struct V4DProduct final {
    V4D a;
    V4D b;
    operator V4D() const noexcept { return _mm256_mul_pd(a, b); }
};
inline V4DProduct v4d_mul(V4D a, V4D b) noexcept { return V4DProduct{a, b}; }
inline V4D v4d_add(V4D x, V4DProduct p) noexcept { return _mm256_fmadd_pd(p.a, p.b, x); }
inline V4D v4d_add(V4DProduct p, V4D x) noexcept { return _mm256_fmadd_pd(p.a, p.b, x); }
inline V4D v4d_add(V4DProduct p, V4DProduct q) noexcept { return _mm256_fmadd_pd(q.a, q.b, _mm256_mul_pd(p.a, p.b)); }
inline V4D v4d_sub(V4D x, V4DProduct p) noexcept { return _mm256_fnmadd_pd(p.a, p.b, x); }
inline V4D v4d_sub(V4DProduct p, V4D x) noexcept { return _mm256_fmsub_pd(p.a, p.b, x); }
inline V4D v4d_sub(V4DProduct p, V4DProduct q) noexcept { return _mm256_fnmadd_pd(q.a, q.b, _mm256_mul_pd(p.a, p.b)); }
#endif

#endif
