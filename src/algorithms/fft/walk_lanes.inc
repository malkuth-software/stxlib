// The PSFT's vector policies for one tier (plan_state.hpp, WalkTier),
// chosen by STXLIB_FFT_WALK_TIER (0 exact_portable, 1 exact128, 2 fused128)
// and the architecture. Included only by walk_variant.inc, inside the tier's
// namespace and its target attribute; the intrinsic headers come first.
//
// Each policy is one vector width of one precision:
//   ld, st          load and store L reals (unaligned)
//   add, sub, mul   lane by lane
//   fma(a, b, c)    a + b c, and fms(a, b, c) a - b c: in the fused build one
//                   fused operation (NEON's FMLA and FMLS, x86's VFMADD and
//                   VFNMADD, which compute exactly these two forms), in the
//                   exact build a multiply then an add or subtract. These are
//                   the only fused forms anywhere (A1 rule 1, the PSFT's
//                   exception).
//   set1            every lane one value
//   neg             the sign flipped (exact)
//   zip1, zip2, swap and the other lane moves: permutations, exact
//   sqrt            IEEE square root, correctly rounded
//   flush           STXlib's subnormal rule on every lane (A1 rule 4): a
//                   lane below the smallest normal in magnitude keeps only its
//                   sign; every other lane, zero and NaN included, passes
//                   unchanged. A compare and bit operations, no arithmetic,
//                   so the bits are the same on every policy. The PSFT's
//                   output stores go through it (walk_put below).
// WalkPolicies<T> names, per precision: W, the widest vector the cell loops
// use (128 bits on every tier: the owner's ruling, 2026-11-23); Nr, a
// narrower one for passes whose runs are shorter than W; H, two lanes for
// the terminal pass; S, the straight-line kernels' policy (walk_small_*.inc;
// for float64 a two-lane policy with set2, for float32 a four-lane one with
// the transposes).
//
// The exact build gives the same bits on every policy below: each lane does
// the same IEEE operations in the same order.

#if !defined(STXLIB_FFT_WALK_TIER)
#error "walk_lanes.inc needs STXLIB_FFT_WALK_TIER"
#endif

// ---------------------------------------------------------------------------
// Plain lanes (exact_portable): arrays of L values, one operation per lane.
// ---------------------------------------------------------------------------

template <typename T, u32 Count>
struct PlainLanes final {
    T lane[Count];
};

template <typename T, u32 Count>
struct PlainPolicy {
    using v = PlainLanes<T, Count>;
    using t = T;
    static constexpr u32 L = Count;
    [[nodiscard]] static v ld(const T* const p) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = p[k];
        return r;
    }
    static void st(T* const p, const v a) noexcept {
        for (u32 k = 0; k < Count; ++k) p[k] = a.lane[k];
    }
    [[nodiscard]] static v flush(const v a) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = stxlib::detail::flush_subnormal(a.lane[k]);
        return r;
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = a.lane[k] + b.lane[k];
        return r;
    }
    [[nodiscard]] static v sub(const v a, const v b) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = a.lane[k] - b.lane[k];
        return r;
    }
    [[nodiscard]] static v mul(const v a, const v b) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = a.lane[k] * b.lane[k];
        return r;
    }
    // The exact build: the product is rounded, then added.
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept { return add(a, mul(b, c)); }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept { return sub(a, mul(b, c)); }
    [[nodiscard]] static v set1(const T x) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = x;
        return r;
    }
    [[nodiscard]] static v neg(const v a) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = -a.lane[k];
        return r;
    }
    [[nodiscard]] static v sqrt(const v a) noexcept {
        v r{};
        for (u32 k = 0; k < Count; ++k) r.lane[k] = std::sqrt(a.lane[k]);
        return r;
    }
    // (a0, b0, a1, b1, ...) and (a_{L/2}, b_{L/2}, ...).
    [[nodiscard]] static v zip1(const v a, const v b) noexcept {
        v r{};
        for (u32 k = 0; k < Count / 2; ++k) {
            r.lane[2 * k] = a.lane[k];
            r.lane[2 * k + 1] = b.lane[k];
        }
        return r;
    }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept {
        v r{};
        for (u32 k = 0; k < Count / 2; ++k) {
            r.lane[2 * k] = a.lane[Count / 2 + k];
            r.lane[2 * k + 1] = b.lane[Count / 2 + k];
        }
        return r;
    }
    static void stl(T* const p, const v a, const int lane) noexcept { p[0] = a.lane[lane]; }
};

