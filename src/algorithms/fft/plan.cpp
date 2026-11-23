// stx_fft's plans (include/stxlib/fft.hpp): validation, the tables of the
// three kernels, and the calls into the SIMD tier the plan was made for.
//
// Tables. BFFT computes its twiddles with libm (BODFT, DIP) or a half-angle
// sqrt recurrence (DIF). Here every one comes from STXlib's half-turn
// functions (stxlib/math.hpp: sincospi, cospi; owner, 2026-11-22), whose
// arguments are exact here: each angle is a rational multiple of pi with a
// power-of-two denominator, so the argument in half-turns is an exact
// double and the value is the half-turn function's (within 0.5002 ulp).
// float tables are the double values rounded to float, as BFFT rounds its
// own. The index tables (DIF's IDX and KINV, BODFT's digit reversal, DIP's
// boundary permutations) are BFFT's constructions, copied.
//
// Tiers. A plan takes its kernels from simd::selected_level() when it is
// made and keeps the pointer, so no transform checks the CPU (simd.hpp:
// "a library resolves its kernel table from it once"). The plan's pointer is
// the plan's own field, not process-wide state.

#include "plan_state.hpp"

#include "algorithms/fft/plan32_ic_checks.hpp"
#include "algorithms/fft/plan64_ic_checks.hpp"

#include "core/subnormal.hpp"
#include "stxlib/error.hpp"
#include "stxlib/fft.hpp"
#include "stxlib/math.hpp"
#include "stxlib/memory.hpp"
#include "stxlib/result.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include <bit>
#include <new>

