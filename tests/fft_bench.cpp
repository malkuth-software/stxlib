// A benchmark of stx_fft (include/stxlib/fft.hpp) beside BFFT's
// (BFFT examples/benchmark.cpp and examples/bodft_benchmark.cpp time the
// same transforms). Not a test: built, never run by ctest; the integrator
// runs it on the M4.
//
//   stxlib_fft_bench [repetitions]
//
// For each size 2^4 .. 2^20 it times rfft and irfft at every SIMD tier this
// machine runs (through the private PlanAccess, so one run covers the
// tiers), taking the best of the repetitions (default 5) of a batch sized
// to about 20 ms, and prints nanoseconds per transform and per N log2 N.
//
// The plain mode is timed with each engine (DIF and DIT, plan_state.hpp)
// in both precisions and directions; at the fused measuring variant's tier
// (tests/fft_fused_kernels.cpp: NEON vector128 on arm64, avx2_fma on
// x86-64) it is timed again with the variant swapped in, which prints what
// the no-contraction rule costs there and how far the fused bins move
// (relative L2 distance from the unfused bins, in epsilons). Built with
// STXLIB_BFFT_ORACLE_DIR, it also times BFFT's own DIF and DIT (compiled
// at this build's baseline tier, as BFFT selects at compile time) on the
// same inputs, prints stx_fft's best engine at that tier against BFFT's
// best kernel per size, precision and direction, and ends with the engine
// table for engine_policy (plan.cpp): the faster engine per size and tier.
// The shifted and dip modes are timed once each, beside BFFT's forward.
//
// Timing uses the host's steady clock (std::chrono): a benchmark's output
// is not a promised result (A1, last paragraph).

#include "stxlib/fft.hpp"
#include "stxlib/result.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include "algorithms/fft/plan_state.hpp"

#if defined(STXLIB_FFT_BENCH_BFFT)
#include "detail/bodft_kernel.hpp"
#include "detail/bruun_dif_kernel.hpp"
#include "detail/bruun_dip_kernel.hpp"
#include "detail/bruun_dit_kernel.hpp"
#endif

#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <vector>

// The fused measuring variant (tests/fft_fused_kernels.cpp).
#if defined(__aarch64__) || defined(_M_ARM64)
#define STXLIB_FFT_BENCH_FUSED 1
namespace stxlib::fft::detail::v128_fused {
extern const Kernels kernels;
} // namespace stxlib::fft::detail::v128_fused
#elif defined(__x86_64__) || defined(_M_X64)
#define STXLIB_FFT_BENCH_FUSED 1
namespace stxlib::fft::detail::avx2_fused {
extern const Kernels kernels;
} // namespace stxlib::fft::detail::avx2_fused
#endif

