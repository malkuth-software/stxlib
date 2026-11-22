// stxlib::conv's lane vocabulary for one SIMD tier: ScalarLanes (one double) always, and Lanes,
// the tier's group (scalar: one; vector128: NEON float64x2 or SSE2 __m128d; avx2_fma: __m256d).
// add, sub, mul, div are correctly rounded IEEE in every tier; min and max are compare-select
// with stx_conv's semantics (least(a, b) = b < a ? b : a), so the tiers agree bit for bit on
// finite operands, signed zeros included. Included only by variant.inc, inside the tier's
// namespace and target attribute.

struct ScalarLanes final {
    using Value = double;
    using Mask = bool;
    static constexpr std::size_t width = 1U;
    static STXLIB_CONV_INLINE Value load(const double* p) noexcept { return *p; }
    static STXLIB_CONV_INLINE void store(double* p, Value a) noexcept { *p = a; }
    static STXLIB_CONV_INLINE Value set(double x) noexcept { return x; }
    static STXLIB_CONV_INLINE Value add(Value a, Value b) noexcept { return a + b; }
    static STXLIB_CONV_INLINE Value sub(Value a, Value b) noexcept { return a - b; }
    static STXLIB_CONV_INLINE Value mul(Value a, Value b) noexcept { return a * b; }
    static STXLIB_CONV_INLINE Value div(Value a, Value b) noexcept { return a / b; }
    static STXLIB_CONV_INLINE Mask less(Value a, Value b) noexcept { return a < b; }
    static STXLIB_CONV_INLINE Mask equal(Value a, Value b) noexcept { return a == b; }
    static STXLIB_CONV_INLINE Mask both(Mask a, Mask b) noexcept { return a && b; }
    static STXLIB_CONV_INLINE Mask either(Mask a, Mask b) noexcept { return a || b; }
    static STXLIB_CONV_INLINE Mask invert(Mask a) noexcept { return !a; }
    static STXLIB_CONV_INLINE Value select(Mask m, Value a, Value b) noexcept { return m ? a : b; }
    // Lane k of a mask as 0 or 1.
    static STXLIB_CONV_INLINE std::size_t flag(Mask m, std::size_t) noexcept { return m ? 1U : 0U; }
};