namespace stxlib::fft::detail {

const Kernels& kernels_for(const simd::Level level) noexcept {
#if defined(STXLIB_FFT_TIER_AVX2)
    if (level == simd::Level::avx2_fma) return avx2_fma::kernels;
#endif
#if defined(STXLIB_FFT_TIER_V128)
    if (level == simd::Level::avx2_fma || level == simd::Level::vector128) return v128::kernels;
#endif
    (void)level;
    return scalar::kernels;
}

namespace {

// Blocks are aligned for the widest vector a tier loads (BFFT's heap_array
// aligns to 64 bytes; every kernel load is unaligned, so this is speed, not
// correctness).
constexpr usize block_alignment = 64;

enum class Precision : u8 {
    float64,
    float32,
};

[[nodiscard]] ErrorRecord exhausted() noexcept {
    const ErrorRecord record = ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.fft.make");
    return record;
}

// A zeroed block of count elements of `size` bytes, recorded in the state
// so destroy_state releases it; null when the allocator refuses or the
// byte count is not representable (the caller then fails with
// out_of_memory, and destroy_state releases what was taken).
[[nodiscard]] void* take_block(PlanState& state, const usize count, const usize size) noexcept {
    if (state.block_count == plan_block_capacity) return nullptr;
    if (count == 0 || count > usize_max / size) return nullptr;
    const usize bytes = count * size;
    void* const block = memory::allocate(bytes, block_alignment);
    if (block == nullptr) return nullptr;
    __builtin_memset(block, 0, bytes);
    state.blocks[state.block_count] = block;
    state.block_count = state.block_count + 1;
    return block;
}

void destroy_state(PlanState* const state) noexcept {
    if (state == nullptr) return;
    PlanState& plan = *state;
    // Lets go of each block before releasing it (AGENTS.md, rule 1).
    for (usize index = 0; index < plan.block_count; ++index) {
        void* const block = plan.blocks[index];
        plan.blocks[index] = nullptr;
        if (block != nullptr) memory::release(block);
    }
    plan.block_count = 0;
    plan.~PlanState();
    memory::release(state);
}

// ---------------------------------------------------------------------------
// DIF tables (BFFT bruun_dif_kernel.hpp, init: IDX, C, KINV, TW, CF, TWF).
// ---------------------------------------------------------------------------

// BFFT's graydecode_int, bitrev_int and bruun_idx_int
// (bruun_simd_backend.hpp): leaf m of the Bruun tree holds bin IDX[m].
[[nodiscard]] i32 graydecode(i32 g) noexcept {
    for (i32 s = 1; s < 32; s <<= 1) g ^= g >> s;
    return g;
}

[[nodiscard]] i32 bitrev(i32 r, const i32 t) noexcept {
    i32 out = 0;
    for (i32 i = 0; i < t; ++i) {
        out = (out << 1) | (r & 1);
        r >>= 1;
    }
    return out;
}

[[nodiscard]] i32 bruun_idx(const i32 m, const i32 log2n) noexcept {
    const i32 t = 31 - std::countl_zero(static_cast<u32>(m));
    const i32 r = m ^ (1 << t);
    return (2 * graydecode(bitrev(r, t)) + 1) << ((log2n - 2) - t);
}

// The sine of leaf m's angle (BFFT's s_twiddle): its sibling's cosine for
// m > 1, and cos(pi/4) for m = 1.
[[nodiscard]] double dif_sine(const double* const c, const i32 m) noexcept {
    const i32 sibling = m > 1 ? (m ^ 1) : m;
    return c[sibling];
}

[[nodiscard]] bool build_dif(PlanState& state, const i32 n, const Precision precision) noexcept {
    DifTables& tables = state.dif;
    const i32 log2n = std::countr_zero(static_cast<u32>(n));
    const usize half = static_cast<usize>(n) / 2;
    tables.n = n;
    tables.log2n = log2n;

    i32* const idx = static_cast<i32*>(take_block(state, half, sizeof(i32)));
    i32* const kinv = static_cast<i32*>(take_block(state, half, sizeof(i32)));
    double* const c = static_cast<double*>(take_block(state, half, sizeof(double)));
    if (idx == nullptr || kinv == nullptr || c == nullptr) return false;

    // C[m] = cos(pi IDX[m] / N): IDX[m] / N is exact (N a power of two),
    // and BFFT's recurrence alpha(1) = pi/4, alpha(2m) = alpha(m)/2,
    // alpha(2m+1) = pi/2 - alpha(m)/2 names the same angles.
    idx[0] = 0;
    c[0] = 0.0;
    for (i32 m = 1; m < n / 2; ++m) {
        idx[m] = bruun_idx(m, log2n);
        c[m] = math::cospi(static_cast<double>(idx[m]) / static_cast<double>(n));
    }
    // KINV[k] = m such that IDX[m] = k (IDX is a bijection of [1, N/2)).
    kinv[0] = 0;
    for (i32 m = 1; m < n / 2; ++m) kinv[idx[m]] = m;
    tables.idx = idx;
    tables.kinv = kinv;
    tables.c = c;

    const usize leaf_blocks = static_cast<usize>(n) / 16;
    if (precision == Precision::float64) {
        double* const work = static_cast<double*>(take_block(state, static_cast<usize>(n), sizeof(double)));
        if (work == nullptr) return false;
        tables.work64 = work;
        if (n >= 32) {
            LeafTwiddles64* const tw =
                static_cast<LeafTwiddles64*>(take_block(state, leaf_blocks, sizeof(LeafTwiddles64)));
            if (tw == nullptr) return false;
            for (i32 m = 1; m < n / 16; ++m) {
                LeafTwiddles64& e = tw[m];
                for (i32 g = 0; g < 4; ++g) e.c4[g] = c[4 * m + g];
                e.c2[0] = c[2 * m];
                e.c2[1] = c[2 * m + 1];
                e.c1 = c[m];
                e.s1 = dif_sine(c, m);
            }
            tables.tw = tw;
        }
        return true;
    }

    float* const cf = static_cast<float*>(take_block(state, half, sizeof(float)));
    float* const work = static_cast<float*>(take_block(state, static_cast<usize>(n), sizeof(float)));
    if (cf == nullptr || work == nullptr) return false;
    for (usize m = 0; m < half; ++m) cf[m] = static_cast<float>(c[m]);
    tables.cf = cf;
    tables.work32 = work;
    if (n >= 32) {
        LeafTwiddles32* const twf = static_cast<LeafTwiddles32*>(take_block(state, leaf_blocks, sizeof(LeafTwiddles32)));
        if (twf == nullptr) return false;
        for (i32 m = 1; m < n / 16; ++m) {
            LeafTwiddles32& f = twf[m];
            for (i32 g = 0; g < 4; ++g) f.c4[g] = static_cast<float>(c[4 * m + g]);
            f.c2d[0] = static_cast<float>(c[2 * m]);
            f.c2d[1] = f.c2d[0];
            f.c2d[2] = static_cast<float>(c[2 * m + 1]);
            f.c2d[3] = f.c2d[2];
            f.c1 = static_cast<float>(c[m]);
            f.s1 = static_cast<float>(dif_sine(c, m));
        }
        tables.twf = twf;
    }
    return true;
}

// ---------------------------------------------------------------------------
// BODFT tables (BFFT bodft_kernel.hpp, plan_t's constructor).
// ---------------------------------------------------------------------------

// BFFT's gen_perm: the radix-4 digit reversal, the order in which the
// leaves consume input samples. Depth at most log4 N (15).
void bodft_perm(i32* const perm, const i32 base, const i32 stride, const i32 count, i32& index) noexcept {
    if (count == 2) {
        perm[index] = base;
        perm[index + 1] = base + stride;
        index = index + 2;
        return;
    }
    if (count == 4) {
        for (i32 j = 0; j < 4; ++j) {
            perm[index] = base + j * stride;
            index = index + 1;
        }
        return;
    }
    for (i32 r = 0; r < 4; ++r) bodft_perm(perm, base + r * stride, stride * 4, count / 4, index);
}

// e^(i pi x) as (cos, sin) through sincospi.
[[nodiscard]] Complex64 half_turn(const double x) noexcept {
    Complex64 result;
    math::sincospi(x, result.im, result.re);
    return result;
}

[[nodiscard]] Complex32 rounded(const Complex64 value) noexcept {
    const Complex32 result{.re = static_cast<float>(value.re), .im = static_cast<float>(value.im)};
    return result;
}

[[nodiscard]] bool build_bodft(PlanState& state, const i32 n, const Precision precision) noexcept {
    BodftTables& tables = state.bodft;
    const i32 p = std::countr_zero(static_cast<u32>(n));
    const bool leaf4 = (p & 1) == 0;  // p even: radix-4 leaves, else radix-2
    tables.n = n;
    tables.leafsize = leaf4 ? 4 : 2;
    tables.leafbins = tables.leafsize / 2;
    tables.numleaves = n / tables.leafsize;
    tables.passes = 0;
    usize twiddles = 0;
    for (i32 s = tables.leafsize * 4; s <= n; s <<= 2) {
        tables.passes = tables.passes + 1;
        twiddles = twiddles + 3 * static_cast<usize>(s / 8);
    }

    // Radix-4 leaf root: e^(-i pi/4).
    tables.root4 = half_turn(-0.25);
    tables.root4f = rounded(tables.root4);

    i32* const perm = static_cast<i32*>(take_block(state, static_cast<usize>(n), sizeof(i32)));
    if (perm == nullptr) return false;
    i32 index = 0;
    bodft_perm(perm, 0, 1, n, index);
    tables.perm = perm;

    // Per combine level s, the powers t, t^2, t^3 of t_k = e^(-2 pi i (k + 1/2)/s),
    // k = 0 .. s/8 - 1, each straight from the multiplied angle as BFFT
    // builds them: angle / pi = -(2k + 1)/s, times 1, 2 and 3. BFFT fills
    // every level s = 8 .. N; the combine passes read only s = 4 leafsize,
    // 16 leafsize, ... <= N, which are the levels filled here.
    const usize element = precision == Precision::float64 ? sizeof(Complex64) : sizeof(Complex32);
    void* const block = twiddles == 0 ? nullptr : take_block(state, twiddles, element);
    if (twiddles != 0 && block == nullptr) return false;
    usize offset = 0;
    for (i32 s = tables.leafsize * 4; s <= n; s <<= 2) {
        const i32 lg = std::countr_zero(static_cast<u32>(s));
        const i32 length = s / 8;
        const double scale = static_cast<double>(s);
        if (precision == Precision::float64) {
            Complex64* const base = static_cast<Complex64*>(block) + offset;
            Complex64* const t1 = base;
            Complex64* const t2 = base + length;
            Complex64* const t3 = base + 2 * length;
            for (i32 k = 0; k < length; ++k) {
                const double odd = static_cast<double>(2 * k + 1);
                t1[k] = half_turn(-odd / scale);
                t2[k] = half_turn(-2.0 * odd / scale);
                t3[k] = half_turn(-3.0 * odd / scale);
            }
            tables.level64[lg] = BodftLevel64{.t1 = t1, .t2 = t2, .t3 = t3};
        } else {
            Complex32* const base = static_cast<Complex32*>(block) + offset;
            Complex32* const t1 = base;
            Complex32* const t2 = base + length;
            Complex32* const t3 = base + 2 * length;
            for (i32 k = 0; k < length; ++k) {
                const double odd = static_cast<double>(2 * k + 1);
                t1[k] = rounded(half_turn(-odd / scale));
                t2[k] = rounded(half_turn(-2.0 * odd / scale));
                t3[k] = rounded(half_turn(-3.0 * odd / scale));
            }
            tables.level32[lg] = BodftLevel32{.t1 = t1, .t2 = t2, .t3 = t3};
        }
        offset = offset + 3 * static_cast<usize>(length);
    }

    // Two ping-pong work buffers of N/2 complex each.
    void* const scratch = take_block(state, static_cast<usize>(n), element);
    if (scratch == nullptr) return false;
    if (precision == Precision::float64) {
        tables.scratch64 = static_cast<Complex64*>(scratch);
    } else {
        tables.scratch32 = static_cast<Complex32*>(scratch);
    }
    return true;
}

// ---------------------------------------------------------------------------
// DIP tables (BFFT bruun_dip_kernel.hpp, reset).
// ---------------------------------------------------------------------------

// BFFT's build_tperm. Universal within-subtree boundary permutations:
// T_w[rank] = tree position of the rank-th smallest leaf bin, identical for
// every subtree of row width w. The leaves of a subtree are m*e + s*d with
// (m, s) path-only: lo child keeps (m, s), the hi child at depth j maps to
// (2^j - m, -s). Ascending-bin rank of (m, s) is closed-form: 0 for m == 0,
// else 2m - 1 + (s > 0). Depth log2 w <= 12.
void dip_tperm(i32* const table, i32& pos, const i32 m, const i32 s, const i32 j, const i32 w) noexcept {
    if (w == 1) {
        const i32 rank = (m == 0) ? 0 : (2 * m - 1 + (s > 0 ? 1 : 0));
        table[rank] = pos;
        pos = pos + 1;
        return;
    }
    const i32 w2 = w >> 1;
    dip_tperm(table, pos, m, s, j + 1, w2);
    dip_tperm(table, pos, (1 << j) - m, -s, j + 1, w2);
}

[[nodiscard]] bool build_dip(PlanState& state, const i32 n) noexcept {
    DipTables& tables = state.dip;
    const i32 half = n >> 1;
    const usize entries = static_cast<usize>(half) + 1;
    tables.n = n;
    tables.half = half;

    // [ cos : half + 1 | sin : half + 1 | cs : 2 (half + 1) ]
    double* const trig = static_cast<double*>(take_block(state, 4 * entries, sizeof(double)));
    i32* const tperm = static_cast<i32*>(take_block(state, 2 * static_cast<usize>(dip_egress_width) - 1, sizeof(i32)));
    double* const work = static_cast<double*>(take_block(state, static_cast<usize>(n), sizeof(double)));
    if (trig == nullptr || tperm == nullptr || work == nullptr) return false;
    double* const cos_table = trig;
    double* const sin_table = trig + entries;
    double* const cs = trig + 2 * entries;

    // theta = 2 pi k / N: in half-turns 2k / N, exact.
    for (i32 k = 0; k <= half; ++k) {
        math::sincospi(static_cast<double>(2 * k) / static_cast<double>(n), sin_table[k], cos_table[k]);
    }
    // BFFT: enforce quarter-wave symmetry bit-exactly on the cell range
    // [0, n/4]: sibling spans (d, 2e) and (e-d, 2e) have complementary
    // angles whose table indices sum to exactly n/4, so with
    // sin_[k] == cos_[n/4 - k] by construction the mirror twiddle sharing
    // (one load + swap serves both siblings) is bit-identical to loading
    // both.
    const i32 quarter = n >> 2;
    for (i32 k = 0; 2 * k <= quarter; ++k) {
        sin_table[quarter - k] = cos_table[k];
        cos_table[quarter - k] = sin_table[k];
    }
    // Interleaved copy for the paired-lane leaf cells: one vector load
    // yields [c, s] instead of two scalar loads.
    for (i32 k = 0; k <= half; ++k) {
        cs[2 * k] = cos_table[k];
        cs[2 * k + 1] = sin_table[k];
    }
    // T_w at offset w - 1, w = 1, 2, 4 .. dip_egress_width.
    for (i32 w = 1; w <= dip_egress_width; w <<= 1) {
        i32 pos = 0;
        dip_tperm(tperm + (w - 1), pos, 0, 1, 0, w);
    }
    tables.cos = cos_table;
    tables.sin = sin_table;
    tables.cs = cs;
    tables.tperm = tperm;
    tables.work = work;
    return true;
}

// make's clauses, as include/stxlib/fft_plan64.ic.toml and fft_plan32.ic.toml
// state them (the generated checks; the float32 plan's own differ only in
// refusing dip).
[[nodiscard]] bool mode_admitted(const u8 mode, const bool float64) noexcept {
    const bool result = float64 ? ::stxlib::detail::ic::fft_plan64::admits_make_mode(mode)
                                : ::stxlib::detail::ic::fft_plan32::admits_make_mode(mode);
    return result;
}

[[nodiscard]] bool size_within_limit(const usize size, const bool float64) noexcept {
    const bool result = float64 ? ::stxlib::detail::ic::fft_plan64::admits_make_size_limit(size)
                                : ::stxlib::detail::ic::fft_plan32::admits_make_size_limit(size);
    return result;
}

[[nodiscard]] bool size_shaped(const usize size, const u8 mode, const bool float64) noexcept {
    const bool result = float64 ? ::stxlib::detail::ic::fft_plan64::admits_make_size_shape(size, mode)
                                : ::stxlib::detail::ic::fft_plan32::admits_make_size_shape(size, mode);
    return result;
}

// A new plan state at `level`, its tables built; null with `failure` set
// when the arguments are refused or an allocation fails.
[[nodiscard]] PlanState* make_state(const usize size, const Mode mode, const Precision precision,
                                    const simd::Level level, ErrorRecord& failure) noexcept {
    const u8 mode_value = static_cast<u8>(mode);
    const bool float64 = precision == Precision::float64;
    if (!mode_admitted(mode_value, float64)) {
        failure = ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fft.make",
                                    ErrorDetail{.message = "the mode is not plain, shifted or dip"});
        return nullptr;
    }
    if (!size_within_limit(size, float64)) {
        failure = ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.fft.make",
                                    ErrorDetail{.message = "the size is above fft::max_size (2^30)"});
        return nullptr;
    }
    if (!size_shaped(size, mode_value, float64)) {
        failure = ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fft.make",
                                    ErrorDetail{.message = "the size is not a power of two at least the mode's minimum"});
        return nullptr;
    }
    if (!float64 && !::stxlib::detail::ic::fft_plan32::admits_make_float32_mode(mode_value)) {
        failure = ErrorRecord::make(ErrorKind::unsupported, "stxlib.fft.make",
                                    ErrorDetail{.message = "the dip mode is float64 only"});
        return nullptr;
    }
    void* const block = memory::allocate(sizeof(PlanState), alignof(PlanState));
    if (block == nullptr) {
        failure = exhausted();
        return nullptr;
    }
    PlanState* const state = ::new (block) PlanState();
    PlanState& plan = *state;
    plan.size = size;
    plan.mode = mode;
    plan.level = level;
    plan.kernels = &kernels_for(level);
    const i32 n = static_cast<i32>(size);
    bool built = false;
    if (mode == Mode::plain) built = build_dif(plan, n, precision);
    if (mode == Mode::shifted) built = build_bodft(plan, n, precision);
    if (mode == Mode::dip) built = build_dip(plan, n);
    if (built) {
        const usize element = precision == Precision::float32 ? sizeof(float) : sizeof(double);
        plan.staging = take_block(plan, size + 2, element);
        built = plan.staging != nullptr;
    }
    if (!built) {
        destroy_state(state);
        failure = exhausted();
        return nullptr;
    }
    return state;
}