namespace {

using stxlib::MutableView;
using stxlib::OperationResult;
using stxlib::usize;
using stxlib::View;
using stxlib::fft::Complex32;
using stxlib::fft::Complex64;
using stxlib::fft::Mode;
using stxlib::fft::Plan32;
using stxlib::fft::Plan64;
using stxlib::fft::detail::Engine;
using stxlib::fft::detail::Engines;
using stxlib::fft::detail::Kernels;
using stxlib::fft::detail::PlanAccess;
using stxlib::simd::Level;

#if defined(STXLIB_FFT_BENCH_FUSED)
#if defined(__aarch64__) || defined(_M_ARM64)
constexpr Level fused_level = Level::vector128;
[[nodiscard]] const Kernels& fused_kernels() { return stxlib::fft::detail::v128_fused::kernels; }
#else
constexpr Level fused_level = Level::avx2_fma;
[[nodiscard]] const Kernels& fused_kernels() { return stxlib::fft::detail::avx2_fused::kernels; }
#endif
#endif

[[nodiscard]] double seconds_now() {
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> since = now.time_since_epoch();
    return since.count();
}

[[nodiscard]] double log2_of(const usize n) {
    double result = 0.0;
    for (usize v = n; v > 1; v >>= 1) result = result + 1.0;
    return result;
}

// One timed operation: a plan and its buffers, run `count` times.
class Job {
public:
    virtual ~Job() = default;
    virtual void run() = 0;
};

class Forward64 final : public Job {
public:
    Forward64(Plan64& plan, const std::vector<double>& input, std::vector<Complex64>& output)
        : plan_(plan), input_(input), output_(output) {}
    void run() override {
        const stxlib::OperationStatus done = plan_.rfft(View<double>(input_.data(), input_.size()),
                                                        MutableView<Complex64>(output_.data(), output_.size()));
        (void)done;
    }

private:
    Plan64& plan_;
    const std::vector<double>& input_;
    std::vector<Complex64>& output_;
};

class Inverse64 final : public Job {
public:
    Inverse64(Plan64& plan, const std::vector<Complex64>& input, std::vector<double>& output)
        : plan_(plan), input_(input), output_(output) {}
    void run() override {
        const stxlib::OperationStatus done = plan_.irfft(View<Complex64>(input_.data(), input_.size()),
                                                         MutableView<double>(output_.data(), output_.size()));
        (void)done;
    }

private:
    Plan64& plan_;
    const std::vector<Complex64>& input_;
    std::vector<double>& output_;
};

class Forward32 final : public Job {
public:
    Forward32(Plan32& plan, const std::vector<float>& input, std::vector<Complex32>& output)
        : plan_(plan), input_(input), output_(output) {}
    void run() override {
        const stxlib::OperationStatus done = plan_.rfft(View<float>(input_.data(), input_.size()),
                                                        MutableView<Complex32>(output_.data(), output_.size()));
        (void)done;
    }

private:
    Plan32& plan_;
    const std::vector<float>& input_;
    std::vector<Complex32>& output_;
};

class Inverse32 final : public Job {
public:
    Inverse32(Plan32& plan, const std::vector<Complex32>& input, std::vector<float>& output)
        : plan_(plan), input_(input), output_(output) {}
    void run() override {
        const stxlib::OperationStatus done = plan_.irfft(View<Complex32>(input_.data(), input_.size()),
                                                         MutableView<float>(output_.data(), output_.size()));
        (void)done;
    }

private:
    Plan32& plan_;
    const std::vector<Complex32>& input_;
    std::vector<float>& output_;
};

// Best nanoseconds per run over `repetitions` batches of about 20 ms.
[[nodiscard]] double time_job(Job& job, const int repetitions) {
    job.run();
    usize batch = 1;
    for (;;) {
        const double start = seconds_now();
        for (usize index = 0; index < batch; ++index) job.run();
        if (seconds_now() - start >= 0.02 || batch >= (usize{1} << 24)) break;
        batch = batch * 2;
    }
    double best = 1.0e30;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const double start = seconds_now();
        for (usize index = 0; index < batch; ++index) job.run();
        const double each = (seconds_now() - start) / static_cast<double>(batch);
        if (each < best) best = each;
    }
    return best * 1.0e9;
}

#if defined(STXLIB_FFT_BENCH_BFFT)
class BfftPlain64 final : public Job {
public:
    BfftPlain64(const usize n, const std::vector<double>& input)
        : input_(input), output_(n / 2 + 1), work_(n), native_(n / 2 + 1) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
    }
    void run() override { kernel_.forward_standard(input_.data(), output_.data(), work_.data(), native_.data()); }

private:
    bruun::DIF_RFFT_kernel kernel_;
    const std::vector<double>& input_;
    std::vector<bruun::complex_t> output_;
    std::vector<double> work_;
    std::vector<bruun::complex_t> native_;
};

class BfftPlainInverse64 final : public Job {
public:
    BfftPlainInverse64(const usize n, const std::vector<Complex64>& spectrum) : input_(spectrum.size()), output_(n) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
        for (usize k = 0; k < spectrum.size(); ++k) input_[k] = bruun::complex_t{spectrum[k].re, spectrum[k].im};
    }
    void run() override { kernel_.inverse(input_.data(), output_.data()); }

private:
    bruun::DIF_RFFT_kernel kernel_;
    std::vector<bruun::complex_t> input_;
    std::vector<double> output_;
};

