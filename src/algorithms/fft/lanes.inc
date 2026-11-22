// stx_fft's vector vocabulary for one SIMD tier: BFFT's macros of
// src/detail/bruun_simd_backend.hpp and bodft_kernel.hpp at edf5f01 (V2_*,
// V4F_*, V4D_*), as inline functions with the same meaning, lower-cased
// (V2_ADD is v2_add). Included only by variant.inc, inside the tier's
// namespace and its target attribute; the intrinsic headers are included
// there first.
//
//   V2   two doubles  (SSE2 __m128d, NEON float64x2_t)   STXLIB_FFT_LEVEL >= 1
//   V4F  four floats  (SSE __m128,   NEON float32x4_t)   STXLIB_FFT_LEVEL >= 1
//   V4D  four doubles (AVX __m256d)                      STXLIB_FFT_LEVEL == 2
//
// BFFT's fused V2_MADD, V2_MSUB (and V4F_, V4D_) are not here: the kernels
// spell the multiply and the add or subtract (A1 rule 1), so every tier
// rounds as the scalar code does. Its compare and select macros are unused
// by the three kernels and not copied.

#if STXLIB_FFT_LEVEL >= 1 && defined(STXLIB_FFT_X86)

using V2 = __m128d;
inline V2 v2_ld(const double* p) noexcept { return _mm_loadu_pd(p); }
inline void v2_st(double* p, V2 a) noexcept { _mm_storeu_pd(p, a); }
inline V2 v2_add(V2 a, V2 b) noexcept { return _mm_add_pd(a, b); }
inline V2 v2_sub(V2 a, V2 b) noexcept { return _mm_sub_pd(a, b); }
inline V2 v2_mul(V2 a, V2 b) noexcept { return _mm_mul_pd(a, b); }
inline V2 v2_set1(double x) noexcept { return _mm_set1_pd(x); }
inline V2 v2_setlh(double l, double h) noexcept { return _mm_set_pd(h, l); }
inline V2 v2_unplo(V2 a, V2 b) noexcept { return _mm_unpacklo_pd(a, b); }
inline V2 v2_unphi(V2 a, V2 b) noexcept { return _mm_unpackhi_pd(a, b); }
inline V2 v2_dup0(V2 a) noexcept { return _mm_unpacklo_pd(a, a); }
inline V2 v2_dup1(V2 a) noexcept { return _mm_unpackhi_pd(a, a); }
inline V2 v2_neghi(V2 a) noexcept { return _mm_xor_pd(a, _mm_set_pd(-0.0, 0.0)); }
inline V2 v2_swap(V2 a) noexcept { return _mm_shuffle_pd(a, a, 1); }

using V4F = __m128;
inline V4F v4f_ld(const float* p) noexcept { return _mm_loadu_ps(p); }
inline void v4f_st(float* p, V4F a) noexcept { _mm_storeu_ps(p, a); }
inline V4F v4f_add(V4F a, V4F b) noexcept { return _mm_add_ps(a, b); }
inline V4F v4f_sub(V4F a, V4F b) noexcept { return _mm_sub_ps(a, b); }
inline V4F v4f_mul(V4F a, V4F b) noexcept { return _mm_mul_ps(a, b); }
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
inline V2 v2_mul(V2 a, V2 b) noexcept { return vmulq_f64(a, b); }
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

using V4F = float32x4_t;
inline V4F v4f_ld(const float* p) noexcept { return vld1q_f32(p); }
inline void v4f_st(float* p, V4F a) noexcept { vst1q_f32(p, a); }
inline V4F v4f_add(V4F a, V4F b) noexcept { return vaddq_f32(a, b); }
inline V4F v4f_sub(V4F a, V4F b) noexcept { return vsubq_f32(a, b); }
inline V4F v4f_mul(V4F a, V4F b) noexcept { return vmulq_f32(a, b); }
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
inline V4D v4d_mul(V4D a, V4D b) noexcept { return _mm256_mul_pd(a, b); }
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

#endif