// Two lanes: the terminal's and float64's straight-line policy.
template <typename T>
struct PlainPair final : PlainPolicy<T, 2> {
    using v = PlainLanes<T, 2>;
    using t = T;
    [[nodiscard]] static v swap(const v a) noexcept { return v{{a.lane[1], a.lane[0]}}; }
    [[nodiscard]] static v ld2s(const T* const a, const T* const b) noexcept { return v{{*a, *b}}; }
    [[nodiscard]] static v set2(const T a, const T b) noexcept { return v{{a, b}}; }
};

// Four float lanes: float32's straight-line policy.
struct PlainQuad final : PlainPolicy<float, 4> {
    using v = PlainLanes<float, 4>;
    using t = float;
    [[nodiscard]] static v trn1(const v a, const v b) noexcept { return v{{a.lane[0], b.lane[0], a.lane[2], b.lane[2]}}; }
    [[nodiscard]] static v trn2(const v a, const v b) noexcept { return v{{a.lane[1], b.lane[1], a.lane[3], b.lane[3]}}; }
    [[nodiscard]] static v uzp1(const v a, const v b) noexcept { return v{{a.lane[0], a.lane[2], b.lane[0], b.lane[2]}}; }
    [[nodiscard]] static v uzp2(const v a, const v b) noexcept { return v{{a.lane[1], a.lane[3], b.lane[1], b.lane[3]}}; }
    [[nodiscard]] static v set4(const float a, const float b, const float c, const float d) noexcept {
        return v{{a, b, c, d}};
    }
    [[nodiscard]] static v cat_lo(const v a, const v b) noexcept { return v{{a.lane[0], a.lane[1], b.lane[0], b.lane[1]}}; }
    [[nodiscard]] static v cat_hi(const v a, const v b) noexcept { return v{{a.lane[2], a.lane[3], b.lane[2], b.lane[3]}}; }
    [[nodiscard]] static v ld2(const float* const p, const float* const q) noexcept { return v{{p[0], p[1], q[0], q[1]}}; }
    static void st_lo(float* const p, const v a) noexcept {
        p[0] = a.lane[0];
        p[1] = a.lane[1];
    }
    static void st_hi(float* const p, const v a) noexcept {
        p[0] = a.lane[2];
        p[1] = a.lane[3];
    }
};

#if STXLIB_FFT_WALK_TIER == 0

template <typename T>
struct WalkPolicies;
template <>
struct WalkPolicies<double> final {
    using W = PlainPair<double>;
    using Nr = PlainPair<double>;
    using H = PlainPair<double>;
    using S = PlainPair<double>;
};
template <>
struct WalkPolicies<float> final {
    using W = PlainQuad;
    using Nr = PlainPair<float>;
    using H = PlainPair<float>;
    using S = PlainQuad;
};

#elif defined(STXLIB_FFT_WALK_NEON)

// ---------------------------------------------------------------------------
// NEON: exact128 (multiply, then add) or fused128 (FMLA, FMLS).
// ---------------------------------------------------------------------------

#if STXLIB_FFT_WALK_TIER == 2
#define STXLIB_FFT_WALK_FUSE(fused, plain) (fused)
#else
#define STXLIB_FFT_WALK_FUSE(fused, plain) (plain)
#endif