[[nodiscard]] usize bins_of(const PlanState* const state) noexcept {
    if (state == nullptr) return 0;
    const PlanState& plan = *state;
    if (plan.mode == Mode::shifted) return plan.size / 2;
    return plan.size / 2 + 1;
}

// Whether two byte ranges share a byte (the transforms' pointers are
// restricted: input and output never overlap).
[[nodiscard]] bool overlap(const void* const a, const usize a_bytes, const void* const b, const usize b_bytes) noexcept {
    const usize a_begin = reinterpret_cast<usize>(a);
    const usize b_begin = reinterpret_cast<usize>(b);
    return a_begin < b_begin + b_bytes && b_begin < a_begin + a_bytes;
}

// Why a transform is refused; none lets it go ahead.
enum class Refusal : u8 {
    none,
    empty_plan,
    input_count,
    output_count,
    overlap,
};

[[nodiscard]] ErrorRecord refused(const Refusal refusal) noexcept {
    TextView message = "the transform was refused";
    switch (refusal) {
    case Refusal::empty_plan:
        message = "the plan is empty";
        break;
    case Refusal::input_count:
        message = "the input count is not the plan's";
        break;
    case Refusal::output_count:
        message = "the output count is not the plan's";
        break;
    case Refusal::overlap:
        message = "the input and output overlap";
        break;
    case Refusal::none:
    default:
        break;
    }
    const ErrorRecord record =
        ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fft.transform", ErrorDetail{.message = message});
    return record;
}

