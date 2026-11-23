#pragma once

// stx_fft: real fast Fourier transforms of power-of-two size, copied from
// BFFT (falseywinchnet/bfft at edf5f01; owner, 2026-11-22, projects
// tracks/stxlib/stx_fft.md; provenance in src/algorithms/fft/NOTICE.md).
//
// A plan is made once per size, mode and precision, owns its twiddle tables
// and workspace, and transforms any number of signals of that size:
//
//   OperationResult<fft::Plan64> made = fft::Plan64::make(4096, fft::Mode::plain);
//   fft::Plan64 plan = static_cast<OperationResult<fft::Plan64>&&>(made).take_value();
//   OperationStatus done = plan.rfft(samples.view(), spectrum.mutable_view());
//
// Three modes, each its own kernel:
//
//   plain    BFFT's DIF kernel (Bruun's real factorisation, decimation in
//            frequency) or, from N = 16, its DIT kernel (the same
//            factorisation, decimation in time); the plan picks per size
//            and tier, each within the same bounds. N >= 4. N/2 + 1 bins
//              X[k] = sum_n x[n] e^(-2 pi i k n / N),  k = 0 .. N/2,
//            X[0] and X[N/2] real.
//   shifted  BFFT's BODFT (odd-frequency DFT): the spectrum sampled halfway
//            between the ordinary bins, produced directly by paired radix-4
//            butterflies (no modulation of the input). N >= 2. N/2 bins
//              H[k] = sum_n x[n] e^(-2 pi i (k + 1/2) n / N),  k = 0 .. N/2 - 1;
//            the other half is H[N-1-k] = conj H[k].
//   dip      BFFT's DIP kernel (diagonal in packets): the same bins as plain,
//            computed by an in-place walk of the Bruun diagonal tree whose
//            subtree (d, E) holds exactly the bins j E + d and j E - d, so a
//            pair of harmonic combs of spacing E is one subtree. It is
//            exposed as a transform only; nothing selects combs yet.
//            float64 only (BFFT has no float32 DIP): Plan32 refuses it.
//
// Scale: no transform divides by N. irfft undoes rfft exactly in exact
// arithmetic, for every mode: irfft(rfft(x)) = x, so irfft of a spectrum S
// is (1/N) times the plain inverse sum (for plain and dip, each interior bin
// counted with its conjugate). irfft ignores the imaginary parts of plain's
// and dip's X[0] and X[N/2].
//
// Floating point (docs/CODE_STYLE_APPENDIX.md, A1): no fused multiply-add,
// no contraction; every SIMD tier (stxlib/simd.hpp) computes the scalar
// tier's value for every output (a zero's sign may differ). Accuracy is
// measured by tests/fft_tests.cpp against a long-double DFT and stated in
// proofs/REPORT.md. Subnormals (A1 rule 4): a subnormal input is read as
// zero of its sign (the caller's input is not changed), and a subnormal
// result is returned as zero of its sign.
//
// A plan is not thread-safe: rfft and irfft use its workspace, so one plan
// serves one thread at a time. Plans of the same size are independent.

#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

namespace stxlib::fft {

enum class Mode : u8 {
    plain = 0,
    shifted = 1,
    dip = 2,
};

// A complex value as the transforms read and write it: the real part, then
// the imaginary part, with no padding.
struct Complex64 final {
    double re{0.0};
    double im{0.0};
};
struct Complex32 final {
    float re{0.0F};
    float im{0.0F};
};

// The largest size a plan takes: 2^30 (BFFT's kernels index with int).
inline constexpr usize max_size = usize{1} << 30;

namespace detail {
struct PlanState;
struct PlanAccess;
} // namespace detail

// A float64 plan. The default plan is the empty plan: it owns nothing, and
// rfft and irfft refuse it.
class Plan64 final {
public:
    constexpr Plan64() noexcept = default;

    // A plan for `size` real samples. Fails with invalid_argument when size
    // is not a power of two or is below the mode's minimum (4 for plain and
    // dip, 2 for shifted), limit_exceeded above max_size, out_of_memory when
    // the allocator refuses. The plan's SIMD tier is simd::selected_level().
    [[nodiscard]] static OperationResult<Plan64> make(usize size, Mode mode) noexcept;

    Plan64(Plan64&& other) noexcept;
    // Takes other's state, then releases this plan's old state.
    Plan64& operator=(Plan64&& other) noexcept;
    Plan64(const Plan64&) = delete;
    Plan64& operator=(const Plan64&) = delete;
    ~Plan64();

    [[nodiscard]] bool empty() const noexcept;
    // 0 for the empty plan.
    [[nodiscard]] usize size() const noexcept;
    // Complex values of a spectrum: N/2 + 1 (plain, dip), N/2 (shifted);
    // 0 for the empty plan.
    [[nodiscard]] usize bins() const noexcept;
    // plain for the empty plan.
    [[nodiscard]] Mode mode() const noexcept;

    // Forward: size() samples to bins() values. Fails with invalid_argument,
    // writing nothing, for the empty plan, a count other than size() or
    // bins(), or input and output that overlap.
    [[nodiscard]] OperationStatus rfft(View<double> input, MutableView<Complex64> output) noexcept;
    // Inverse: bins() values to size() samples, with the same failures.
    [[nodiscard]] OperationStatus irfft(View<Complex64> input, MutableView<double> output) noexcept;

private:
    friend struct detail::PlanAccess;
    void release() noexcept;

    detail::PlanState* state_{nullptr};
};

// A float32 plan: as Plan64, with float samples and Complex32 bins. make
// fails with unsupported for Mode::dip.
class Plan32 final {
public:
    constexpr Plan32() noexcept = default;

    [[nodiscard]] static OperationResult<Plan32> make(usize size, Mode mode) noexcept;

    Plan32(Plan32&& other) noexcept;
    Plan32& operator=(Plan32&& other) noexcept;
    Plan32(const Plan32&) = delete;
    Plan32& operator=(const Plan32&) = delete;
    ~Plan32();

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] usize size() const noexcept;
    [[nodiscard]] usize bins() const noexcept;
    [[nodiscard]] Mode mode() const noexcept;

    [[nodiscard]] OperationStatus rfft(View<float> input, MutableView<Complex32> output) noexcept;
    [[nodiscard]] OperationStatus irfft(View<Complex32> input, MutableView<float> output) noexcept;

private:
    friend struct detail::PlanAccess;
    void release() noexcept;

    detail::PlanState* state_{nullptr};
};

} // namespace stxlib::fft