class BfftPlain32 final : public Job {
public:
    BfftPlain32(const usize n, const std::vector<float>& input)
        : input_(input), output_(n / 2 + 1), work_(n), native_(n / 2 + 1) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
    }
    void run() override { kernel_.forward_standard_f32(input_.data(), output_.data(), work_.data(), native_.data()); }

private:
    bruun::DIF_RFFT_kernel kernel_;
    const std::vector<float>& input_;
    std::vector<bruun::complex_f32_t> output_;
    std::vector<float> work_;
    std::vector<bruun::complex_f32_t> native_;
};

class BfftPlainInverse32 final : public Job {
public:
    BfftPlainInverse32(const usize n, const std::vector<Complex32>& spectrum) : input_(spectrum.size()), output_(n) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
        for (usize k = 0; k < spectrum.size(); ++k) input_[k] = bruun::complex_f32_t{spectrum[k].re, spectrum[k].im};
    }
    void run() override { kernel_.inverse_f32(input_.data(), output_.data()); }

private:
    bruun::DIF_RFFT_kernel kernel_;
    std::vector<bruun::complex_f32_t> input_;
    std::vector<float> output_;
};

// BFFT's DIT, its live paths (forward_simd / inverse_simd and the f32 forms).
class BfftDit64 final : public Job {
public:
    BfftDit64(const usize n, const std::vector<double>& input, const std::vector<Complex64>& spectrum,
              const bool forward)
        : input_(input), spectrum_(spectrum.size()), output_(n / 2 + 1), back_(n), work_(n), forward_(forward) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
        for (usize k = 0; k < spectrum.size(); ++k) spectrum_[k] = bruun::complex_t{spectrum[k].re, spectrum[k].im};
    }
    void run() override {
        if (forward_) {
            kernel_.forward_simd(input_.data(), output_.data(), work_.data());
        } else {
            kernel_.inverse_simd(spectrum_.data(), back_.data(), work_.data());
        }
    }

private:
    bruun::DIT_RFFT_kernel kernel_;
    const std::vector<double>& input_;
    std::vector<bruun::complex_t> spectrum_;
    std::vector<bruun::complex_t> output_;
    std::vector<double> back_;
    std::vector<double> work_;
    bool forward_;
};

class BfftDit32 final : public Job {
public:
    BfftDit32(const usize n, const std::vector<float>& input, const std::vector<Complex32>& spectrum,
              const bool forward)
        : input_(input), spectrum_(spectrum.size()), output_(n / 2 + 1), back_(n), work_(n), forward_(forward) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
        for (usize k = 0; k < spectrum.size(); ++k) {
            spectrum_[k] = bruun::complex_f32_t{spectrum[k].re, spectrum[k].im};
        }
    }
    void run() override {
        if (forward_) {
            kernel_.forward_simd_f32(input_.data(), output_.data(), work_.data());
        } else {
            kernel_.inverse_simd_f32(spectrum_.data(), back_.data(), work_.data());
        }
    }

private:
    bruun::DIT_RFFT_kernel kernel_;
    const std::vector<float>& input_;
    std::vector<bruun::complex_f32_t> spectrum_;
    std::vector<bruun::complex_f32_t> output_;
    std::vector<float> back_;
    std::vector<float> work_;
    bool forward_;
};

class BfftShifted64 final : public Job {
public:
    BfftShifted64(const usize n, const std::vector<double>& input)
        : kernel_(static_cast<int>(n)), input_(input), output_(n / 2) {}
    void run() override { kernel_.forward(input_.data(), output_.data()); }

private:
    bodft::plan kernel_;
    const std::vector<double>& input_;
    std::vector<bruun::complex_t> output_;
};

class BfftDip64 final : public Job {
public:
    BfftDip64(const usize n, const std::vector<double>& input) : input_(input), output_(n / 2 + 1), work_(n) {
        const bool ready = kernel_.init(static_cast<int>(n));
        (void)ready;
    }
    void run() override { kernel_.forward_standard(input_.data(), output_.data(), work_.data()); }

private:
    bruun::DIP_RFFT_kernel kernel_;
    const std::vector<double>& input_;
    std::vector<bruun::complex_t> output_;
    std::vector<double> work_;
};