// The transforms' count clauses, as the declarations state them (the
// generated checks of Plan64's and Plan32's rfft and irfft; `input` picks the
// input's clause, else the output's).
[[nodiscard]] bool counts_admitted(const usize input_count, const usize output_count, const usize samples,
                                   const usize bins, const bool forward, const bool float64,
                                   const bool input) noexcept {
    namespace plan64 = ::stxlib::detail::ic::fft_plan64;
    namespace plan32 = ::stxlib::detail::ic::fft_plan32;
    if (forward && input) {
        if (float64) return plan64::admits_rfft_input(input_count, samples);
        return plan32::admits_rfft_input(input_count, samples);
    }
    if (forward) {
        if (float64) return plan64::admits_rfft_output(output_count, bins);
        return plan32::admits_rfft_output(output_count, bins);
    }
    if (input) {
        if (float64) return plan64::admits_irfft_input(input_count, bins);
        return plan32::admits_irfft_input(input_count, bins);
    }
    if (float64) return plan64::admits_irfft_output(output_count, samples);
    return plan32::admits_irfft_output(output_count, samples);
}

// The checks every transform makes before it writes: a plan, the counts,
// no overlap.
[[nodiscard]] Refusal transform_refusal(const PlanState* const state, const usize input_count,
                                        const usize input_element, const void* const input,
                                        const usize output_count, const usize output_element,
                                        const void* const output, const bool forward, const bool float64) noexcept {
    if (state == nullptr) return Refusal::empty_plan;
    const usize samples = (*state).size;
    const usize bins = bins_of(state);
    if (!counts_admitted(input_count, output_count, samples, bins, forward, float64, true)) return Refusal::input_count;
    if (!counts_admitted(input_count, output_count, samples, bins, forward, float64, false)) {
        return Refusal::output_count;
    }
    if (overlap(input, input_count * input_element, output, output_count * output_element)) {
        return Refusal::overlap;
    }
    return Refusal::none;
}

