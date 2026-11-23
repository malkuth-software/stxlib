// stx_fft's plans (include/stxlib/fft.hpp): validation, the tables of the
// four kernels, and the calls into the SIMD tier the plan was made for.
//
// Tables. BFFT computes its twiddles with libm (BODFT, DIP, DIT) or a
// half-angle sqrt recurrence (DIF). Here every one comes from STXlib's
// half-turn functions (stxlib/math.hpp: sincospi, cospi; owner,
// 2026-11-22), whose arguments are exact here: each angle is a rational
// multiple of pi with a power-of-two denominator, so the argument in
// half-turns is an exact double and the value is the half-turn function's
// (within 0.5002 ulp). float tables are the double values rounded to float,
// as BFFT rounds its own. The index tables (DIF's IDX, KINV, native order
// and op schedules, DIT's bit reversal, BODFT's digit reversal, DIP's
// boundary permutations) are BFFT's constructions, copied.
//
// Engines. The plain mode has two kernels, BFFT's DIF and DIT; a plan runs
// the ones engine_policy gives its size and tier (PlanAccess can choose
// them for tests and the benchmark), and builds DIT's tables only when it
// runs DIT.
//
// Tiers. A plan takes its kernels from simd::selected_level() when it is
// made and keeps the pointer, so no transform checks the CPU (simd.hpp:
// "a library resolves its kernel table from it once"). The plan's pointer is
// the plan's own field, not process-wide state.

#include "plan_state.hpp"

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

// BFFT's native (heap-optimised) order, init under
// BRUUN_HEAPOPT_SPECTRUM_ORDER: every Bruun factor node must keep a
// contiguous interval of final leaves, and inside each level the sibling
// orientations are chosen to reduce adjacent frequency travel. Level by
// level from bin M/2 (M = N/2), each even bin k of the level before gives
// the pair (k/2, M - k/2); a dynamic program over the pairs picks each
// pair's orientation, minimising first the largest and then the summed
// cyclic distance between neighbours, and the oriented pairs are the next
// level. outidx[m] is leaf m's position in that order (BFFT's NATIVE_POS
// and OUTIDX). BFFT's lambdas and growable arrays are functions and one
// scratch block here; the order is the same.
struct NativeScratch final {
    i32* order{nullptr};       // [M]: the bins in native order
    i32* first{nullptr};       // [M/2]: each pair, as BFFT's int_pair
    i32* second{nullptr};
    i32* max0{nullptr};        // [M/2]: the program's costs per orientation
    i32* max1{nullptr};
    i64* sum0{nullptr};
    i64* sum1{nullptr};
    u8* back0{nullptr};        // [M/2]: back pointers
    u8* back1{nullptr};
};

[[nodiscard]] i32 cyclic_distance(const i32 a, const i32 b, const i32 period) noexcept {
    const i32 d = a > b ? a - b : b - a;
    return d < period - d ? d : period - d;
}

// BFFT's better: lexicographic on (largest, sum).
[[nodiscard]] bool better(const i32 max_a, const i64 sum_a, const i32 max_b, const i64 sum_b) noexcept {
    return max_a < max_b || (max_a == max_b && sum_a < sum_b);
}