[[nodiscard]] double time_bfft64(const Mode mode, const usize n, const std::vector<double>& x, const int repetitions) {
    if (mode == Mode::shifted) {
        BfftShifted64 job(n, x);
        return time_job(job, repetitions);
    }
    if (mode == Mode::dip) {
        BfftDip64 job(n, x);
        return time_job(job, repetitions);
    }
    BfftPlain64 job(n, x);
    return time_job(job, repetitions);
}
#endif

[[nodiscard]] const char* mode_name(const Mode mode) {
    if (mode == Mode::shifted) return "shifted";
    if (mode == Mode::dip) return "dip";
    return "plain";
}

// The four timings of one engine pair at one tier, in nanoseconds; 0 for
// a timing not taken.
struct Times final {
    double forward64{0.0};
    double inverse64{0.0};
    double forward32{0.0};
    double inverse32{0.0};
};

// The inputs of one size: the signal, its float rounding, and the spectra
// the inverses start from.
struct Signal final {
    std::vector<double> x;
    std::vector<float> xf;
    std::vector<Complex64> spectrum;
    std::vector<Complex32> spectrum32;
};

[[nodiscard]] Signal signal_of(const usize n) {
    Signal signal;
    signal.x.resize(n);
    for (usize index = 0; index < n; ++index) {
        signal.x[index] = static_cast<double>((index * 2654435761U) % 1000U) / 500.0 - 1.0;
    }
    signal.xf.resize(n);
    for (usize index = 0; index < n; ++index) signal.xf[index] = static_cast<float>(signal.x[index]);
    return signal;
}

// Relative L2 distance between two spectra, in units of `epsilon`.
template <typename Complex>
[[nodiscard]] double distance(const std::vector<Complex>& a, const std::vector<Complex>& b, const double epsilon) {
    double error = 0.0;
    double norm = 0.0;
    for (usize k = 0; k < a.size() && k < b.size(); ++k) {
        const double dr = static_cast<double>(a[k].re) - static_cast<double>(b[k].re);
        const double di = static_cast<double>(a[k].im) - static_cast<double>(b[k].im);
        error = error + dr * dr + di * di;
        norm = norm + static_cast<double>(b[k].re) * static_cast<double>(b[k].re) +
               static_cast<double>(b[k].im) * static_cast<double>(b[k].im);
    }
    if (norm == 0.0) return 0.0;
    return std::sqrt(error / norm) / epsilon;
}

// Times one plain-mode engine at `level`, with `swap` (the fused variant)
// in place of the tier's kernels when it is not null. The forward bins are
// left in `bins` and `bins32`.
[[nodiscard]] Times time_plain(const usize n, const Level level, const Engine engine, const Kernels* const swap,
                               const Signal& signal, std::vector<Complex64>& bins, std::vector<Complex32>& bins32,
                               const int repetitions) {
    Times times;
    const Engines engines{.forward = engine, .inverse = engine};
    OperationResult<Plan64> made = PlanAccess::make64(n, Mode::plain, level, engines);
    if (made.succeeded()) {
        Plan64 plan = static_cast<OperationResult<Plan64>&&>(made).take_value();
        if (swap != nullptr) PlanAccess::use_kernels(plan, *swap);
        bins.assign(plan.bins(), Complex64{});
        std::vector<double> back(n);
        Forward64 forward(plan, signal.x, bins);
        times.forward64 = time_job(forward, repetitions);
        Inverse64 inverse(plan, signal.spectrum, back);
        times.inverse64 = time_job(inverse, repetitions);
    }
    OperationResult<Plan32> made32 = PlanAccess::make32(n, Mode::plain, level, engines);
    if (made32.succeeded()) {
        Plan32 plan32 = static_cast<OperationResult<Plan32>&&>(made32).take_value();
        if (swap != nullptr) PlanAccess::use_kernels(plan32, *swap);
        bins32.assign(plan32.bins(), Complex32{});
        std::vector<float> back32(n);
        Forward32 forward32(plan32, signal.xf, bins32);
        times.forward32 = time_job(forward32, repetitions);
        Inverse32 inverse32(plan32, signal.spectrum32, back32);
        times.inverse32 = time_job(inverse32, repetitions);
    }
    return times;
}