// STXlib's subnormal rule (A1 rule 4) on a transform's results: a subnormal
// output is zero of its sign.
void flush_outputs(double* const values, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) values[index] = stxlib::detail::flush_subnormal(values[index]);
}

void flush_outputs(float* const values, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) values[index] = stxlib::detail::flush_subnormal(values[index]);
}

void flush_outputs(Complex64* const values, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        Complex64& value = values[index];
        value.re = stxlib::detail::flush_subnormal(value.re);
        value.im = stxlib::detail::flush_subnormal(value.im);
    }
}

void flush_outputs(Complex32* const values, const usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        Complex32& value = values[index];
        value.re = stxlib::detail::flush_subnormal(value.re);
        value.im = stxlib::detail::flush_subnormal(value.im);
    }
}

// STXlib's subnormal rule on a transform's arguments: a subnormal input is
// read as zero of its sign. The input itself when it holds no subnormal (no
// copy), else the plan's staging block holding the flushed copy; the kernels
// then read the staging block, never the caller's memory. Bit-for-bit the
// same as flushing every input value, since the rest pass unchanged.
[[nodiscard]] bool subnormal(const double value) noexcept {
    return value != 0.0 && stxlib::detail::flush_subnormal(value) == 0.0;
}

[[nodiscard]] bool subnormal(const float value) noexcept {
    return value != 0.0F && stxlib::detail::flush_subnormal(value) == 0.0F;
}