// Appends the level after order[begin, end) to order[end ...]; returns the
// new end (end itself when the level has no even bin).
[[nodiscard]] usize native_level(const NativeScratch& s, const usize begin, const usize end,
                                 const i32 period) noexcept {
    usize pairs = 0;
    for (usize index = begin; index < end; ++index) {
        const i32 k = s.order[index];
        if ((k & 1) != 0) continue;
        s.first[pairs] = k / 2;
        s.second[pairs] = period - k / 2;
        pairs = pairs + 1;
    }
    if (pairs == 0) return end;

    // Orientation 0 runs first -> second, orientation 1 second -> first.
    s.max0[0] = 0;
    s.max1[0] = 0;
    s.sum0[0] = 0;
    s.sum1[0] = 0;
    s.back0[0] = 0;
    s.back1[0] = 0;
    for (usize pi = 1; pi < pairs; ++pi) {
        for (i32 o = 0; o < 2; ++o) {
            const i32 start = o == 0 ? s.first[pi] : s.second[pi];
            const i32 j0 = cyclic_distance(s.second[pi - 1], start, period);  // previous ran orientation 0
            const i32 cand0_max = s.max0[pi - 1] > j0 ? s.max0[pi - 1] : j0;
            const i64 cand0_sum = s.sum0[pi - 1] + j0;
            const i32 j1 = cyclic_distance(s.first[pi - 1], start, period);   // previous ran orientation 1
            const i32 cand1_max = s.max1[pi - 1] > j1 ? s.max1[pi - 1] : j1;
            const i64 cand1_sum = s.sum1[pi - 1] + j1;
            const bool take0 = better(cand0_max, cand0_sum, cand1_max, cand1_sum);
            const i32 best_max = take0 ? cand0_max : cand1_max;
            const i64 best_sum = take0 ? cand0_sum : cand1_sum;
            const u8 back = take0 ? u8{0} : u8{1};
            if (o == 0) {
                s.max0[pi] = best_max;
                s.sum0[pi] = best_sum;
                s.back0[pi] = back;
            } else {
                s.max1[pi] = best_max;
                s.sum1[pi] = best_sum;
                s.back1[pi] = back;
            }
        }
    }

    // Walk the back pointers from the cheaper end; the chosen orientation
    // of pair pi is kept in back0[pi]'s place once it has been read (the
    // walk reads each pair's back pointers before overwriting them).
    u8 choose = better(s.max0[pairs - 1], s.sum0[pairs - 1], s.max1[pairs - 1], s.sum1[pairs - 1]) ? u8{0} : u8{1};
    for (usize rr = pairs; rr-- > 0;) {
        const u8 previous = choose == 0 ? s.back0[rr] : s.back1[rr];
        s.back0[rr] = choose;
        choose = previous;
    }
    usize next = end;
    for (usize pi = 0; pi < pairs; ++pi) {
        const bool forward = s.back0[pi] == 0;
        s.order[next] = forward ? s.first[pi] : s.second[pi];
        s.order[next + 1] = forward ? s.second[pi] : s.first[pi];
        next = next + 2;
    }
    return next;
}

// outidx[m] (leaf m's native slot) and standard_native[k] (bin k's native
// slot, BFFT's STANDARD_NATIVE_POS) for N >= 32; false when the scratch
// block is refused.
[[nodiscard]] bool build_native_order(const i32 n, const i32* const kinv, i32* const outidx,
                                      i32* const standard_native) noexcept {
    const usize period = static_cast<usize>(n) / 2;
    const usize pairs = period / 2;
    const usize bytes = period * sizeof(i32) + pairs * (4 * sizeof(i32) + 2 * sizeof(i64) + 2 * sizeof(u8));
    void* const block = memory::allocate(bytes, alignof(i64));
    if (block == nullptr) return false;
    // i64 first, then i32, then u8: each run stays aligned.
    i64* const wide = static_cast<i64*>(block);
    i32* const narrow = reinterpret_cast<i32*>(wide + 2 * pairs);
    u8* const bytes_run = reinterpret_cast<u8*>(narrow + period + 4 * pairs);
    const NativeScratch scratch{
        .order = narrow,
        .first = narrow + period,
        .second = narrow + period + pairs,
        .max0 = narrow + period + 2 * pairs,
        .max1 = narrow + period + 3 * pairs,
        .sum0 = wide,
        .sum1 = wide + pairs,
        .back0 = bytes_run,
        .back1 = bytes_run + pairs,
    };

    const i32 m_count = static_cast<i32>(period);
    scratch.order[0] = m_count / 2;
    usize begin = 0;
    usize end = 1;
    while (true) {
        const usize next = native_level(scratch, begin, end, m_count);
        if (next == end) break;
        begin = end;
        end = next;
    }

    for (i32 m = 0; m < m_count; ++m) outidx[m] = 0;
    i32 pos = 1;
    for (usize index = 0; index < end; ++index) {
        const i32 m = kinv[scratch.order[index]];
        if (m <= 0 || m >= m_count) continue;
        outidx[m] = pos;
        pos = pos + 1;
    }
    for (i32 m = 1; m < m_count; ++m) {
        if (outidx[m] != 0) continue;
        outidx[m] = pos;
        pos = pos + 1;
    }
    memory::release(block);

    standard_native[0] = 0;
    for (i32 k = 1; k < m_count; ++k) standard_native[k] = outidx[kinv[k]];
    return true;
}