#if STXLIB_CONV_LEVEL == 0
using Lanes = ScalarLanes;
#elif STXLIB_CONV_LEVEL == 1 && defined(STXLIB_CONV_NEON)
struct Lanes final {
    using Value = float64x2_t;
    using Mask = uint64x2_t;
    static constexpr std::size_t width = 2U;
    static STXLIB_CONV_INLINE Value load(const double* p) noexcept { return vld1q_f64(p); }
    static STXLIB_CONV_INLINE void store(double* p, Value a) noexcept { vst1q_f64(p, a); }
    static STXLIB_CONV_INLINE Value set(double x) noexcept { return vdupq_n_f64(x); }
    static STXLIB_CONV_INLINE Value add(Value a, Value b) noexcept { return vaddq_f64(a, b); }
    static STXLIB_CONV_INLINE Value sub(Value a, Value b) noexcept { return vsubq_f64(a, b); }
    static STXLIB_CONV_INLINE Value mul(Value a, Value b) noexcept { return vmulq_f64(a, b); }
    static STXLIB_CONV_INLINE Value div(Value a, Value b) noexcept { return vdivq_f64(a, b); }
    static STXLIB_CONV_INLINE Mask less(Value a, Value b) noexcept { return vcltq_f64(a, b); }
    static STXLIB_CONV_INLINE Mask equal(Value a, Value b) noexcept { return vceqq_f64(a, b); }
    static STXLIB_CONV_INLINE Mask both(Mask a, Mask b) noexcept { return vandq_u64(a, b); }
    static STXLIB_CONV_INLINE Mask either(Mask a, Mask b) noexcept { return vorrq_u64(a, b); }
    static STXLIB_CONV_INLINE Mask invert(Mask a) noexcept {
        return vreinterpretq_u64_u32(vmvnq_u32(vreinterpretq_u32_u64(a)));
    }
    static STXLIB_CONV_INLINE Value select(Mask m, Value a, Value b) noexcept { return vbslq_f64(m, a, b); }
    static STXLIB_CONV_INLINE std::size_t flag(Mask m, std::size_t k) noexcept {
        const std::uint64_t bits = k == 0U ? vgetq_lane_u64(m, 0) : vgetq_lane_u64(m, 1);
        return static_cast<std::size_t>(bits & 1U);
    }
};
#elif STXLIB_CONV_LEVEL == 1 && defined(STXLIB_CONV_X86)
struct Lanes final {
    using Value = __m128d;
    using Mask = __m128d;
    static constexpr std::size_t width = 2U;
    static STXLIB_CONV_INLINE Value load(const double* p) noexcept { return _mm_loadu_pd(p); }
    static STXLIB_CONV_INLINE void store(double* p, Value a) noexcept { _mm_storeu_pd(p, a); }
    static STXLIB_CONV_INLINE Value set(double x) noexcept { return _mm_set1_pd(x); }
    static STXLIB_CONV_INLINE Value add(Value a, Value b) noexcept { return _mm_add_pd(a, b); }
    static STXLIB_CONV_INLINE Value sub(Value a, Value b) noexcept { return _mm_sub_pd(a, b); }
    static STXLIB_CONV_INLINE Value mul(Value a, Value b) noexcept { return _mm_mul_pd(a, b); }
    static STXLIB_CONV_INLINE Value div(Value a, Value b) noexcept { return _mm_div_pd(a, b); }
    static STXLIB_CONV_INLINE Mask less(Value a, Value b) noexcept { return _mm_cmplt_pd(a, b); }
    static STXLIB_CONV_INLINE Mask equal(Value a, Value b) noexcept { return _mm_cmpeq_pd(a, b); }
    static STXLIB_CONV_INLINE Mask both(Mask a, Mask b) noexcept { return _mm_and_pd(a, b); }
    static STXLIB_CONV_INLINE Mask either(Mask a, Mask b) noexcept { return _mm_or_pd(a, b); }
    static STXLIB_CONV_INLINE Mask invert(Mask a) noexcept { return _mm_xor_pd(a, _mm_castsi128_pd(_mm_set1_epi32(-1))); }
    static STXLIB_CONV_INLINE Value select(Mask m, Value a, Value b) noexcept {
        return _mm_or_pd(_mm_and_pd(m, a), _mm_andnot_pd(m, b));
    }
    static STXLIB_CONV_INLINE std::size_t flag(Mask m, std::size_t k) noexcept {
        return static_cast<std::size_t>((_mm_movemask_pd(m) >> k) & 1);
    }
};
#elif STXLIB_CONV_LEVEL == 2
struct Lanes final {
    using Value = __m256d;
    using Mask = __m256d;
    static constexpr std::size_t width = 4U;
    static STXLIB_CONV_INLINE Value load(const double* p) noexcept { return _mm256_loadu_pd(p); }
    static STXLIB_CONV_INLINE void store(double* p, Value a) noexcept { _mm256_storeu_pd(p, a); }
    static STXLIB_CONV_INLINE Value set(double x) noexcept { return _mm256_set1_pd(x); }
    static STXLIB_CONV_INLINE Value add(Value a, Value b) noexcept { return _mm256_add_pd(a, b); }
    static STXLIB_CONV_INLINE Value sub(Value a, Value b) noexcept { return _mm256_sub_pd(a, b); }
    static STXLIB_CONV_INLINE Value mul(Value a, Value b) noexcept { return _mm256_mul_pd(a, b); }
    static STXLIB_CONV_INLINE Value div(Value a, Value b) noexcept { return _mm256_div_pd(a, b); }
    static STXLIB_CONV_INLINE Mask less(Value a, Value b) noexcept { return _mm256_cmp_pd(a, b, _CMP_LT_OQ); }
    static STXLIB_CONV_INLINE Mask equal(Value a, Value b) noexcept { return _mm256_cmp_pd(a, b, _CMP_EQ_OQ); }
    static STXLIB_CONV_INLINE Mask both(Mask a, Mask b) noexcept { return _mm256_and_pd(a, b); }
    static STXLIB_CONV_INLINE Mask either(Mask a, Mask b) noexcept { return _mm256_or_pd(a, b); }
    static STXLIB_CONV_INLINE Mask invert(Mask a) noexcept { return _mm256_xor_pd(a, _mm256_castsi256_pd(_mm256_set1_epi32(-1))); }
    static STXLIB_CONV_INLINE Value select(Mask m, Value a, Value b) noexcept { return _mm256_blendv_pd(b, a, m); }
    static STXLIB_CONV_INLINE std::size_t flag(Mask m, std::size_t k) noexcept {
        return static_cast<std::size_t>((_mm256_movemask_pd(m) >> k) & 1);
    }
};
#endif

// The derived primitives, written once over the vocabulary (any lane type L).
template<class L> STXLIB_CONV_INLINE typename L::Value least(typename L::Value a, typename L::Value b) noexcept {
    return L::select(L::less(b, a), b, a);
}
template<class L> STXLIB_CONV_INLINE typename L::Value greatest(typename L::Value a, typename L::Value b) noexcept {
    return L::select(L::less(a, b), b, a);
}
template<class L> STXLIB_CONV_INLINE typename L::Value positive_part(typename L::Value x) noexcept {
    const typename L::Value zero = L::set(0.0);
    return L::select(L::less(zero, x), x, zero);
}
template<class L> STXLIB_CONV_INLINE typename L::Value negative_part(typename L::Value x) noexcept {
    const typename L::Value zero = L::set(0.0);
    return L::select(L::less(x, zero), L::sub(zero, x), zero);
}