template <typename Real>
[[nodiscard]] const Real* flushed_reals(const PlanState& plan, const Real* const values, const usize count) noexcept {
    bool found = false;
    for (usize index = 0; index < count && !found; ++index) found = subnormal(values[index]);
    if (!found) return values;
    Real* const staging = static_cast<Real*>(plan.staging);
    for (usize index = 0; index < count; ++index) staging[index] = stxlib::detail::flush_subnormal(values[index]);
    return staging;
}

[[nodiscard]] const double* flushed_inputs(const PlanState& plan, const double* const values,
                                           const usize count) noexcept {
    return flushed_reals(plan, values, count);
}

[[nodiscard]] const float* flushed_inputs(const PlanState& plan, const float* const values,
                                          const usize count) noexcept {
    return flushed_reals(plan, values, count);
}

// A complex input is flushed part by part.
template <typename Complex>
[[nodiscard]] const Complex* flushed_complex(const PlanState& plan, const Complex* const values,
                                             const usize count) noexcept {
    bool found = false;
    for (usize index = 0; index < count && !found; ++index) {
        const Complex& value = values[index];
        found = subnormal(value.re) || subnormal(value.im);
    }
    if (!found) return values;
    Complex* const staging = static_cast<Complex*>(plan.staging);
    for (usize index = 0; index < count; ++index) {
        const Complex& value = values[index];
        Complex& copy = staging[index];
        copy.re = stxlib::detail::flush_subnormal(value.re);
        copy.im = stxlib::detail::flush_subnormal(value.im);
    }
    return staging;
}