// BFFT's append_segment_schedule, as a recursion in the order BFFT's stack
// pops (the node, then its children base, base + q, base + 2q, base + 3q):
// writes the ops of the segment (base, q, m) from ops[count] when ops is
// not null; returns the new count either way.
[[nodiscard]] u32 append_segment(DifOp* const ops, u32 count, const u32 base, const i32 q, const i32 m) noexcept {
    const DifOpKind kind = q >= 16 ? DifOpKind::norm2 : (q == 8 ? DifOpKind::codelet_q8 : DifOpKind::codelet_d3);
    if (ops != nullptr) {
        ops[count] = DifOp{.base = base, .q = static_cast<u32>(q), .m = static_cast<u32>(m), .kind = kind};
    }
    count = count + 1;
    if (q < 16) return count;
    const i32 qq = q >> 2;
    for (i32 child = 0; child < 4; ++child) {
        count = append_segment(ops, count, base + static_cast<u32>(child * q), qq, 4 * m + child);
    }
    return count;
}

[[nodiscard]] u32 append_op(DifOp* const ops, const u32 count, const DifOpKind kind, const u32 base, const u32 q,
                            const u32 m) noexcept {
    if (ops != nullptr) ops[count] = DifOp{.base = base, .q = q, .m = m, .kind = kind};
    return count + 1;
}

// BFFT's build_forward_schedules for N >= 64: the fused-scatter ops
// (fused) or the residue ops; returns the count, writing when ops is not
// null.
[[nodiscard]] u32 forward_schedule(DifOp* const ops, const i32 n, const bool fused) noexcept {
    u32 count = 0;
    for (i32 h = n / 2; h >= 32; h >>= 1) {
        count = append_segment(ops, count, static_cast<u32>(h), h >> 2, 1);
        count = append_op(ops, count, DifOpKind::binomial, 0, static_cast<u32>(h >> 1), 0);
    }
    if (fused) {
        count = append_op(ops, count, DifOpKind::spine_d3, 16, 0, 1);
        count = append_op(ops, count, DifOpKind::binomial, 0, 8, 0);
        count = append_op(ops, count, DifOpKind::spine_d2, 8, 0, 1);
        count = append_op(ops, count, DifOpKind::binomial, 0, 4, 0);
        count = append_op(ops, count, DifOpKind::spine_d1, 4, 0, 1);
        count = append_op(ops, count, DifOpKind::binomial, 0, 2, 0);
        count = append_op(ops, count, DifOpKind::spine_leaf, 2, 0, 1);
        count = append_op(ops, count, DifOpKind::dc_nyquist, 0, 0, 0);
        return count;
    }
    count = append_op(ops, count, DifOpKind::spine_d3, 16, 0, 1);
    count = append_op(ops, count, DifOpKind::binomial, 0, 8, 0);
    count = append_op(ops, count, DifOpKind::spine_norm2, 8, 2, 1);
    count = append_op(ops, count, DifOpKind::binomial, 0, 4, 0);
    count = append_op(ops, count, DifOpKind::spine_d1, 4, 0, 1);
    count = append_op(ops, count, DifOpKind::binomial, 0, 2, 0);
    return count;
}