struct NeonD2 {
    using v = float64x2_t;
    using t = double;
    static constexpr u32 L = 2;
    [[nodiscard]] static v ld(const double* const p) noexcept { return vld1q_f64(p); }
    static void st(double* const p, const v a) noexcept { vst1q_f64(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const uint64x2_t small = vcagtq_f64(vdupq_n_f64(0x1p-1022), a);
        const uint64x2_t magnitude = vdupq_n_u64(0x7fffffffffffffffULL);
        return vreinterpretq_f64_u64(vbicq_u64(vreinterpretq_u64_f64(a), vandq_u64(small, magnitude)));
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return vaddq_f64(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return vsubq_f64(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return vmulq_f64(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfmaq_f64(a, b, c), vaddq_f64(a, vmulq_f64(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfmsq_f64(a, b, c), vsubq_f64(a, vmulq_f64(b, c)));
    }
    [[nodiscard]] static v set1(const double x) noexcept { return vdupq_n_f64(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return vnegq_f64(a); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return vzip1q_f64(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return vzip2q_f64(a, b); }
    [[nodiscard]] static v swap(const v a) noexcept { return vextq_f64(a, a, 1); }
    static void stl(double* const p, const v a, const int lane) noexcept {
        if (lane == 0) {
            vst1q_lane_f64(p, a, 0);
        } else {
            vst1q_lane_f64(p, a, 1);
        }
    }
    [[nodiscard]] static v sqrt(const v a) noexcept { return vsqrtq_f64(a); }
    [[nodiscard]] static v ld2s(const double* const a, const double* const b) noexcept {
        return vcombine_f64(vld1_f64(a), vld1_f64(b));
    }
    [[nodiscard]] static v set2(const double a, const double b) noexcept {
        const double pair[2] = {a, b};
        return vld1q_f64(pair);
    }
};

struct NeonF4 {
    using v = float32x4_t;
    using t = float;
    static constexpr u32 L = 4;
    [[nodiscard]] static v ld(const float* const p) noexcept { return vld1q_f32(p); }
    static void st(float* const p, const v a) noexcept { vst1q_f32(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const uint32x4_t small = vcagtq_f32(vdupq_n_f32(0x1p-126F), a);
        const uint32x4_t magnitude = vdupq_n_u32(0x7fffffffU);
        return vreinterpretq_f32_u32(vbicq_u32(vreinterpretq_u32_f32(a), vandq_u32(small, magnitude)));
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return vaddq_f32(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return vsubq_f32(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return vmulq_f32(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfmaq_f32(a, b, c), vaddq_f32(a, vmulq_f32(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfmsq_f32(a, b, c), vsubq_f32(a, vmulq_f32(b, c)));
    }
    [[nodiscard]] static v set1(const float x) noexcept { return vdupq_n_f32(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return vnegq_f32(a); }
    [[nodiscard]] static v sqrt(const v a) noexcept { return vsqrtq_f32(a); }
    [[nodiscard]] static v trn1(const v a, const v b) noexcept { return vtrn1q_f32(a, b); }
    [[nodiscard]] static v trn2(const v a, const v b) noexcept { return vtrn2q_f32(a, b); }
    [[nodiscard]] static v uzp1(const v a, const v b) noexcept { return vuzp1q_f32(a, b); }
    [[nodiscard]] static v uzp2(const v a, const v b) noexcept { return vuzp2q_f32(a, b); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return vzip1q_f32(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return vzip2q_f32(a, b); }
    [[nodiscard]] static v set4(const float a, const float b, const float c, const float d) noexcept {
        const float four[4] = {a, b, c, d};
        return vld1q_f32(four);
    }
    [[nodiscard]] static v cat_lo(const v a, const v b) noexcept { return vcombine_f32(vget_low_f32(a), vget_low_f32(b)); }
    [[nodiscard]] static v cat_hi(const v a, const v b) noexcept {
        return vcombine_f32(vget_high_f32(a), vget_high_f32(b));
    }
    [[nodiscard]] static v ld2(const float* const p, const float* const q) noexcept {
        return vcombine_f32(vld1_f32(p), vld1_f32(q));
    }
    static void st_lo(float* const p, const v a) noexcept { vst1_f32(p, vget_low_f32(a)); }
    static void st_hi(float* const p, const v a) noexcept { vst1_f32(p, vget_high_f32(a)); }
    static void stl(float* const p, const v a, const int lane) noexcept {
        switch (lane) {
        case 0:
            vst1q_lane_f32(p, a, 0);
            return;
        case 1:
            vst1q_lane_f32(p, a, 1);
            return;
        case 2:
            vst1q_lane_f32(p, a, 2);
            return;
        default:
            vst1q_lane_f32(p, a, 3);
            return;
        }
    }
};

struct NeonF2 {
    using v = float32x2_t;
    using t = float;
    static constexpr u32 L = 2;
    [[nodiscard]] static v ld(const float* const p) noexcept { return vld1_f32(p); }
    static void st(float* const p, const v a) noexcept { vst1_f32(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const uint32x2_t small = vcagt_f32(vdup_n_f32(0x1p-126F), a);
        const uint32x2_t magnitude = vdup_n_u32(0x7fffffffU);
        return vreinterpret_f32_u32(vbic_u32(vreinterpret_u32_f32(a), vand_u32(small, magnitude)));
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return vadd_f32(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return vsub_f32(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return vmul_f32(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfma_f32(a, b, c), vadd_f32(a, vmul_f32(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(vfms_f32(a, b, c), vsub_f32(a, vmul_f32(b, c)));
    }
    [[nodiscard]] static v set1(const float x) noexcept { return vdup_n_f32(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return vneg_f32(a); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return vzip1_f32(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return vzip2_f32(a, b); }
    [[nodiscard]] static v swap(const v a) noexcept { return vrev64_f32(a); }
    static void stl(float* const p, const v a, const int lane) noexcept {
        if (lane == 0) {
            vst1_lane_f32(p, a, 0);
        } else {
            vst1_lane_f32(p, a, 1);
        }
    }
    [[nodiscard]] static v sqrt(const v a) noexcept { return vsqrt_f32(a); }
};

#undef STXLIB_FFT_WALK_FUSE

template <typename T>
struct WalkPolicies;
template <>
struct WalkPolicies<double> final {
    using W = NeonD2;
    using Nr = NeonD2;
    using H = NeonD2;
    using S = NeonD2;
};
template <>
struct WalkPolicies<float> final {
    using W = NeonF4;
    using Nr = NeonF4;
    using H = NeonF2;
    using S = NeonF4;
};

#elif defined(STXLIB_FFT_WALK_SSE)

// ---------------------------------------------------------------------------
// x86-64 at 128 bits: exact128 (SSE2 only) or fused128 (SSE registers with
// FMA3, compiled under the "fma" target attribute). The lane moves are SSE2
// in both, so the two differ only in fma and fms.
// ---------------------------------------------------------------------------

#if STXLIB_FFT_WALK_TIER == 2
#define STXLIB_FFT_WALK_FUSE(fused, plain) (fused)
#else
#define STXLIB_FFT_WALK_FUSE(fused, plain) (plain)
#endif

struct SseD2 {
    using v = __m128d;
    using t = double;
    static constexpr u32 L = 2;
    [[nodiscard]] static v ld(const double* const p) noexcept { return _mm_loadu_pd(p); }
    static void st(double* const p, const v a) noexcept { _mm_storeu_pd(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const v magnitude = _mm_castsi128_pd(_mm_set1_epi64x(0x7fffffffffffffffLL));
        const v small = _mm_cmplt_pd(_mm_and_pd(a, magnitude), _mm_set1_pd(0x1p-1022));
        return _mm_andnot_pd(_mm_and_pd(small, magnitude), a);
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return _mm_add_pd(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return _mm_sub_pd(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return _mm_mul_pd(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fmadd_pd(b, c, a), _mm_add_pd(a, _mm_mul_pd(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fnmadd_pd(b, c, a), _mm_sub_pd(a, _mm_mul_pd(b, c)));
    }
    [[nodiscard]] static v set1(const double x) noexcept { return _mm_set1_pd(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return _mm_xor_pd(a, _mm_set1_pd(-0.0)); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return _mm_unpacklo_pd(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return _mm_unpackhi_pd(a, b); }
    [[nodiscard]] static v swap(const v a) noexcept { return _mm_shuffle_pd(a, a, 1); }
    static void stl(double* const p, const v a, const int lane) noexcept {
        if (lane == 0) {
            _mm_storel_pd(p, a);
        } else {
            _mm_storeh_pd(p, a);
        }
    }
    [[nodiscard]] static v sqrt(const v a) noexcept { return _mm_sqrt_pd(a); }
    [[nodiscard]] static v ld2s(const double* const a, const double* const b) noexcept { return _mm_set_pd(*b, *a); }
    [[nodiscard]] static v set2(const double a, const double b) noexcept { return _mm_set_pd(b, a); }
};

struct SseF4 {
    using v = __m128;
    using t = float;
    static constexpr u32 L = 4;
    [[nodiscard]] static v ld(const float* const p) noexcept { return _mm_loadu_ps(p); }
    static void st(float* const p, const v a) noexcept { _mm_storeu_ps(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const v magnitude = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const v small = _mm_cmplt_ps(_mm_and_ps(a, magnitude), _mm_set1_ps(0x1p-126F));
        return _mm_andnot_ps(_mm_and_ps(small, magnitude), a);
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return _mm_add_ps(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return _mm_sub_ps(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return _mm_mul_ps(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fmadd_ps(b, c, a), _mm_add_ps(a, _mm_mul_ps(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fnmadd_ps(b, c, a), _mm_sub_ps(a, _mm_mul_ps(b, c)));
    }
    [[nodiscard]] static v set1(const float x) noexcept { return _mm_set1_ps(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return _mm_xor_ps(a, _mm_set1_ps(-0.0F)); }
    [[nodiscard]] static v sqrt(const v a) noexcept { return _mm_sqrt_ps(a); }
    // (a0, b0, a2, b2) and (a1, b1, a3, b3), in two SSE shuffles each.
    [[nodiscard]] static v trn1(const v a, const v b) noexcept {
        const v evens = _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0));
        return _mm_shuffle_ps(evens, evens, _MM_SHUFFLE(3, 1, 2, 0));
    }
    [[nodiscard]] static v trn2(const v a, const v b) noexcept {
        const v odds = _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1));
        return _mm_shuffle_ps(odds, odds, _MM_SHUFFLE(3, 1, 2, 0));
    }
    [[nodiscard]] static v uzp1(const v a, const v b) noexcept { return _mm_shuffle_ps(a, b, _MM_SHUFFLE(2, 0, 2, 0)); }
    [[nodiscard]] static v uzp2(const v a, const v b) noexcept { return _mm_shuffle_ps(a, b, _MM_SHUFFLE(3, 1, 3, 1)); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return _mm_unpacklo_ps(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return _mm_unpackhi_ps(a, b); }
    [[nodiscard]] static v set4(const float a, const float b, const float c, const float d) noexcept {
        return _mm_set_ps(d, c, b, a);
    }
    [[nodiscard]] static v cat_lo(const v a, const v b) noexcept { return _mm_movelh_ps(a, b); }
    [[nodiscard]] static v cat_hi(const v a, const v b) noexcept { return _mm_movehl_ps(b, a); }
    [[nodiscard]] static v ld2(const float* const p, const float* const q) noexcept {
        const v low = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(p)));
        const v high = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(q)));
        return _mm_movelh_ps(low, high);
    }
    static void st_lo(float* const p, const v a) noexcept { _mm_storel_pi(reinterpret_cast<__m64*>(p), a); }
    static void st_hi(float* const p, const v a) noexcept { _mm_storeh_pi(reinterpret_cast<__m64*>(p), a); }
    static void stl(float* const p, const v a, const int lane) noexcept {
        switch (lane) {
        case 0:
            _mm_store_ss(p, a);
            return;
        case 1:
            _mm_store_ss(p, _mm_shuffle_ps(a, a, 1));
            return;
        case 2:
            _mm_store_ss(p, _mm_movehl_ps(a, a));
            return;
        default:
            _mm_store_ss(p, _mm_shuffle_ps(a, a, 3));
            return;
        }
    }
};

// Two floats in the low half of an __m128; the high half is zero on load and
// never stored.
struct SseF2 {
    using v = __m128;
    using t = float;
    static constexpr u32 L = 2;
    [[nodiscard]] static v ld(const float* const p) noexcept {
        return _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(p)));
    }
    static void st(float* const p, const v a) noexcept { _mm_storel_pi(reinterpret_cast<__m64*>(p), a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const v magnitude = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const v small = _mm_cmplt_ps(_mm_and_ps(a, magnitude), _mm_set1_ps(0x1p-126F));
        return _mm_andnot_ps(_mm_and_ps(small, magnitude), a);
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return _mm_add_ps(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return _mm_sub_ps(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return _mm_mul_ps(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fmadd_ps(b, c, a), _mm_add_ps(a, _mm_mul_ps(b, c)));
    }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept {
        return STXLIB_FFT_WALK_FUSE(_mm_fnmadd_ps(b, c, a), _mm_sub_ps(a, _mm_mul_ps(b, c)));
    }
    [[nodiscard]] static v set1(const float x) noexcept { return _mm_set1_ps(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return _mm_xor_ps(a, _mm_set1_ps(-0.0F)); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return _mm_unpacklo_ps(a, b); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept {
        const v both = _mm_unpacklo_ps(a, b);
        return _mm_movehl_ps(both, both);
    }
    [[nodiscard]] static v swap(const v a) noexcept { return _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 2, 0, 1)); }
    static void stl(float* const p, const v a, const int lane) noexcept {
        if (lane == 0) {
            _mm_store_ss(p, a);
        } else {
            _mm_store_ss(p, _mm_shuffle_ps(a, a, 1));
        }
    }
    [[nodiscard]] static v sqrt(const v a) noexcept { return _mm_sqrt_ps(a); }
};

#undef STXLIB_FFT_WALK_FUSE

template <typename T>
struct WalkPolicies;
template <>
struct WalkPolicies<double> final {
    using W = SseD2;
    using Nr = SseD2;
    using H = SseD2;
    using S = SseD2;
};
template <>
struct WalkPolicies<float> final {
    using W = SseF4;
    using Nr = SseF4;
    using H = SseF2;
    using S = SseF4;
};

#elif defined(STXLIB_FFT_WALK_WASM)

// ---------------------------------------------------------------------------
// wasm SIMD128: exact128 only (wasm has no fused operation).
// ---------------------------------------------------------------------------

struct WasmD2 {
    using v = v128_t;
    using t = double;
    static constexpr u32 L = 2;
    [[nodiscard]] static v ld(const double* const p) noexcept { return wasm_v128_load(p); }
    static void st(double* const p, const v a) noexcept { wasm_v128_store(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const v small = wasm_f64x2_lt(wasm_f64x2_abs(a), wasm_f64x2_splat(0x1p-1022));
        return wasm_v128_andnot(a, wasm_v128_and(small, wasm_i64x2_splat(0x7fffffffffffffffLL)));
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return wasm_f64x2_add(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return wasm_f64x2_sub(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return wasm_f64x2_mul(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept { return add(a, mul(b, c)); }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept { return sub(a, mul(b, c)); }
    [[nodiscard]] static v set1(const double x) noexcept { return wasm_f64x2_splat(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return wasm_f64x2_neg(a); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return wasm_i64x2_shuffle(a, b, 0, 2); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return wasm_i64x2_shuffle(a, b, 1, 3); }
    [[nodiscard]] static v swap(const v a) noexcept { return wasm_i64x2_shuffle(a, a, 1, 0); }
    static void stl(double* const p, const v a, const int lane) noexcept {
        if (lane == 0) {
            *p = wasm_f64x2_extract_lane(a, 0);
        } else {
            *p = wasm_f64x2_extract_lane(a, 1);
        }
    }
    [[nodiscard]] static v sqrt(const v a) noexcept { return wasm_f64x2_sqrt(a); }
    [[nodiscard]] static v ld2s(const double* const a, const double* const b) noexcept { return wasm_f64x2_make(*a, *b); }
    [[nodiscard]] static v set2(const double a, const double b) noexcept { return wasm_f64x2_make(a, b); }
};

struct WasmF4 {
    using v = v128_t;
    using t = float;
    static constexpr u32 L = 4;
    [[nodiscard]] static v ld(const float* const p) noexcept { return wasm_v128_load(p); }
    static void st(float* const p, const v a) noexcept { wasm_v128_store(p, a); }
    [[nodiscard]] static v flush(const v a) noexcept {
        const v small = wasm_f32x4_lt(wasm_f32x4_abs(a), wasm_f32x4_splat(0x1p-126F));
        return wasm_v128_andnot(a, wasm_v128_and(small, wasm_i32x4_splat(0x7fffffff)));
    }
    [[nodiscard]] static v add(const v a, const v b) noexcept { return wasm_f32x4_add(a, b); }
    [[nodiscard]] static v sub(const v a, const v b) noexcept { return wasm_f32x4_sub(a, b); }
    [[nodiscard]] static v mul(const v a, const v b) noexcept { return wasm_f32x4_mul(a, b); }
    [[nodiscard]] static v fma(const v a, const v b, const v c) noexcept { return add(a, mul(b, c)); }
    [[nodiscard]] static v fms(const v a, const v b, const v c) noexcept { return sub(a, mul(b, c)); }
    [[nodiscard]] static v set1(const float x) noexcept { return wasm_f32x4_splat(x); }
    [[nodiscard]] static v neg(const v a) noexcept { return wasm_f32x4_neg(a); }
    [[nodiscard]] static v sqrt(const v a) noexcept { return wasm_f32x4_sqrt(a); }
    [[nodiscard]] static v trn1(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 0, 4, 2, 6); }
    [[nodiscard]] static v trn2(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 1, 5, 3, 7); }
    [[nodiscard]] static v uzp1(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 0, 2, 4, 6); }
    [[nodiscard]] static v uzp2(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 1, 3, 5, 7); }
    [[nodiscard]] static v zip1(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 0, 4, 1, 5); }
    [[nodiscard]] static v zip2(const v a, const v b) noexcept { return wasm_i32x4_shuffle(a, b, 2, 6, 3, 7); }
    [[nodiscard]] static v set4(const float a, const float b, const float c, const float d) noexcept {
        return wasm_f32x4_make(a, b, c, d);
    }
    [[nodiscard]] static v cat_lo(const v a, const v b) noexcept { return wasm_i64x2_shuffle(a, b, 0, 2); }
    [[nodiscard]] static v cat_hi(const v a, const v b) noexcept { return wasm_i64x2_shuffle(a, b, 1, 3); }
    [[nodiscard]] static v ld2(const float* const p, const float* const q) noexcept {
        return wasm_f32x4_make(p[0], p[1], q[0], q[1]);
    }
    static void st_lo(float* const p, const v a) noexcept {
        p[0] = wasm_f32x4_extract_lane(a, 0);
        p[1] = wasm_f32x4_extract_lane(a, 1);
    }
    static void st_hi(float* const p, const v a) noexcept {
        p[0] = wasm_f32x4_extract_lane(a, 2);
        p[1] = wasm_f32x4_extract_lane(a, 3);
    }
    static void stl(float* const p, const v a, const int lane) noexcept {
        switch (lane) {
        case 0:
            *p = wasm_f32x4_extract_lane(a, 0);
            return;
        case 1:
            *p = wasm_f32x4_extract_lane(a, 1);
            return;
        case 2:
            *p = wasm_f32x4_extract_lane(a, 2);
            return;
        default:
            *p = wasm_f32x4_extract_lane(a, 3);
            return;
        }
    }
};

template <typename T>
struct WalkPolicies;
template <>
struct WalkPolicies<double> final {
    using W = WasmD2;
    using Nr = WasmD2;
    using H = WasmD2;
    using S = WasmD2;
};
// wasm's terminal runs on plain pairs: SIMD128 has no two-float vector, and
// the pair gives the same bits.
template <>
struct WalkPolicies<float> final {
    using W = WasmF4;
    using Nr = WasmF4;
    using H = PlainPair<float>;
    using S = WasmF4;
};

#else
#error "walk_lanes.inc: no vector policy for this tier and architecture"
#endif

// STXlib's subnormal rule on one real (walk_flush), for inputs and outputs
// read or written one real at a time.
template <typename T>
[[nodiscard]] inline T walk_flush(const T value) noexcept {
    return stxlib::detail::flush_subnormal(value);
}

// The PSFT's input loads: STXlib's rule on its arguments (a subnormal input
// is read as zero of its sign) folded into the load, so no transform scans
// or copies its input first. walk_load flushes only when In (the load reads
// the caller's input, not a work buffer).
template <typename P>
[[nodiscard]] inline typename P::v walk_get(const typename P::t* const p) noexcept {
    return P::flush(P::ld(p));
}

template <typename P>
[[nodiscard]] inline typename P::v walk_get2(const typename P::t* const p, const typename P::t* const q) noexcept {
    return P::flush(P::ld2(p, q));
}

template <typename P, bool In>
[[nodiscard]] inline typename P::v walk_load(const typename P::t* const p) noexcept {
    if constexpr (In) {
        return P::flush(P::ld(p));
    } else {
        return P::ld(p);
    }
}

// One real of the input (In: flushed) or of a work buffer (unchanged).
template <typename T, bool In>
[[nodiscard]] inline T walk_in(const T value) noexcept {
    if constexpr (In) {
        return stxlib::detail::flush_subnormal(value);
    } else {
        return value;
    }
}

// One real of the output (Out: flushed) or of a work buffer (unchanged).
template <typename T, bool Out>
[[nodiscard]] inline T walk_out(const T value) noexcept {
    if constexpr (Out) {
        return stxlib::detail::flush_subnormal(value);
    } else {
        return value;
    }
}

// The PSFT's output stores: the rule folded into the store (flush, then
// st), so no transform reads its output again to flush it.
template <typename P>
inline void walk_put(typename P::t* const p, const typename P::v a) noexcept {
    P::st(p, P::flush(a));
}

template <typename P>
inline void walk_putl(typename P::t* const p, const typename P::v a, const int lane) noexcept {
    P::stl(p, P::flush(a), lane);
}

template <typename P>
inline void walk_put_lo(typename P::t* const p, const typename P::v a) noexcept {
    P::st_lo(p, P::flush(a));
}

template <typename P>
inline void walk_put_hi(typename P::t* const p, const typename P::v a) noexcept {
    P::st_hi(p, P::flush(a));
}

// A store that flushes (Out: the transform's output) or not (a work buffer).
template <typename P, bool Out>
inline void walk_store(typename P::t* const p, const typename P::v a) noexcept {
    if constexpr (Out) {
        P::st(p, P::flush(a));
    } else {
        P::st(p, a);
    }
}