[[nodiscard]] const Complex64* flushed_inputs(const PlanState& plan, const Complex64* const values,
                                              const usize count) noexcept {
    static_assert(sizeof(Complex64) == 2 * sizeof(double) && alignof(Complex64) <= block_alignment);
    return flushed_complex(plan, values, count);
}

[[nodiscard]] const Complex32* flushed_inputs(const PlanState& plan, const Complex32* const values,
                                              const usize count) noexcept {
    static_assert(sizeof(Complex32) == 2 * sizeof(float) && alignof(Complex32) <= block_alignment);
    return flushed_complex(plan, values, count);
}

} // namespace

// ---------------------------------------------------------------------------
// PlanAccess: the factories behind make, at a chosen tier.
// ---------------------------------------------------------------------------

OperationResult<Plan64> PlanAccess::make64(const usize size, const Mode mode, const simd::Level level) noexcept {
    ErrorRecord failure = ErrorRecord::make(ErrorKind::internal_failure, "stxlib.fft.make");
    PlanState* const state = make_state(size, mode, Precision::float64, level, failure);
    if (state == nullptr) return OperationResult<Plan64>::failure(failure);
    Plan64 plan;
    plan.state_ = state;
    return OperationResult<Plan64>::success(static_cast<Plan64&&>(plan));
}

OperationResult<Plan32> PlanAccess::make32(const usize size, const Mode mode, const simd::Level level) noexcept {
    ErrorRecord failure = ErrorRecord::make(ErrorKind::internal_failure, "stxlib.fft.make");
    PlanState* const state = make_state(size, mode, Precision::float32, level, failure);
    if (state == nullptr) return OperationResult<Plan32>::failure(failure);
    Plan32 plan;
    plan.state_ = state;
    return OperationResult<Plan32>::success(static_cast<Plan32&&>(plan));
}

simd::Level PlanAccess::level(const Plan64& plan) noexcept {
    if (plan.state_ == nullptr) return simd::Level::scalar;
    return (*plan.state_).level;
}

simd::Level PlanAccess::level(const Plan32& plan) noexcept {
    if (plan.state_ == nullptr) return simd::Level::scalar;
    return (*plan.state_).level;
}

} // namespace stxlib::fft::detail

