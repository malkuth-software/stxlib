// A benchmark of stx_fft (include/stxlib/fft.hpp) beside BFFT's
// (BFFT examples/benchmark.cpp and examples/bodft_benchmark.cpp time the
// same transforms). Not a test: built, never run by ctest; the integrator
// runs it on the M4.
//
//   stxlib_fft_bench [repetitions]
//
// For each mode, precision and size 2^4 .. 2^20 it times rfft and irfft at
// every SIMD tier this machine runs (through the private PlanAccess, so one
// run covers the tiers), taking the best of the repetitions (default 5) of
// a batch sized to about 20 ms, and prints nanoseconds per transform and
// per N log2 N. Built with STXLIB_BFFT_ORACLE_DIR, it also times BFFT's own
// kernels (compiled at this build's baseline tier, as BFFT selects at
// compile time) on the same inputs, in the same table, for the forward.
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
#endif

#include <chrono>
#include <cstdio>
#include <vector>

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
using stxlib::fft::detail::PlanAccess;
using stxlib::simd::Level;

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
    bench_mode(Mode::plain, repetitions);
    bench_mode(Mode::shifted, repetitions);
    bench_mode(Mode::dip, repetitions);
    return 0;
}
