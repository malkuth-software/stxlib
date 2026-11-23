#pragma once

// stx_fft's private plan state: the tables each kernel reads, the kernel
// table of one SIMD tier, and the plan record behind Plan64 and Plan32
// (include/stxlib/fft.hpp). plan.cpp builds the tables (scalar code, with
// STXlib's sincospi and cospi); the tier files (kernels_*.cpp, through
// variant.inc) only compute.
//
// Every table is a plain pointer into a block the plan owns (PlanState::
// blocks), so a table record is trivially copyable and the kernels take it
// by reference. Work buffers are writable through a const record: the
// record is the plan's description, the buffer's contents are the
// transform's scratch.

#include "stxlib/fft.hpp"
#include "stxlib/result.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/types.hpp"

namespace stxlib::fft::detail {

// DIF's packed per-leaf twiddles for the three deepest levels of a 16-real
// leaf block (BFFT's LeafTw, with the native output slots of its eight
// leaves, and LeafTwF).
struct LeafTwiddles64 final {
    double c4[4];
    double c2[2];
    double c1;
    double s1;
    i32 idx[8]; // native slot of leaf 8m + j (DifTables::outidx)
};
struct LeafTwiddles32 final {
    float c4[4];
    float c2d[4]; // [c2[0], c2[0], c2[1], c2[1]]
    float c1;
    float s1;
};

// One operation of DIF's precomputed walk schedules (BFFT's FwdOp): the
// operation on the block at `base` (reals from the work buffer's start),
// with the block's quarter `q` and Bruun node `m` where it has them.
enum class DifOpKind : u16 {
    norm2 = 1,         // two levels of one segment (q >= 16)
    codelet_q8 = 2,    // one level of a 32-real segment, then two leaf codelets
    codelet_d3 = 3,    // one 16-real leaf codelet
    binomial = 4,      // the spine's binomial split of half-width q
    spine_d3 = 5,      // the spine's last leaf codelet (node 1)
    spine_d2 = 6,      // the spine's depth-2 pack (fused scatter only)
    spine_d1 = 7,      // the spine's depth-1 step
    spine_leaf = 8,    // the spine's last leaf (fused scatter only)
    dc_nyquist = 9,    // X[0] and X[N/2] (fused scatter only)
    spine_norm2 = 10,  // the spine's depth-2 residue step (residue walk only)
};

struct DifOp final {
    u32 base{0};
    u32 q{0};
    u32 m{0};
    DifOpKind kind{DifOpKind::binomial};
};
static_assert(sizeof(DifOp) == 16, "BFFT keeps FwdOp at 16 bytes for the schedule stream");

// DIF's tables. Native order (BFFT's BRUUN_HEAPOPT_SPECTRUM_ORDER, on in
// every BFFT build): the fused-scatter forward writes leaf m's bin to slot
// outidx[m] of the native spectrum, an order in which every Bruun factor
// node's leaves stay contiguous, and standard_native[k] names the slot of
// bin k for the pass to bin order. Below N = 32 the forward writes bin order
// directly and these are not built.
struct DifTables final {
    i32 n{0};
    i32 log2n{0};
    const i32* idx{nullptr};               // [N/2]: leaf m -> bin IDX[m]
    const i32* kinv{nullptr};              // [N/2]: bin k -> leaf
    const double* c{nullptr};              // [N/2]: cos(pi IDX[m] / N)
    const float* cf{nullptr};              // [N/2]: c rounded to float
    const LeafTwiddles64* tw{nullptr};     // [N/16] when N >= 32
    const LeafTwiddles32* twf{nullptr};    // [N/16] when N >= 32
    const i32* outidx{nullptr};            // [N/2]: leaf -> native slot (Plan64, N >= 32)
    const i32* standard_native{nullptr};   // [N/2]: bin -> native slot (Plan64, N >= 32)
    // The walk schedules (N >= 64): the fused-scatter forward, the residue
    // forward, and the residue inverse (the residue forward reversed, then
    // the first binomial split).
    const DifOp* forward_ops{nullptr};
    u32 forward_op_count{0};
    const DifOp* residue_ops{nullptr};
    u32 residue_op_count{0};
    const DifOp* inverse_ops{nullptr};
    u32 inverse_op_count{0};
    double* work64{nullptr};               // [N] (Plan64)
    Complex64* native64{nullptr};          // [N/2 + 1]: the native spectrum (Plan64, N >= 32)
    float* work32{nullptr};                // [N] (Plan32)
};

// DIT's tables (BFFT bruun_dit_kernel.hpp, reset), N >= 16. One set serves
// both precisions: the float kernels round each double twiddle where they
// use it, as BFFT does.
struct DitTables final {
    i32 n{0};
    i32 half{0};
    i32 twiddle_stage_count{0};
    bool radix2_terminal{false};           // log2 N odd
    const i32* rev{nullptr};               // [N/2]: bit reversal over log2 N - 1 bits
    const i32* twiddle_offset{nullptr};    // [stages]: first entry of stage s = 4 .. N
    const double* cos{nullptr};            // [N/2 + 1]: cos(2 pi k / N)
    const double* sin{nullptr};            // [N/2 + 1]: sin(2 pi k / N)
    const double* cs{nullptr};             // [2 entries]: (cos, sin) of 2 pi k / s, k in [1, s/4)
    const double* fterm_cs{nullptr};       // [6 (N/8 - 1)]: the forward terminal's rotations
    double* work64{nullptr};               // [N] (Plan64; DifTables::work64)
    float* work32{nullptr};                // [N] (Plan32; DifTables::work32)
};

// One BODFT level's three twiddle powers, s/8 values each.
struct BodftLevel64 final {
    const Complex64* t1{nullptr};
    const Complex64* t2{nullptr};
    const Complex64* t3{nullptr};
};
struct BodftLevel32 final {
    const Complex32* t1{nullptr};
    const Complex32* t2{nullptr};
    const Complex32* t3{nullptr};
};

inline constexpr i32 bodft_levels = 32;

struct BodftTables final {
    i32 n{0};
    i32 leafsize{0};   // 4 when log2 N is even, else 2
    i32 leafbins{0};   // leafsize / 2
    i32 numleaves{0};  // N / leafsize
    i32 passes{0};     // radix-4 combine passes
    Complex64 root4{};          // e^(-i pi/4)
    Complex32 root4f{};
    const i32* perm{nullptr};   // [N]: radix-4 digit reversal
    BodftLevel64 level64[bodft_levels]{};  // indexed by log2 s (Plan64)
    BodftLevel32 level32[bodft_levels]{};  // (Plan32)
    Complex64* scratch64{nullptr};  // [N]: two ping-pong halves of N/2
    Complex32* scratch32{nullptr};
};

// DIP's width of the egress permutation tables (BFFT's kEgressW).
inline constexpr i32 dip_egress_width = 4096;

struct DipTables final {
    i32 n{0};
    i32 half{0};
    const double* cos{nullptr};   // [N/2 + 1]: cos(2 pi k / N), quarter-wave symmetric
    const double* sin{nullptr};   // [N/2 + 1]
    const double* cs{nullptr};    // [N + 2]: interleaved [cos, sin]
    const i32* tperm{nullptr};    // [2 * dip_egress_width - 1]: T_w at offset w - 1
    double* work{nullptr};        // [N]
};

// One SIMD tier's kernels (kernels_scalar.cpp, kernels_v128.cpp,
// kernels_avx2_fma.cpp). Each reads its tables, the input and its work
// buffer, and writes only the work buffer and the output; requirements
// (checked by plan.cpp before the call): counts match the tables' size,
// input and output do not overlap.
struct Kernels final {
    void (*dif_forward64)(const DifTables&, const double*, Complex64*) noexcept = nullptr;
    void (*dif_inverse64)(const DifTables&, const Complex64*, double*) noexcept = nullptr;
    void (*dif_forward32)(const DifTables&, const float*, Complex32*) noexcept = nullptr;
    void (*dif_inverse32)(const DifTables&, const Complex32*, float*) noexcept = nullptr;
    void (*bodft_forward64)(const BodftTables&, const double*, Complex64*) noexcept = nullptr;
    void (*bodft_inverse64)(const BodftTables&, const Complex64*, double*) noexcept = nullptr;
    void (*bodft_forward32)(const BodftTables&, const float*, Complex32*) noexcept = nullptr;
    void (*bodft_inverse32)(const BodftTables&, const Complex32*, float*) noexcept = nullptr;
    void (*dip_forward64)(const DipTables&, const double*, Complex64*) noexcept = nullptr;
    void (*dip_inverse64)(const DipTables&, const Complex64*, double*) noexcept = nullptr;
    void (*dit_forward64)(const DitTables&, const double*, Complex64*) noexcept = nullptr;
    void (*dit_inverse64)(const DitTables&, const Complex64*, double*) noexcept = nullptr;
    void (*dit_forward32)(const DitTables&, const float*, Complex32*) noexcept = nullptr;
    void (*dit_inverse32)(const DitTables&, const Complex32*, float*) noexcept = nullptr;
};

// The tiers this build compiles: scalar everywhere; vector128 on x86-64
// (SSE2) and arm64 (NEON); avx2_fma on x86-64. wasm's SIMD128 has no
// kernels here and runs the scalar tier.
#if defined(__x86_64__) || defined(_M_X64)
#define STXLIB_FFT_TIER_V128 1
#define STXLIB_FFT_TIER_AVX2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#define STXLIB_FFT_TIER_V128 1
#endif

namespace scalar {
extern const Kernels kernels;
} // namespace scalar
#if defined(STXLIB_FFT_TIER_V128)
namespace v128 {
extern const Kernels kernels;
} // namespace v128
#endif
#if defined(STXLIB_FFT_TIER_AVX2)
namespace avx2_fma {
extern const Kernels kernels;
} // namespace avx2_fma
#endif

// The kernels of `level`, or of the highest tier below it that this build
// has. The caller makes sure the machine runs `level` (simd::hardware_level).
[[nodiscard]] const Kernels& kernels_for(simd::Level level) noexcept;

// The most blocks one plan owns (tables and workspace).
inline constexpr usize plan_block_capacity = 32;

// The plain mode's two kernels, BFFT's DIF and DIT. Both compute the same
// bins (each within its own rounding), so a plan may run either in either
// direction; BFFT picks them at build time, a plan picks them per size when
// it is made (plan.cpp, the engine policy). DIT runs from N = 16; below
// that a DIT choice runs DIF, as BFFT's library does.
enum class Engine : u8 {
    dif = 0,
    dit = 1,
};

struct Engines final {
    Engine forward{Engine::dif};
    Engine inverse{Engine::dif};
};

struct PlanState final {
    usize size{0};
    Mode mode{Mode::plain};
    simd::Level level{simd::Level::scalar};
    const Kernels* kernels{nullptr};
    // The plain mode's engines (dif in the other modes).
    Engines engines{};
    DifTables dif{};
    DitTables dit{};
    BodftTables bodft{};
    DipTables dip{};
    // size + 2 reals of the plan's precision (one of the blocks below): the
    // flushed copy of a transform's input when that input holds a subnormal
    // (rfft reads size reals, irfft bins complex values, at most size + 2).
    void* staging{nullptr};
    // Every block allocated for this plan (memory::allocate), released by
    // destroy_plan_state; unused entries are null.
    void* blocks[plan_block_capacity]{};
    usize block_count{0};
};

// For tests and the benchmark: a plan at a given tier. Requirement: the
// machine runs `level` (a level above simd::hardware_level() would run
// instructions the CPU lacks). Fails as make does.
struct PlanAccess final {
    // The engines of the plain mode come from the engine policy.
    [[nodiscard]] static OperationResult<Plan64> make64(usize size, Mode mode, simd::Level level) noexcept;
    [[nodiscard]] static OperationResult<Plan32> make32(usize size, Mode mode, simd::Level level) noexcept;
    // The plain mode with chosen engines (other modes ignore them).
    [[nodiscard]] static OperationResult<Plan64> make64(usize size, Mode mode, simd::Level level,
                                                        Engines engines) noexcept;
    [[nodiscard]] static OperationResult<Plan32> make32(usize size, Mode mode, simd::Level level,
                                                        Engines engines) noexcept;
    // The plan's tier; scalar for the empty plan.
    [[nodiscard]] static simd::Level level(const Plan64& plan) noexcept;
    [[nodiscard]] static simd::Level level(const Plan32& plan) noexcept;
    // The engines the plan runs (DIT below N = 16 reads as dif); dif, dif
    // for the empty plan.
    [[nodiscard]] static Engines engines(const Plan64& plan) noexcept;
    [[nodiscard]] static Engines engines(const Plan32& plan) noexcept;
    // For the benchmark only: run `kernels` (a measuring variant compiled
    // into the benchmark, tests/fft_fused_*.cpp) in place of the plan's
    // tier. Requirement: the machine runs the variant's instructions. The
    // empty plan is left as it is.
    static void use_kernels(Plan64& plan, const Kernels& kernels) noexcept;
    static void use_kernels(Plan32& plan, const Kernels& kernels) noexcept;
};

// The engines the policy gives a plain plan of `size` at `level` (plan.cpp).
[[nodiscard]] Engines engine_policy(simd::Level level, usize size) noexcept;

} // namespace stxlib::fft::detail