// The three schedules in one block (N >= 64): fused, residue, and the
// inverse (the residue ops reversed, then BINOMIAL N/2; BFFT's
// build_inverse_residue_schedule).
[[nodiscard]] bool build_schedules(PlanState& state, DifTables& tables, const i32 n) noexcept {
    const u32 fused_count = forward_schedule(nullptr, n, true);
    const u32 residue_count = forward_schedule(nullptr, n, false);
    const u32 inverse_count = residue_count + 1;
    const usize total = static_cast<usize>(fused_count) + residue_count + inverse_count;
    DifOp* const ops = static_cast<DifOp*>(take_block(state, total, sizeof(DifOp)));
    if (ops == nullptr) return false;
    DifOp* const fused = ops;
    DifOp* const residue = ops + fused_count;
    DifOp* const inverse = residue + residue_count;
    (void)forward_schedule(fused, n, true);
    (void)forward_schedule(residue, n, false);
    for (u32 index = 0; index < residue_count; ++index) inverse[index] = residue[residue_count - 1 - index];
    inverse[residue_count] = DifOp{.base = 0, .q = static_cast<u32>(n / 2), .m = 0, .kind = DifOpKind::binomial};
    tables.forward_ops = fused;
    tables.forward_op_count = fused_count;
    tables.residue_ops = residue;
    tables.residue_op_count = residue_count;
    tables.inverse_ops = inverse;
    tables.inverse_op_count = inverse_count;
    return true;
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
    if (n >= 64 && !build_schedules(state, tables, n)) return false;

    const usize leaf_blocks = static_cast<usize>(n) / 16;
    if (precision == Precision::float64) {
        double* const work = static_cast<double*>(take_block(state, static_cast<usize>(n), sizeof(double)));
        if (work == nullptr) return false;
        tables.work64 = work;
        if (n >= 32) {
            i32* const outidx = static_cast<i32*>(take_block(state, half, sizeof(i32)));
            i32* const standard_native = static_cast<i32*>(take_block(state, half, sizeof(i32)));
            Complex64* const native = static_cast<Complex64*>(take_block(state, half + 1, sizeof(Complex64)));
            LeafTwiddles64* const tw =
                static_cast<LeafTwiddles64*>(take_block(state, leaf_blocks, sizeof(LeafTwiddles64)));
            if (outidx == nullptr || standard_native == nullptr || native == nullptr || tw == nullptr) return false;
            if (!build_native_order(n, kinv, outidx, standard_native)) return false;
            for (i32 m = 1; m < n / 16; ++m) {
                LeafTwiddles64& e = tw[m];
                for (i32 g = 0; g < 4; ++g) e.c4[g] = c[4 * m + g];
                e.c2[0] = c[2 * m];
                e.c2[1] = c[2 * m + 1];
                e.c1 = c[m];
                e.s1 = dif_sine(c, m);
                for (i32 j = 0; j < 8; ++j) e.idx[j] = outidx[8 * m + j];
            }
            tables.outidx = outidx;
            tables.standard_native = standard_native;
            tables.native64 = native;
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
// DIT tables (BFFT bruun_dit_kernel.hpp, reset), N >= 16.
// ---------------------------------------------------------------------------

// rev (bit reversal over log2 N - 1 bits), cos and sin of 2 pi k / N for
// k in [0, N/2], the per-stage (cos, sin) of 2 pi k / s for s = 4 .. N and
// k in [1, s/4), and the forward terminal's six-double records
// {cos 2i, sin 2i, cos i, cos (N/4 - i), sin i, sin (N/4 - i)} (in
// 2 pi / N) for i in [1, N/8). Each angle in half-turns is 2k / N or
// 2k / s, exact. BFFT's raw-basis twiddle table (2 cos) feeds only its
// scalar walk and is not built. The work buffer is DIF's (both kernels
// need N reals, and a plan runs one transform at a time).
[[nodiscard]] bool build_dit(PlanState& state, const i32 n, const Precision precision) noexcept {
    DitTables& tables = state.dit;
    const i32 half = n >> 1;
    const i32 logn = std::countr_zero(static_cast<u32>(n));
    tables.n = n;
    tables.half = half;
    tables.radix2_terminal = (logn & 1) != 0;

    i32 stages = 0;
    i32 twiddle_count = 0;
    for (i32 s = 4; s <= n; s <<= 1) {
        stages = stages + 1;
        const i32 q = s >> 2;
        if (q > 1) twiddle_count = twiddle_count + (q - 1);
    }
    tables.twiddle_stage_count = stages;
    const i32 alloc_count = twiddle_count == 0 ? 1 : twiddle_count;
    const i32 tm = n >> 3;
    const i32 q4 = n >> 2;
    const i32 fterm_count = tm > 1 ? 6 * (tm - 1) : 1;

    i32* const rev = static_cast<i32*>(take_block(state, static_cast<usize>(half), sizeof(i32)));
    i32* const offsets = static_cast<i32*>(take_block(state, static_cast<usize>(stages), sizeof(i32)));
    // [ cos : half + 1 | sin : half + 1 | cs : 2 alloc_count | fterm : fterm_count ]
    const usize entries = static_cast<usize>(half) + 1;
    const usize trig_count = 2 * entries + 2 * static_cast<usize>(alloc_count) + static_cast<usize>(fterm_count);
    double* const trig = static_cast<double*>(take_block(state, trig_count, sizeof(double)));
    if (rev == nullptr || offsets == nullptr || trig == nullptr) return false;
    double* const cos_table = trig;
    double* const sin_table = trig + entries;
    double* const cs = trig + 2 * entries;
    double* const fterm = cs + 2 * alloc_count;

    for (i32 b = 0; b < half; ++b) rev[b] = bitrev(b, logn - 1);
    for (i32 k = 0; k <= half; ++k) {
        math::sincospi(static_cast<double>(2 * k) / static_cast<double>(n), sin_table[k], cos_table[k]);
    }
    i32 offset = 0;
    i32 stage = 0;
    for (i32 s = 4; s <= n; s <<= 1) {
        offsets[stage] = offset;
        const i32 q = s >> 2;
        for (i32 k = 1; k < q; ++k) {
            math::sincospi(static_cast<double>(2 * k) / static_cast<double>(s), cs[2 * offset + 1], cs[2 * offset]);
            offset = offset + 1;
        }
        stage = stage + 1;
    }
    for (i32 i = 1; i < tm; ++i) {
        double* const record = fterm + 6 * (i - 1);
        record[0] = cos_table[2 * i];
        record[1] = sin_table[2 * i];
        record[2] = cos_table[i];
        record[3] = cos_table[q4 - i];
        record[4] = sin_table[i];
        record[5] = sin_table[q4 - i];
    }
    tables.rev = rev;
    tables.twiddle_offset = offsets;
    tables.cos = cos_table;
    tables.sin = sin_table;
    tables.cs = cs;
    tables.fterm_cs = fterm;
    if (precision == Precision::float64) {
        tables.work64 = state.dif.work64;
    } else {
        tables.work32 = state.dif.work32;
    }
    return true;
}

// DIT runs from N = 16; a DIT choice below that runs DIF.
[[nodiscard]] Engines effective_engines(const Engines engines, const Mode mode, const usize size) noexcept {
    Engines result{};
    if (mode != Mode::plain || size < 16) return result;
    result = engines;
    return result;
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

[[nodiscard]] usize minimum_size(const Mode mode) noexcept {
    if (mode == Mode::shifted) return 2;
    return 4;
}

[[nodiscard]] bool known_mode(const Mode mode) noexcept {
    return mode == Mode::plain || mode == Mode::shifted || mode == Mode::dip;
}

// A new plan state at `level`, its tables built; null with `failure` set
// when the arguments are refused or an allocation fails.
[[nodiscard]] PlanState* make_state(const usize size, const Mode mode, const Precision precision,
                                    const simd::Level level, const Engines engines, ErrorRecord& failure) noexcept {
    if (!known_mode(mode)) {
        failure = ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fft.make",
                                    ErrorDetail{.message = "the mode is not plain, shifted or dip"});
        return nullptr;
    }
    if (size > max_size) {
        failure = ErrorRecord::make(ErrorKind::limit_exceeded, "stxlib.fft.make",
                                    ErrorDetail{.message = "the size is above fft::max_size (2^30)"});
        return nullptr;
    }
    if (!std::has_single_bit(size) || size < minimum_size(mode)) {
        failure = ErrorRecord::make(ErrorKind::invalid_argument, "stxlib.fft.make",
                                    ErrorDetail{.message = "the size is not a power of two at least the mode's minimum"});
        return nullptr;
    }
    if (precision == Precision::float32 && mode == Mode::dip) {
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
    plan.engines = effective_engines(engines, mode, size);
    const i32 n = static_cast<i32>(size);
    bool built = false;
    if (mode == Mode::plain) {
        built = build_dif(plan, n, precision);
        const bool dit = plan.engines.forward == Engine::dit || plan.engines.inverse == Engine::dit;
        if (built && dit) built = build_dit(plan, n, precision);
    }
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

// The checks every transform makes before it writes: a plan, the counts,
// no overlap.
[[nodiscard]] Refusal transform_refusal(const PlanState* const state, const usize input_count,
                                        const usize input_element, const void* const input,
                                        const usize output_count, const usize output_element,
                                        const void* const output, const bool forward) noexcept {
    if (state == nullptr) return Refusal::empty_plan;
    const usize samples = (*state).size;
    const usize bins = bins_of(state);
    const usize expected_input = forward ? samples : bins;
    const usize expected_output = forward ? bins : samples;
    if (input_count != expected_input) return Refusal::input_count;
    if (output_count != expected_output) return Refusal::output_count;
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

// The engine policy (owner, 2026-11-22: "so each size can use whichever is
// faster"). DIF in both directions everywhere until stxlib_fft_bench has
// measured the tiers against each other on the machines that matter; the
// benchmark prints the per-size winners as the table to put here.
Engines engine_policy(const simd::Level level, const usize size) noexcept {
    (void)level;
    (void)size;
    const Engines engines{};
    return engines;
}

OperationResult<Plan64> PlanAccess::make64(const usize size, const Mode mode, const simd::Level level) noexcept {
    return make64(size, mode, level, engine_policy(level, size));
}

OperationResult<Plan32> PlanAccess::make32(const usize size, const Mode mode, const simd::Level level) noexcept {
    return make32(size, mode, level, engine_policy(level, size));
}

OperationResult<Plan64> PlanAccess::make64(const usize size, const Mode mode, const simd::Level level,
                                           const Engines engines) noexcept {
    ErrorRecord failure = ErrorRecord::make(ErrorKind::internal_failure, "stxlib.fft.make");
    PlanState* const state = make_state(size, mode, Precision::float64, level, engines, failure);
    if (state == nullptr) return OperationResult<Plan64>::failure(failure);
    Plan64 plan;
    plan.state_ = state;
    return OperationResult<Plan64>::success(static_cast<Plan64&&>(plan));
}

OperationResult<Plan32> PlanAccess::make32(const usize size, const Mode mode, const simd::Level level,
                                           const Engines engines) noexcept {
    ErrorRecord failure = ErrorRecord::make(ErrorKind::internal_failure, "stxlib.fft.make");
    PlanState* const state = make_state(size, mode, Precision::float32, level, engines, failure);
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

Engines PlanAccess::engines(const Plan64& plan) noexcept {
    if (plan.state_ == nullptr) return Engines{};
    return (*plan.state_).engines;
}

Engines PlanAccess::engines(const Plan32& plan) noexcept {
    if (plan.state_ == nullptr) return Engines{};
    return (*plan.state_).engines;
}

void PlanAccess::use_kernels(Plan64& plan, const Kernels& kernels) noexcept {
    if (plan.state_ == nullptr) return;
    (*plan.state_).kernels = &kernels;
}

void PlanAccess::use_kernels(Plan32& plan, const Kernels& kernels) noexcept {
    if (plan.state_ == nullptr) return;
    (*plan.state_).kernels = &kernels;
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
                                                          output.count(), sizeof(Complex64), output.data(), true);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const double* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) {
        if (plan.engines.forward == detail::Engine::dit) {
            kernels.dit_forward64(plan.dit, source, output.data());
        } else {
            kernels.dif_forward64(plan.dif, source, output.data());
        }
    }
    if (plan.mode == Mode::shifted) kernels.bodft_forward64(plan.bodft, source, output.data());
    if (plan.mode == Mode::dip) kernels.dip_forward64(plan.dip, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

OperationStatus Plan64::irfft(const View<Complex64> input, const MutableView<double> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(Complex64), input.data(),
                                                          output.count(), sizeof(double), output.data(), false);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const Complex64* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) {
        if (plan.engines.inverse == detail::Engine::dit) {
            kernels.dit_inverse64(plan.dit, source, output.data());
        } else {
            kernels.dif_inverse64(plan.dif, source, output.data());
        }
    }
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
                                                          output.count(), sizeof(Complex32), output.data(), true);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const float* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) {
        if (plan.engines.forward == detail::Engine::dit) {
            kernels.dit_forward32(plan.dit, source, output.data());
        } else {
            kernels.dif_forward32(plan.dif, source, output.data());
        }
    }
    if (plan.mode == Mode::shifted) kernels.bodft_forward32(plan.bodft, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

OperationStatus Plan32::irfft(const View<Complex32> input, const MutableView<float> output) noexcept {
    const detail::Refusal refusal = detail::transform_refusal(state_, input.count(), sizeof(Complex32), input.data(),
                                                          output.count(), sizeof(float), output.data(), false);
    if (refusal != detail::Refusal::none) return OperationStatus::failure(detail::refused(refusal));
    const detail::PlanState& plan = *state_;
    const detail::Kernels& kernels = *plan.kernels;
    const Complex32* const source = detail::flushed_inputs(plan, input.data(), input.count());
    if (plan.mode == Mode::plain) {
        if (plan.engines.inverse == detail::Engine::dit) {
            kernels.dit_inverse32(plan.dit, source, output.data());
        } else {
            kernels.dif_inverse32(plan.dif, source, output.data());
        }
    }
    if (plan.mode == Mode::shifted) kernels.bodft_inverse32(plan.bodft, source, output.data());
    detail::flush_outputs(output.data(), output.count());
    return OperationStatus::success();
}

} // namespace stxlib::fft