namespace stxlib::fft {

// ---------------------------------------------------------------------------
// Plan64
// ---------------------------------------------------------------------------

OperationResult<Plan64> Plan64::make(const usize size, const Mode mode) noexcept {
    return detail::PlanAccess::make64(size, mode, simd::selected_level());
}

Plan64::Plan64(Plan64&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Plan64& Plan64::operator=(Plan64&& other) noexcept {
    if (this == &other) return *this;
    detail::PlanState* const taken = other.state_;
    other.state_ = nullptr;
    detail::PlanState* const previous = state_;
    state_ = taken;
    if (previous != nullptr) detail::destroy_state(previous);
    return *this;
}

Plan64::~Plan64() { release(); }

void Plan64::release() noexcept {
    detail::PlanState* const held = state_;
    state_ = nullptr;
    detail::destroy_state(held);
}

bool Plan64::empty() const noexcept { return state_ == nullptr; }
usize Plan64::size() const noexcept { return state_ == nullptr ? 0 : (*state_).size; }
usize Plan64::bins() const noexcept { return detail::bins_of(state_); }
Mode Plan64::mode() const noexcept { return state_ == nullptr ? Mode::plain : (*state_).mode; }

OperationStatus Plan64::rfft(const View<double> input, const MutableView<Complex64> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(double), input.data(),
                                                          output.count(), sizeof(Complex64), output.data(), true, true);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const double* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) kernels.dif_forward64(plan.dif, source, output.data());
    if (plan.mode == Mode::shifted) kernels.bodft_forward64(plan.bodft, source, output.data());
    if (plan.mode == Mode::dip) kernels.dip_forward64(plan.dip, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

OperationStatus Plan64::irfft(const View<Complex64> input, const MutableView<double> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(Complex64), input.data(),
                                                          output.count(), sizeof(double), output.data(), false, true);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const Complex64* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) kernels.dif_inverse64(plan.dif, source, output.data());
    if (plan.mode == Mode::shifted) kernels.bodft_inverse64(plan.bodft, source, output.data());
    if (plan.mode == Mode::dip) kernels.dip_inverse64(plan.dip, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

// ---------------------------------------------------------------------------
// Plan32
// ---------------------------------------------------------------------------

OperationResult<Plan32> Plan32::make(const usize size, const Mode mode) noexcept {
    return detail::PlanAccess::make32(size, mode, simd::selected_level());
}

Plan32::Plan32(Plan32&& other) noexcept : state_(other.state_) { other.state_ = nullptr; }

Plan32& Plan32::operator=(Plan32&& other) noexcept {
    if (this == &other) return *this;
    detail::PlanState* const taken = other.state_;
    other.state_ = nullptr;
    detail::PlanState* const previous = state_;
    state_ = taken;
    if (previous != nullptr) detail::destroy_state(previous);
    return *this;
}

Plan32::~Plan32() { release(); }

void Plan32::release() noexcept {
    detail::PlanState* const held = state_;
    state_ = nullptr;
    detail::destroy_state(held);
}

bool Plan32::empty() const noexcept { return state_ == nullptr; }
usize Plan32::size() const noexcept { return state_ == nullptr ? 0 : (*state_).size; }
usize Plan32::bins() const noexcept { return detail::bins_of(state_); }
Mode Plan32::mode() const noexcept { return state_ == nullptr ? Mode::plain : (*state_).mode; }

OperationStatus Plan32::rfft(const View<float> input, const MutableView<Complex32> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(float), input.data(),
                                                          output.count(), sizeof(Complex32), output.data(), true,
                                                          false);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const float* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) kernels.dif_forward32(plan.dif, source, output.data());
    if (plan.mode == Mode::shifted) kernels.bodft_forward32(plan.bodft, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

OperationStatus Plan32::irfft(const View<Complex32> input, const MutableView<float> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(Complex32), input.data(),
                                                          output.count(), sizeof(float), output.data(), false, false);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const Complex32* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) kernels.dif_inverse32(plan.dif, source, output.data());
    if (plan.mode == Mode::shifted) kernels.bodft_inverse32(plan.bodft, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

} // namespace stxlib::fft