void print_times(const char* const label, const stxlib::TextView tier, const usize n, const Times& times) {
    const double scale = static_cast<double>(n) * log2_of(n);
    std::printf("plain    %-6s %-10.*s N=%-8zu f64 rfft %11.1f (%6.3f) irfft %11.1f (%6.3f)  "
                "f32 rfft %11.1f (%6.3f) irfft %11.1f (%6.3f)\n",
                label, static_cast<int>(tier.count()), tier.data(), n, times.forward64, times.forward64 / scale,
                times.inverse64, times.inverse64 / scale, times.forward32, times.forward32 / scale, times.inverse32,
                times.inverse32 / scale);
}

[[nodiscard]] const char* engine_name(const bool dit) { return dit ? "dit" : "dif"; }

#if defined(STXLIB_FFT_BENCH_BFFT)
[[nodiscard]] double smaller(const double a, const double b) { return a < b ? a : b; }
#endif

// The per-size winners at each tier, for the engine table at the end.
struct Winners final {
    usize n{0};
    bool valid[3]{false, false, false};
    bool dit[3][4]{};   // [tier][forward64, inverse64, forward32, inverse32]
};

void bench_plain(const int repetitions) {
    const Level hardware = stxlib::simd::hardware_level();
    const Level levels[3] = {Level::scalar, Level::vector128, Level::avx2_fma};
    std::printf("plain mode: ns per transform (ns per N log2 N)\n");
    std::vector<Winners> table;
    for (usize n = 16; n <= (usize{1} << 20); n <<= 1) {
        Signal signal = signal_of(n);
        {
            // The inverses start from the scalar DIF spectra.
            std::vector<Complex64> bins;
            std::vector<Complex32> bins32;
            const Times reference = time_plain(n, Level::scalar, Engine::dif, nullptr, signal, bins, bins32, 1);
            (void)reference;
            signal.spectrum = bins;
            signal.spectrum32 = bins32;
        }
        Winners winners;
        winners.n = n;
        Times best_top{};
        for (int tier = 0; tier < 3; ++tier) {
            const Level level = levels[tier];
            if (static_cast<int>(level) > static_cast<int>(hardware)) continue;
            const stxlib::TextView name = stxlib::simd::level_name(level);
            std::vector<Complex64> dif_bins;
            std::vector<Complex32> dif_bins32;
            std::vector<Complex64> dit_bins;
            std::vector<Complex32> dit_bins32;
            const Times dif = time_plain(n, level, Engine::dif, nullptr, signal, dif_bins, dif_bins32, repetitions);
            const Times dit = time_plain(n, level, Engine::dit, nullptr, signal, dit_bins, dit_bins32, repetitions);
            print_times("dif", name, n, dif);
            print_times("dit", name, n, dit);
            winners.valid[tier] = true;
            winners.dit[tier][0] = dit.forward64 < dif.forward64;
            winners.dit[tier][1] = dit.inverse64 < dif.inverse64;
            winners.dit[tier][2] = dit.forward32 < dif.forward32;
            winners.dit[tier][3] = dit.inverse32 < dif.inverse32;
            best_top.forward64 = winners.dit[tier][0] ? dit.forward64 : dif.forward64;
            best_top.inverse64 = winners.dit[tier][1] ? dit.inverse64 : dif.inverse64;
            best_top.forward32 = winners.dit[tier][2] ? dit.forward32 : dif.forward32;
            best_top.inverse32 = winners.dit[tier][3] ? dit.inverse32 : dif.inverse32;
#if defined(STXLIB_FFT_BENCH_FUSED)
            if (level == fused_level) {
                std::vector<Complex64> fused_bins;
                std::vector<Complex32> fused_bins32;
                const Times dif_fused =
                    time_plain(n, level, Engine::dif, &fused_kernels(), signal, fused_bins, fused_bins32, repetitions);
                print_times("dif+fma", name, n, dif_fused);
                std::printf("  fma: dif f64 %+.1f%% f32 %+.1f%% time, bins move %.3f / %.3f eps\n",
                            100.0 * (dif_fused.forward64 / dif.forward64 - 1.0),
                            100.0 * (dif_fused.forward32 / dif.forward32 - 1.0),
                            distance(fused_bins, dif_bins, DBL_EPSILON),
                            distance(fused_bins32, dif_bins32, static_cast<double>(FLT_EPSILON)));
                const Times dit_fused =
                    time_plain(n, level, Engine::dit, &fused_kernels(), signal, fused_bins, fused_bins32, repetitions);
                print_times("dit+fma", name, n, dit_fused);
                std::printf("  fma: dit f64 %+.1f%% f32 %+.1f%% time, bins move %.3f / %.3f eps\n",
                            100.0 * (dit_fused.forward64 / dit.forward64 - 1.0),
                            100.0 * (dit_fused.forward32 / dit.forward32 - 1.0),
                            distance(fused_bins, dit_bins, DBL_EPSILON),
                            distance(fused_bins32, dit_bins32, static_cast<double>(FLT_EPSILON)));
            }
#endif
        }
        table.push_back(winners);
#if defined(STXLIB_FFT_BENCH_BFFT)
        BfftPlain64 dif_forward(n, signal.x);
        BfftPlainInverse64 dif_inverse(n, signal.spectrum);
        BfftPlain32 dif_forward32(n, signal.xf);
        BfftPlainInverse32 dif_inverse32(n, signal.spectrum32);
        BfftDit64 dit_forward(n, signal.x, signal.spectrum, true);
        BfftDit64 dit_inverse(n, signal.x, signal.spectrum, false);
        BfftDit32 dit_forward32(n, signal.xf, signal.spectrum32, true);
        BfftDit32 dit_inverse32(n, signal.xf, signal.spectrum32, false);
        const Times bfft_dif{.forward64 = time_job(dif_forward, repetitions),
                             .inverse64 = time_job(dif_inverse, repetitions),
                             .forward32 = time_job(dif_forward32, repetitions),
                             .inverse32 = time_job(dif_inverse32, repetitions)};
        const Times bfft_dit{.forward64 = time_job(dit_forward, repetitions),
                             .inverse64 = time_job(dit_inverse, repetitions),
                             .forward32 = time_job(dit_forward32, repetitions),
                             .inverse32 = time_job(dit_inverse32, repetitions)};
        print_times("dif", "bfft", n, bfft_dif);
        print_times("dit", "bfft", n, bfft_dit);
        // stx_fft's best engine at the highest tier timed, over BFFT's best
        // kernel: below 1 is faster than BFFT.
        const double forward64 = best_top.forward64 / smaller(bfft_dif.forward64, bfft_dit.forward64);
        const double inverse64 = best_top.inverse64 / smaller(bfft_dif.inverse64, bfft_dit.inverse64);
        const double forward32 = best_top.forward32 / smaller(bfft_dif.forward32, bfft_dit.forward32);
        const double inverse32 = best_top.inverse32 / smaller(bfft_dif.inverse32, bfft_dit.inverse32);
        std::printf("  stx/bfft N=%-8zu f64 rfft %.3f irfft %.3f  f32 rfft %.3f irfft %.3f\n", n, forward64, inverse64,
                    forward32, inverse32);
#else
        (void)best_top;
#endif
    }

    // The engine table for engine_policy: per tier and size, the faster
    // engine for forward64, inverse64, forward32, inverse32.
    std::printf("engine table (forward64 inverse64 forward32 inverse32):\n");
    for (int tier = 0; tier < 3; ++tier) {
        const stxlib::TextView name = stxlib::simd::level_name(levels[tier]);
        for (const Winners& row : table) {
            if (!row.valid[tier]) continue;
            std::printf("  %-10.*s N=%-8zu %s %s %s %s\n", static_cast<int>(name.count()), name.data(), row.n,
                        engine_name(row.dit[tier][0]), engine_name(row.dit[tier][1]), engine_name(row.dit[tier][2]),
                        engine_name(row.dit[tier][3]));
        }
    }
}

void bench_mode(const Mode mode, const int repetitions) {
    const Level hardware = stxlib::simd::hardware_level();
    const Level levels[3] = {Level::scalar, Level::vector128, Level::avx2_fma};
    for (usize n = 16; n <= (usize{1} << 20); n <<= 1) {
        std::vector<double> x(n);
        for (usize index = 0; index < n; ++index) x[index] = static_cast<double>((index * 2654435761U) % 1000U) / 500.0 - 1.0;
        std::vector<float> xf(n);
        for (usize index = 0; index < n; ++index) xf[index] = static_cast<float>(x[index]);
        const double scale = static_cast<double>(n) * log2_of(n);
        for (const Level level : levels) {
            if (static_cast<int>(level) > static_cast<int>(hardware)) continue;
            const stxlib::TextView name = stxlib::simd::level_name(level);
            OperationResult<Plan64> made = PlanAccess::make64(n, mode, level);
            if (!made.succeeded()) continue;
            Plan64 plan = static_cast<OperationResult<Plan64>&&>(made).take_value();
            std::vector<Complex64> spectrum(plan.bins());
            std::vector<double> back(n);
            Forward64 forward(plan, x, spectrum);
            const double forward_ns = time_job(forward, repetitions);
            Inverse64 inverse(plan, spectrum, back);
            const double inverse_ns = time_job(inverse, repetitions);
            std::printf("%-8s f64 %-13.*s N=%-8zu rfft %12.1f ns (%6.3f ns/NlogN)  irfft %12.1f ns (%6.3f)\n",
                        mode_name(mode), static_cast<int>(name.count()), name.data(), n, forward_ns,
                        forward_ns / scale, inverse_ns, inverse_ns / scale);
            if (mode == Mode::dip) continue;
            OperationResult<Plan32> made32 = PlanAccess::make32(n, mode, level);
            if (!made32.succeeded()) continue;
            Plan32 plan32 = static_cast<OperationResult<Plan32>&&>(made32).take_value();
            std::vector<Complex32> spectrum32(plan32.bins());
            std::vector<float> back32(n);
            Forward32 forward32(plan32, xf, spectrum32);
            const double forward32_ns = time_job(forward32, repetitions);
            Inverse32 inverse32(plan32, spectrum32, back32);
            const double inverse32_ns = time_job(inverse32, repetitions);
            std::printf("%-8s f32 %-13.*s N=%-8zu rfft %12.1f ns (%6.3f ns/NlogN)  irfft %12.1f ns (%6.3f)\n",
                        mode_name(mode), static_cast<int>(name.count()), name.data(), n, forward32_ns,
                        forward32_ns / scale, inverse32_ns, inverse32_ns / scale);
        }
#if defined(STXLIB_FFT_BENCH_BFFT)
        const double bfft_ns = time_bfft64(mode, n, x, repetitions);
        std::printf("%-8s f64 %-13s N=%-8zu rfft %12.1f ns (%6.3f ns/NlogN)\n", mode_name(mode), "bfft",
                    n, bfft_ns, bfft_ns / scale);
#endif
    }
}

// The repetition count: decimal digits, 1 .. 1000; anything else is 5.
[[nodiscard]] int repetitions_of(const char* const text) {
    int value = 0;
    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' || value > 1000) return 5;
        value = value * 10 + (*digit - '0');
    }
    if (value < 1 || value > 1000) return 5;
    return value;
}

} // namespace

int main(const int argument_count, char** const arguments) {
    int repetitions = 5;
    if (argument_count == 2) repetitions = repetitions_of(arguments[1]);
    bench_plain(repetitions);
    bench_mode(Mode::shifted, repetitions);
    bench_mode(Mode::dip, repetitions);
    return 0;
}
