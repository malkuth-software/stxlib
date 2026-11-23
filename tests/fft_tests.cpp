// Tests of stx_fft (include/stxlib/fft.hpp): the three kernels copied from
// BFFT edf5f01 (src/algorithms/fft/NOTICE.md), both precisions, every SIMD
// tier this machine runs.
//
// Properties checked:
//   F1  against a direct O(N^2) DFT for every power-of-two size up to 4096:
//       plain and dip against X[k] = sum x[n] e^(-2 pi i k n/N), shifted
//       against the direct half-bin sum H[k] = sum x[n] e^(-2 pi i (k+1/2) n/N)
//       (the sum itself, never a modulated FFT); the relative L2 error is
//       measured, printed in units of the precision's epsilon, and bounded;
//   F2  single bins of large transforms (2^17, 2^18) against the direct sum;
//   F3  round trip: irfft(rfft(x)) = x within a bound, up to 2^20;
//   F4  Parseval's identity within a bound;
//   F5  exact cases: an impulse, a constant;
//   F6  every tier this machine runs gives the scalar tier's values (==,
//       so the sign of a zero may differ), forward and inverse, for every
//       size from the smallest to 2^18 (every vector path and tail); the
//       plain mode's PSFT only within its exact family (a fused tier is
//       held by tests/fft_walk_tests.cpp's frozen hashes);
//       the plan made by make() runs simd::selected_level();
//   F7  plain and dip agree on large sizes (the same bins by two kernels);
//   F8  refusals: sizes, the dip mode in float32, counts, overlap, the
//       empty and moved-from plan; nothing is written on a refusal;
//   F9  the subnormal rule: a subnormal input is read as zero of its sign
//       (the same values as the input with zeros there, and the caller's
//       input untouched), and a subnormal result is zero of its sign;
//   F10 the plain mode's DIT engine (chosen through PlanAccess): F1, F3
//       and F4 at the selected tier with the same bounds, the round trip
//       across engines (DIT forward with DIF inverse and the reverse), F6
//       across the tiers, a DIT choice below N = 16 running DIF, and make()
//       running the engine policy's engines.
//
// The plain mode's DIF forward takes different paths by tier and size
// (the fused scatter, or the residue walk and the KINV pack from the
// tier's two-phase size: 8192 at avx2_fma, above 2^20 at vector128); F6
// up to 2^18 checks that they give the same values.
//
// The reference (house style section 8, A1: test references may take
// freedoms) sums in double-double (two-sum and two-product through the
// host's fma, exact by IEEE 754) over twiddles computed in long double
// (cosl, sinl of an exact angle index). Where long double has 64 or more
// significand bits (x86-64) the twiddles are good to ~2^-64; where it is
// double (Apple arm64, MSVC) they are good to ~2^-53, which puts a floor of
// about one epsilon under the float64 measurement. The program prints
// which.
//
// Bounds (measured, then stated; proofs/REPORT.md has the run's numbers):
// relative L2 error and round-trip error at most (log2 N + 2) epsilons,
// Parseval's relative defect at most 4 (log2 N + 2) epsilons.
//
// Run once per tier by ctest (STX_SIMD_LEVEL=scalar, vector128, avx2_fma),
// as the math tests are; F6 also compares the tiers inside one process.

#include "stxlib/fft.hpp"
#include "stxlib/result.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/text_view.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include "algorithms/fft/plan_state.hpp"
#include "core/checked.hpp"
#include "test_support.hpp"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using stxlib::MutableView;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::u64;
using stxlib::usize;
using stxlib::View;
using stxlib::fft::Complex32;
using stxlib::fft::Complex64;
using stxlib::fft::Mode;
using stxlib::fft::Plan32;
using stxlib::fft::Plan64;
using stxlib::fft::detail::Engine;
using stxlib::fft::detail::Engines;
using stxlib::fft::detail::PlanAccess;
using stxlib::simd::Level;

constexpr long double pi_long = 3.141592653589793238462643383279502884L;
constexpr double epsilon64 = DBL_EPSILON;
constexpr double epsilon32 = static_cast<double>(FLT_EPSILON);

// ---------------------------------------------------------------------------
// Inputs: a fixed xorshift64* stream, uniform in [-1, 1).
// ---------------------------------------------------------------------------

struct Stream final {
    u64 state{0x9E3779B97F4A7C15ULL};

    [[nodiscard]] STXLIB_MODULAR double next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const u64 mixed = state * 0x2545F4914F6CDD1DULL;
        const double unit = static_cast<double>(mixed >> 11) * 0x1.0p-53;
        return 2.0 * unit - 1.0;
    }
};

// The seed mix and the generator are modulo 2^64.
[[nodiscard]] STXLIB_MODULAR std::vector<double> random_signal(const usize n, const u64 seed) {
    Stream stream;
    stream.state = stream.state ^ (seed * 0xD1B54A32D192ED03ULL);
    std::vector<double> signal(n);
    for (usize index = 0; index < n; ++index) signal[index] = stream.next();
    return signal;
}

[[nodiscard]] std::vector<float> rounded_signal(const std::vector<double>& signal) {
    std::vector<float> result(signal.size());
    for (usize index = 0; index < signal.size(); ++index) result[index] = static_cast<float>(signal[index]);
    return result;
}

// ---------------------------------------------------------------------------
// The reference: double-double accumulation over long-double twiddles.
// ---------------------------------------------------------------------------

struct Wide final {
    double hi{0.0};
    double lo{0.0};
};

[[nodiscard]] Wide wide_of(const long double value) noexcept {
    const double hi = static_cast<double>(value);
    const double lo = static_cast<double>(value - static_cast<long double>(hi));
    const Wide result{.hi = hi, .lo = lo};
    return result;
}

[[nodiscard]] Wide two_sum(const double a, const double b) noexcept {
    const double s = a + b;
    const double bb = s - a;
    const double e = (a - (s - bb)) + (b - bb);
    const Wide result{.hi = s, .lo = e};
    return result;
}

[[nodiscard]] Wide add(const Wide a, const Wide b) noexcept {
    const Wide s = two_sum(a.hi, b.hi);
    const double lo = s.lo + a.lo + b.lo;
    const Wide result = two_sum(s.hi, lo);
    return result;
}

// a (a double) times w (a double-double).
[[nodiscard]] Wide scale(const double a, const Wide w) noexcept {
    const double p = a * w.hi;
    const double e = std::fma(a, w.hi, -p);
    const Wide result = two_sum(p, e + a * w.lo);
    return result;
}

// a * a exactly, as a double-double.
[[nodiscard]] Wide square(const double a) noexcept {
    const double p = a * a;
    const Wide result{.hi = p, .lo = std::fma(a, a, -p)};
    return result;
}

[[nodiscard]] long double wide_value(const Wide w) noexcept {
    return static_cast<long double>(w.hi) + static_cast<long double>(w.lo);
}

struct WideComplex final {
    Wide re{};
    Wide im{};
};

struct ReferenceBin final {
    long double re{0.0L};
    long double im{0.0L};
};

// The twiddles e^(-i pi j / half_turns) for j in [0, 2 half_turns), as
// double-double pairs from long double.
[[nodiscard]] std::vector<WideComplex> twiddles(const usize half_turns) {
    const usize count = 2 * half_turns;
    std::vector<WideComplex> table(count);
    for (usize j = 0; j < count; ++j) {
        const long double angle = pi_long * static_cast<long double>(j) / static_cast<long double>(half_turns);
        table[j].re = wide_of(std::cos(angle));
        table[j].im = wide_of(-std::sin(angle));
    }
    return table;
}

// One bin of the direct sum: sum x[n] e^(-i pi step n / N), step = 2k for
// the ordinary bin k, 2k + 1 for the half bin k + 1/2. table is twiddles(N).
[[nodiscard]] ReferenceBin direct_bin(const std::vector<double>& x, const std::vector<WideComplex>& table,
                                      const u64 step) {
    const u64 period = table.size();
    Wide re{};
    Wide im{};
    u64 phase = 0;
    for (usize n = 0; n < x.size(); ++n) {
        const WideComplex& w = table[phase];
        re = add(re, scale(x[n], w.re));
        im = add(im, scale(x[n], w.im));
        phase = (phase + step) % period;
    }
    const ReferenceBin result{.re = wide_value(re), .im = wide_value(im)};
    return result;
}

[[nodiscard]] u64 step_of(const Mode mode, const usize k) noexcept {
    if (mode == Mode::shifted) return 2 * k + 1;
    return 2 * k;
}

[[nodiscard]] usize bins_of(const Mode mode, const usize n) noexcept {
    if (mode == Mode::shifted) return n / 2;
    return n / 2 + 1;
}

[[nodiscard]] double log2_of(const usize n) noexcept {
    double result = 0.0;
    for (usize v = n; v > 1; v >>= 1) result = result + 1.0;
    return result;
}

[[nodiscard]] const char* mode_name(const Mode mode) noexcept {
    if (mode == Mode::shifted) return "shifted";
    if (mode == Mode::dip) return "dip";
    return "plain";
}

// ---------------------------------------------------------------------------
// Transforms through the public API (abort the check on a refusal).
// ---------------------------------------------------------------------------

[[nodiscard]] Plan64 plan64(const usize n, const Mode mode) {
    OperationResult<Plan64> made = Plan64::make(n, mode);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan64();
    return static_cast<OperationResult<Plan64>&&>(made).take_value();
}

[[nodiscard]] Plan32 plan32(const usize n, const Mode mode) {
    OperationResult<Plan32> made = Plan32::make(n, mode);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan32();
    return static_cast<OperationResult<Plan32>&&>(made).take_value();
}

[[nodiscard]] std::vector<Complex64> forward64(Plan64& plan, const std::vector<double>& x) {
    std::vector<Complex64> spectrum(plan.bins());
    const OperationStatus done = plan.rfft(View<double>(x.data(), x.size()),
                                           MutableView<Complex64>(spectrum.data(), spectrum.size()));
    CHECK(done.succeeded());
    return spectrum;
}

[[nodiscard]] std::vector<double> inverse64(Plan64& plan, const std::vector<Complex64>& spectrum) {
    std::vector<double> x(plan.size());
    const OperationStatus done = plan.irfft(View<Complex64>(spectrum.data(), spectrum.size()),
                                            MutableView<double>(x.data(), x.size()));
    CHECK(done.succeeded());
    return x;
}

[[nodiscard]] std::vector<Complex32> forward32(Plan32& plan, const std::vector<float>& x) {
    std::vector<Complex32> spectrum(plan.bins());
    const OperationStatus done = plan.rfft(View<float>(x.data(), x.size()),
                                           MutableView<Complex32>(spectrum.data(), spectrum.size()));
    CHECK(done.succeeded());
    return spectrum;
}

[[nodiscard]] std::vector<float> inverse32(Plan32& plan, const std::vector<Complex32>& spectrum) {
    std::vector<float> x(plan.size());
    const OperationStatus done = plan.irfft(View<Complex32>(spectrum.data(), spectrum.size()),
                                            MutableView<float>(x.data(), x.size()));
    CHECK(done.succeeded());
    return x;
}

[[nodiscard]] std::vector<ReferenceBin> widened(const std::vector<Complex64>& spectrum) {
    std::vector<ReferenceBin> result(spectrum.size());
    for (usize k = 0; k < spectrum.size(); ++k) {
        result[k].re = static_cast<long double>(spectrum[k].re);
        result[k].im = static_cast<long double>(spectrum[k].im);
    }
    return result;
}

[[nodiscard]] std::vector<ReferenceBin> widened(const std::vector<Complex32>& spectrum) {
    std::vector<ReferenceBin> result(spectrum.size());
    for (usize k = 0; k < spectrum.size(); ++k) {
        result[k].re = static_cast<long double>(spectrum[k].re);
        result[k].im = static_cast<long double>(spectrum[k].im);
    }
    return result;
}

// ||got - want|| / ||want|| over the bins.
[[nodiscard]] double relative_l2(const std::vector<ReferenceBin>& got, const std::vector<ReferenceBin>& want) {
    long double error = 0.0L;
    long double norm = 0.0L;
    for (usize k = 0; k < want.size(); ++k) {
        const long double dr = got[k].re - want[k].re;
        const long double di = got[k].im - want[k].im;
        error = error + dr * dr + di * di;
        norm = norm + want[k].re * want[k].re + want[k].im * want[k].im;
    }
    if (norm == 0.0L) return static_cast<double>(std::sqrt(error));
    return static_cast<double>(std::sqrt(error / norm));
}

// The largest error seen per mode and precision, in epsilons, for the
// summary the integrator copies into proofs/REPORT.md.
struct Worst final {
    double plain64{0.0};
    double shifted64{0.0};
    double dip64{0.0};
    double plain32{0.0};
    double shifted32{0.0};
};

Worst& worst() {
    static Worst record{};
    return record;
}

void note(const Mode mode, const bool single, const double epsilons) {
    Worst& record = worst();
    double* slot = &record.plain64;
    if (mode == Mode::shifted) slot = single ? &record.shifted32 : &record.shifted64;
    if (mode == Mode::dip) slot = &record.dip64;
    if (mode == Mode::plain && single) slot = &record.plain32;
    if (epsilons > *slot) *slot = epsilons;
}

// ---------------------------------------------------------------------------
// F1: against the direct DFT, every size up to 4096.
// ---------------------------------------------------------------------------

void against_direct_sum(const Mode mode, const usize smallest) {
    for (usize n = smallest; n <= 4096; n <<= 1) {
        const std::vector<double> x = random_signal(n, n + 17);
        const std::vector<WideComplex> table = twiddles(n);
        std::vector<ReferenceBin> want(bins_of(mode, n));
        for (usize k = 0; k < want.size(); ++k) want[k] = direct_bin(x, table, step_of(mode, k));
        const double bound = log2_of(n) + 2.0;

        Plan64 wide = plan64(n, mode);
        const double error64 = relative_l2(widened(forward64(wide, x)), want) / epsilon64;
        note(mode, false, error64);
        CHECK(error64 <= bound);
        if (error64 > bound) std::printf("  %s float64 N=%zu: %.3f eps\n", mode_name(mode), n, error64);

        if (mode == Mode::dip) continue;
        // float32 against the reference of the float-rounded input.
        const std::vector<float> xf = rounded_signal(x);
        std::vector<double> xr(n);
        for (usize index = 0; index < n; ++index) xr[index] = static_cast<double>(xf[index]);
        std::vector<ReferenceBin> want32(bins_of(mode, n));
        for (usize k = 0; k < want32.size(); ++k) want32[k] = direct_bin(xr, table, step_of(mode, k));
        Plan32 narrow = plan32(n, mode);
        const double error32 = relative_l2(widened(forward32(narrow, xf)), want32) / epsilon32;
        note(mode, true, error32);
        CHECK(error32 <= bound);
        if (error32 > bound) std::printf("  %s float32 N=%zu: %.3f eps\n", mode_name(mode), n, error32);
    }
}

// ---------------------------------------------------------------------------
// F2: single bins of large transforms.
// ---------------------------------------------------------------------------

void large_bins(const Mode mode) {
    for (usize n = usize{1} << 17; n <= (usize{1} << 18); n <<= 1) {
        const std::vector<double> x = random_signal(n, n + 3);
        const std::vector<WideComplex> table = twiddles(n);
        Plan64 plan = plan64(n, mode);
        const std::vector<Complex64> got = forward64(plan, x);
        const usize bins = bins_of(mode, n);
        // The first, the last, and spread interior bins (DIP's comb pairs
        // land in different subtrees).
        const usize picks[8] = {0, 1, 2, bins / 3, bins / 2, (2 * bins) / 3 + 1, bins - 2, bins - 1};
        long double error = 0.0L;
        long double norm = 0.0L;
        for (const usize k : picks) {
            const ReferenceBin want = direct_bin(x, table, step_of(mode, k));
            const long double dr = static_cast<long double>(got[k].re) - want.re;
            const long double di = static_cast<long double>(got[k].im) - want.im;
            error = error + dr * dr + di * di;
            norm = norm + want.re * want.re + want.im * want.im;
        }
        const double relative = static_cast<double>(std::sqrt(error / norm)) / epsilon64;
        note(mode, false, relative);
        CHECK(relative <= log2_of(n) + 2.0);
    }
}

// ---------------------------------------------------------------------------
// F3, F4: round trip and Parseval.
// ---------------------------------------------------------------------------

// The energies are summed in double-double, not long double: on Apple arm64
// long double is double, and a plain sum of 2^20 squares there drifts by
// far more than the bound being checked. Every bin and sample is a double
// (or a float), so each square is exact as a double-double.
[[nodiscard]] Wide signal_energy_of(const std::vector<double>& x) noexcept {
    Wide energy{};
    for (const double sample : x) energy = add(energy, square(sample));
    return energy;
}

[[nodiscard]] Wide signal_energy_of(const std::vector<float>& x) noexcept {
    Wide energy{};
    for (const float sample : x) energy = add(energy, square(static_cast<double>(sample)));
    return energy;
}

[[nodiscard]] Wide spectrum_energy(const Mode mode, const std::vector<ReferenceBin>& spectrum, const usize n) {
    Wide energy{};
    for (usize k = 0; k < spectrum.size(); ++k) {
        const Wide magnitude =
            add(square(static_cast<double>(spectrum[k].re)), square(static_cast<double>(spectrum[k].im)));
        // plain and dip: X[0] and X[N/2] once, the interior twice (the
        // other half is their conjugates); shifted: every bin twice.
        const bool once = mode != Mode::shifted && (k == 0 || k == n / 2);
        energy = add(energy, once ? magnitude : add(magnitude, magnitude));
    }
    // n is a power of two: the division is exact.
    const double scale_down = 1.0 / static_cast<double>(n);
    const Wide result{.hi = energy.hi * scale_down, .lo = energy.lo * scale_down};
    return result;
}

// |got - want| / want, the difference taken in double-double.
[[nodiscard]] double energy_defect(const Wide got, const Wide want) noexcept {
    const Wide negated{.hi = -want.hi, .lo = -want.lo};
    const Wide difference = add(got, negated);
    return std::fabs(static_cast<double>(wide_value(difference))) / static_cast<double>(wide_value(want));
}

void round_trip_and_parseval(const Mode mode, const usize smallest) {
    for (usize n = smallest; n <= (usize{1} << 20); n <<= 1) {
        const std::vector<double> x = random_signal(n, 3 * n + 1);
        const Wide signal_energy = signal_energy_of(x);
        const double bound = log2_of(n) + 2.0;

        Plan64 wide = plan64(n, mode);
        const std::vector<Complex64> spectrum = forward64(wide, x);
        const std::vector<double> back = inverse64(wide, spectrum);
        double worst_sample = 0.0;
        for (usize index = 0; index < n; ++index) {
            const double error = std::fabs(back[index] - x[index]);
            if (error > worst_sample) worst_sample = error;
        }
        CHECK(worst_sample / epsilon64 <= bound);
        const double defect = energy_defect(spectrum_energy(mode, widened(spectrum), n), signal_energy);
        CHECK(defect / epsilon64 <= 4.0 * bound);

        if (mode == Mode::dip) continue;
        const std::vector<float> xf = rounded_signal(x);
        const Wide signal_energy32 = signal_energy_of(xf);
        Plan32 narrow = plan32(n, mode);
        const std::vector<Complex32> spectrum32 = forward32(narrow, xf);
        const std::vector<float> back32 = inverse32(narrow, spectrum32);
        double worst32 = 0.0;
        for (usize index = 0; index < n; ++index) {
            const double error = std::fabs(static_cast<double>(back32[index]) - static_cast<double>(xf[index]));
            if (error > worst32) worst32 = error;
        }
        CHECK(worst32 / epsilon32 <= bound);
        const double defect32 = energy_defect(spectrum_energy(mode, widened(spectrum32), n), signal_energy32);
        CHECK(defect32 / epsilon32 <= 4.0 * bound);
    }
}

// ---------------------------------------------------------------------------
// F5: exact cases.
// ---------------------------------------------------------------------------

void exact_cases() {
    for (usize n = 4; n <= 1024; n <<= 1) {
        // An impulse at 0: every plain and dip bin is 1, every half bin 1.
        std::vector<double> impulse(n, 0.0);
        impulse[0] = 1.0;
        const Mode modes[3] = {Mode::plain, Mode::shifted, Mode::dip};
        for (const Mode mode : modes) {
            Plan64 plan = plan64(n, mode);
            const std::vector<Complex64> spectrum = forward64(plan, impulse);
            bool ones = true;
            for (const Complex64 bin : spectrum) ones = ones && bin.re == 1.0 && bin.im == 0.0;
            CHECK(ones);
        }
        // A constant: plain and dip put N in X[0] and nothing elsewhere.
        const std::vector<double> constant(n, 1.0);
        const Mode ordinary[2] = {Mode::plain, Mode::dip};
        for (const Mode mode : ordinary) {
            Plan64 plan = plan64(n, mode);
            const std::vector<Complex64> spectrum = forward64(plan, constant);
            CHECK(spectrum[0].re == static_cast<double>(n) && spectrum[0].im == 0.0);
            double rest = 0.0;
            for (usize k = 1; k < spectrum.size(); ++k) rest = rest + std::fabs(spectrum[k].re) + std::fabs(spectrum[k].im);
            CHECK(rest <= static_cast<double>(n) * log2_of(n) * epsilon64);
        }
    }
}

// ---------------------------------------------------------------------------
// F6: every tier gives the scalar tier's values.
// ---------------------------------------------------------------------------

[[nodiscard]] bool same64(const std::vector<Complex64>& a, const std::vector<Complex64>& b) {
    if (a.size() != b.size()) return false;
    for (usize k = 0; k < a.size(); ++k) {
        if (!(a[k].re == b[k].re && a[k].im == b[k].im)) return false;
    }
    return true;
}

[[nodiscard]] bool same32(const std::vector<Complex32>& a, const std::vector<Complex32>& b) {
    if (a.size() != b.size()) return false;
    for (usize k = 0; k < a.size(); ++k) {
        if (!(a[k].re == b[k].re && a[k].im == b[k].im)) return false;
    }
    return true;
}

template <typename Sample>
[[nodiscard]] bool same_samples(const std::vector<Sample>& a, const std::vector<Sample>& b) {
    if (a.size() != b.size()) return false;
    for (usize index = 0; index < a.size(); ++index) {
        if (!(a[index] == b[index])) return false;
    }
    return true;
}

[[nodiscard]] Plan64 plan64_at(const usize n, const Mode mode, const Level level) {
    OperationResult<Plan64> made = PlanAccess::make64(n, mode, level);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan64();
    return static_cast<OperationResult<Plan64>&&>(made).take_value();
}

[[nodiscard]] Plan32 plan32_at(const usize n, const Mode mode, const Level level) {
    OperationResult<Plan32> made = PlanAccess::make32(n, mode, level);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan32();
    return static_cast<OperationResult<Plan32>&&>(made).take_value();
}

void tiers_agree(const Mode mode, const usize smallest) {
    const Level hardware = stxlib::simd::hardware_level();
    const Level levels[2] = {Level::vector128, Level::avx2_fma};
    for (const Level level : levels) {
        if (static_cast<int>(level) > static_cast<int>(hardware)) continue;
        for (usize n = smallest; n <= (usize{1} << 18); n <<= 1) {
            const std::vector<double> x = random_signal(n, 7 * n + 5);
            Plan64 reference = plan64_at(n, mode, Level::scalar);
            Plan64 tier = plan64_at(n, mode, level);
            // The plain mode's PSFT compares within its family: a fused tier
            // has no scalar counterpart (fft_walk_tests' hashes hold it).
            if (mode == Mode::plain && stxlib::fft::detail::walk_fused(PlanAccess::walk_tier(tier)) && n >= 16) continue;
            const std::vector<Complex64> want = forward64(reference, x);
            const std::vector<Complex64> got = forward64(tier, x);
            CHECK(same64(got, want));
            CHECK(same_samples(inverse64(tier, want), inverse64(reference, want)));
            if (mode == Mode::dip) continue;
            const std::vector<float> xf = rounded_signal(x);
            Plan32 reference32 = plan32_at(n, mode, Level::scalar);
            Plan32 tier32 = plan32_at(n, mode, level);
            const std::vector<Complex32> want32 = forward32(reference32, xf);
            CHECK(same32(forward32(tier32, xf), want32));
            CHECK(same_samples(inverse32(tier32, want32), inverse32(reference32, want32)));
        }
    }
}

void make_runs_the_selected_level() {
    Plan64 plan = plan64(64, Mode::plain);
    CHECK(PlanAccess::level(plan) == stxlib::simd::selected_level());
    Plan32 narrow = plan32(64, Mode::shifted);
    CHECK(PlanAccess::level(narrow) == stxlib::simd::selected_level());
}

// ---------------------------------------------------------------------------
// F7: plain and dip agree on large sizes.
// ---------------------------------------------------------------------------

void plain_and_dip_agree() {
    for (usize n = usize{1} << 12; n <= (usize{1} << 20); n <<= 2) {
        const std::vector<double> x = random_signal(n, n + 99);
        Plan64 plain = plan64(n, Mode::plain);
        Plan64 dip = plan64(n, Mode::dip);
        const double difference = relative_l2(widened(forward64(dip, x)), widened(forward64(plain, x))) / epsilon64;
        CHECK(difference <= 2.0 * (log2_of(n) + 2.0));
    }
}

// ---------------------------------------------------------------------------
// F8: refusals.
// ---------------------------------------------------------------------------

[[nodiscard]] bool refused_with(const OperationResult<Plan64>& made, const stxlib::ErrorKind kind) {
    return made.failed() && (*made.error()).kind() == kind;
}

void refusals() {
    CHECK(refused_with(Plan64::make(0, Mode::plain), stxlib::ErrorKind::invalid_argument));
    CHECK(refused_with(Plan64::make(2, Mode::plain), stxlib::ErrorKind::invalid_argument));
    CHECK(refused_with(Plan64::make(2, Mode::dip), stxlib::ErrorKind::invalid_argument));
    CHECK(refused_with(Plan64::make(1, Mode::shifted), stxlib::ErrorKind::invalid_argument));
    CHECK(refused_with(Plan64::make(96, Mode::plain), stxlib::ErrorKind::invalid_argument));
    CHECK(refused_with(Plan64::make(stxlib::fft::max_size * 2, Mode::plain), stxlib::ErrorKind::limit_exceeded));
    CHECK(refused_with(Plan64::make(64, static_cast<Mode>(7)), stxlib::ErrorKind::invalid_argument));
    CHECK(Plan64::make(2, Mode::shifted).succeeded());
    {
        OperationResult<Plan32> made = Plan32::make(64, Mode::dip);
        CHECK(made.failed() && (*made.error()).kind() == stxlib::ErrorKind::unsupported);
    }

    Plan64 plan = plan64(16, Mode::plain);
    CHECK(plan.size() == 16 && plan.bins() == 9 && plan.mode() == Mode::plain && !plan.empty());
    std::vector<double> x(16, 1.0);
    std::vector<Complex64> spectrum(9, Complex64{.re = 5.0, .im = 5.0});
    // Wrong counts write nothing.
    CHECK(plan.rfft(View<double>(x.data(), 15), MutableView<Complex64>(spectrum.data(), 9)).failed());
    CHECK(plan.rfft(View<double>(x.data(), 16), MutableView<Complex64>(spectrum.data(), 8)).failed());
    CHECK(spectrum[0].re == 5.0 && spectrum[8].im == 5.0);
    CHECK(plan.irfft(View<Complex64>(spectrum.data(), 8), MutableView<double>(x.data(), 16)).failed());
    CHECK(x[0] == 1.0);
    // Overlap: the output inside the input's bytes.
    std::vector<double> shared(64, 0.0);
    CHECK(plan.rfft(View<double>(shared.data(), 16),
                    MutableView<Complex64>(reinterpret_cast<Complex64*>(shared.data() + 8), 9)).failed());
    // The empty and the moved-from plan.
    Plan64 empty;
    CHECK(empty.empty() && empty.size() == 0 && empty.bins() == 0);
    CHECK(empty.rfft(View<double>(x.data(), 16), MutableView<Complex64>(spectrum.data(), 9)).failed());
    Plan64 moved(static_cast<Plan64&&>(plan));
    CHECK(plan.empty() && !moved.empty() && moved.size() == 16);
    CHECK(plan.rfft(View<double>(x.data(), 16), MutableView<Complex64>(spectrum.data(), 9)).failed());
    CHECK(moved.rfft(View<double>(x.data(), 16), MutableView<Complex64>(spectrum.data(), 9)).succeeded());
    CHECK(spectrum[0].re == 16.0);
    Plan64 other = plan64(8, Mode::shifted);
    other = static_cast<Plan64&&>(moved);
    CHECK(moved.empty() && other.size() == 16 && other.mode() == Mode::plain);

    Plan32 narrow = plan32(8, Mode::shifted);
    CHECK(narrow.bins() == 4);
    std::vector<float> xf(8, 1.0F);
    std::vector<Complex32> spectrum32(4);
    CHECK(narrow.rfft(View<float>(xf.data(), 8), MutableView<Complex32>(spectrum32.data(), 3)).failed());
    CHECK(narrow.rfft(View<float>(xf.data(), 8), MutableView<Complex32>(spectrum32.data(), 4)).succeeded());
}

// ---------------------------------------------------------------------------
// F9: the subnormal rule.
// ---------------------------------------------------------------------------

// Whether every bin is zero (either sign).
[[nodiscard]] bool all_zero(const std::vector<Complex64>& spectrum) noexcept {
    bool zeros = true;
    for (const Complex64 bin : spectrum) zeros = zeros && bin.re == 0.0 && bin.im == 0.0;
    return zeros;
}

[[nodiscard]] bool all_zero(const std::vector<Complex32>& spectrum) noexcept {
    bool zeros = true;
    for (const Complex32 bin : spectrum) zeros = zeros && bin.re == 0.0F && bin.im == 0.0F;
    return zeros;
}

// Whether two transforms' values are equal (==, so a zero's sign may differ).
[[nodiscard]] bool same(const std::vector<Complex64>& a, const std::vector<Complex64>& b) noexcept {
    bool equal = a.size() == b.size();
    for (usize index = 0; equal && index < a.size(); ++index) {
        equal = a[index].re == b[index].re && a[index].im == b[index].im;
    }
    return equal;
}

[[nodiscard]] bool same(const std::vector<Complex32>& a, const std::vector<Complex32>& b) noexcept {
    bool equal = a.size() == b.size();
    for (usize index = 0; equal && index < a.size(); ++index) {
        equal = a[index].re == b[index].re && a[index].im == b[index].im;
    }
    return equal;
}

template <typename Real>
[[nodiscard]] bool same(const std::vector<Real>& a, const std::vector<Real>& b) noexcept {
    bool equal = a.size() == b.size();
    for (usize index = 0; equal && index < a.size(); ++index) equal = a[index] == b[index];
    return equal;
}

void subnormal_inputs64(const Mode mode) {
    const usize n = 64;
    const double tiny = DBL_MIN / 16.0; // 2^-1026, subnormal
    Plan64 plan = plan64(n, mode);

    // Every sample tiny: bin 0 would be 64 * 2^-1026 = 2^-1020, normal, if
    // the inputs were read as they are. Read as zeros, every bin is zero.
    const std::vector<double> constant(n, tiny);
    CHECK(all_zero(forward64(plan, constant)));

    // A normal signal with subnormals of both signs at some samples gives
    // the values of the same signal with zeros there; the caller's input is
    // not changed.
    std::vector<double> mixed = random_signal(n, 91);
    std::vector<double> zeroed = mixed;
    for (usize index = 0; index < n; index += 5) {
        mixed[index] = (index % 2 == 0) ? tiny : -tiny * 3.0;
        zeroed[index] = 0.0;
    }
    const std::vector<double> kept = mixed;
    const std::vector<Complex64> spectrum = forward64(plan, mixed);
    CHECK(same(spectrum, forward64(plan, zeroed)));
    CHECK(mixed[0] == tiny && mixed[5] == -tiny * 3.0 && same(mixed, kept));

    // The inverse: a spectrum with subnormal parts gives the samples of the
    // spectrum with zeros there, and an all-subnormal spectrum gives zeros.
    std::vector<Complex64> bins = forward64(plan, random_signal(n, 92));
    std::vector<Complex64> bins_zeroed = bins;
    for (usize index = 1; index < bins.size(); index += 3) {
        bins[index].re = tiny;
        bins[index].im = -tiny;
        bins_zeroed[index].re = 0.0;
        bins_zeroed[index].im = 0.0;
    }
    const std::vector<Complex64> bins_kept = bins;
    CHECK(same(inverse64(plan, bins), inverse64(plan, bins_zeroed)));
    CHECK(same(bins, bins_kept));
    const std::vector<Complex64> faint(plan.bins(), Complex64{.re = tiny, .im = tiny});
    const std::vector<double> samples = inverse64(plan, faint);
    bool zeros = true;
    for (const double sample : samples) zeros = zeros && sample == 0.0;
    CHECK(zeros);
}

void subnormal_inputs32(const Mode mode) {
    const usize n = 64;
    const float tiny = FLT_MIN / 16.0F; // 2^-130, subnormal
    Plan32 plan = plan32(n, mode);

    const std::vector<float> constant(n, tiny);
    CHECK(all_zero(forward32(plan, constant)));

    std::vector<float> mixed = rounded_signal(random_signal(n, 93));
    std::vector<float> zeroed = mixed;
    for (usize index = 0; index < n; index += 5) {
        mixed[index] = (index % 2 == 0) ? tiny : -tiny * 3.0F;
        zeroed[index] = 0.0F;
    }
    const std::vector<float> kept = mixed;
    CHECK(same(forward32(plan, mixed), forward32(plan, zeroed)));
    CHECK(same(mixed, kept));

    std::vector<Complex32> bins = forward32(plan, rounded_signal(random_signal(n, 94)));
    std::vector<Complex32> bins_zeroed = bins;
    for (usize index = 1; index < bins.size(); index += 3) {
        bins[index].re = -tiny;
        bins[index].im = tiny;
        bins_zeroed[index].re = 0.0F;
        bins_zeroed[index].im = 0.0F;
    }
    const std::vector<Complex32> bins_kept = bins;
    CHECK(same(inverse32(plan, bins), inverse32(plan, bins_zeroed)));
    CHECK(same(bins, bins_kept));
    const std::vector<Complex32> faint(plan.bins(), Complex32{.re = tiny, .im = tiny});
    const std::vector<float> samples = inverse32(plan, faint);
    bool zeros = true;
    for (const float sample : samples) zeros = zeros && sample == 0.0F;
    CHECK(zeros);
}

void subnormal_rule() {
    // An impulse of 2^-1022 * 2^-4 is read as zero, so every bin is zero.
    // A subnormal result from normal inputs: x[0] = 2^-1022 (1 + 2^-52),
    // x[1] = -2^-1022 makes bin 0 (plain, dip) the exact sum 2^-1074,
    // which comes back as zero; bin N/2, x[0] - x[1], stays normal. (Bin 1
    // is subnormal too: x[0] - x[1] cos(pi/32) and x[1] sin(pi/32).)
    const Mode modes[3] = {Mode::plain, Mode::shifted, Mode::dip};
    for (const Mode mode : modes) {
        std::vector<double> x(64, 0.0);
        x[0] = DBL_MIN / 16.0;
        Plan64 plan = plan64(64, mode);
        CHECK(all_zero(forward64(plan, x)));
        if (mode != Mode::shifted) {
            std::vector<double> pair(64, 0.0);
            pair[0] = DBL_MIN + DBL_MIN * DBL_EPSILON;
            pair[1] = -DBL_MIN;
            const std::vector<Complex64> spectrum = forward64(plan, pair);
            CHECK(spectrum[0].re == 0.0 && spectrum[0].im == 0.0);
            CHECK(spectrum[32].re >= DBL_MIN);
        }
        subnormal_inputs64(mode);
    }
    std::vector<float> xf(64, 0.0F);
    xf[0] = FLT_MIN / 16.0F;
    Plan32 narrow = plan32(64, Mode::plain);
    CHECK(all_zero(forward32(narrow, xf)));
    subnormal_inputs32(Mode::plain);
    subnormal_inputs32(Mode::shifted);
}

// ---------------------------------------------------------------------------
// F10: the DIT engine.
// ---------------------------------------------------------------------------

[[nodiscard]] Plan64 engine_plan64(const usize n, const Level level, const Engines engines) {
    OperationResult<Plan64> made = PlanAccess::make64(n, Mode::plain, level, engines);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan64();
    return static_cast<OperationResult<Plan64>&&>(made).take_value();
}

[[nodiscard]] Plan32 engine_plan32(const usize n, const Level level, const Engines engines) {
    OperationResult<Plan32> made = PlanAccess::make32(n, Mode::plain, level, engines);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan32();
    return static_cast<OperationResult<Plan32>&&>(made).take_value();
}

constexpr Engines dit_engines{.forward = Engine::dit, .inverse = Engine::dit};

[[nodiscard]] bool same_engines(const Engines a, const Engines b) noexcept {
    return a.forward == b.forward && a.inverse == b.inverse;
}

void dit_engine_choice() {
    const Level selected = stxlib::simd::selected_level();
    // Below N = 16 a DIT choice runs DIF; from 16 it is kept.
    for (usize n = 4; n <= 32; n <<= 1) {
        Plan64 wide = engine_plan64(n, selected, dit_engines);
        Plan32 narrow = engine_plan32(n, selected, dit_engines);
        const Engines want = n < 16 ? Engines{} : dit_engines;
        CHECK(same_engines(PlanAccess::engines(wide), want));
        CHECK(same_engines(PlanAccess::engines(narrow), want));
    }
    // make() runs the policy's engines; the other modes run DIF.
    for (usize n = 16; n <= (usize{1} << 20); n <<= 2) {
        Plan64 wide = plan64(n, Mode::plain);
        CHECK(same_engines(PlanAccess::engines(wide), stxlib::fft::detail::engine_policy(selected, n, true)));
        Plan32 narrow = plan32(n, Mode::plain);
        CHECK(same_engines(PlanAccess::engines(narrow), stxlib::fft::detail::engine_policy(selected, n, false)));
        Plan64 shifted = plan64(n, Mode::shifted);
        CHECK(same_engines(PlanAccess::engines(shifted), Engines{}));
    }
}

// F1 for DIT, at the selected tier.
void dit_against_direct_sum() {
    const Level selected = stxlib::simd::selected_level();
    for (usize n = 16; n <= 4096; n <<= 1) {
        const std::vector<double> x = random_signal(n, n + 17);
        const std::vector<WideComplex> table = twiddles(n);
        std::vector<ReferenceBin> want(bins_of(Mode::plain, n));
        for (usize k = 0; k < want.size(); ++k) want[k] = direct_bin(x, table, step_of(Mode::plain, k));
        const double bound = log2_of(n) + 2.0;

        Plan64 wide = engine_plan64(n, selected, dit_engines);
        const double error64 = relative_l2(widened(forward64(wide, x)), want) / epsilon64;
        CHECK(error64 <= bound);
        if (error64 > bound) std::printf("  dit float64 N=%zu: %.3f eps\n", n, error64);

        const std::vector<float> xf = rounded_signal(x);
        std::vector<double> xr(n);
        for (usize index = 0; index < n; ++index) xr[index] = static_cast<double>(xf[index]);
        std::vector<ReferenceBin> want32(bins_of(Mode::plain, n));
        for (usize k = 0; k < want32.size(); ++k) want32[k] = direct_bin(xr, table, step_of(Mode::plain, k));
        Plan32 narrow = engine_plan32(n, selected, dit_engines);
        const double error32 = relative_l2(widened(forward32(narrow, xf)), want32) / epsilon32;
        CHECK(error32 <= bound);
        if (error32 > bound) std::printf("  dit float32 N=%zu: %.3f eps\n", n, error32);
    }
}

[[nodiscard]] double worst_difference(const std::vector<double>& a, const std::vector<double>& b) {
    double worst_sample = 0.0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        const double error = std::fabs(a[index] - b[index]);
        if (error > worst_sample) worst_sample = error;
    }
    return worst_sample;
}

[[nodiscard]] double worst_difference(const std::vector<float>& a, const std::vector<float>& b) {
    double worst_sample = 0.0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        const double error = std::fabs(static_cast<double>(a[index]) - static_cast<double>(b[index]));
        if (error > worst_sample) worst_sample = error;
    }
    return worst_sample;
}

// F3 and F4 for DIT, and the round trip across engines, at the selected tier.
void dit_round_trip_and_parseval() {
    const Level selected = stxlib::simd::selected_level();
    const Engines dit_then_dif{.forward = Engine::dit, .inverse = Engine::dif};
    const Engines dif_then_dit{.forward = Engine::dif, .inverse = Engine::dit};
    for (usize n = 16; n <= (usize{1} << 20); n <<= 1) {
        const std::vector<double> x = random_signal(n, 3 * n + 1);
        const Wide signal_energy = signal_energy_of(x);
        const double bound = log2_of(n) + 2.0;

        Plan64 wide = engine_plan64(n, selected, dit_engines);
        const std::vector<Complex64> spectrum = forward64(wide, x);
        CHECK(worst_difference(inverse64(wide, spectrum), x) / epsilon64 <= bound);
        const double defect = energy_defect(spectrum_energy(Mode::plain, widened(spectrum), n), signal_energy);
        CHECK(defect / epsilon64 <= 4.0 * bound);
        Plan64 forward_dit = engine_plan64(n, selected, dit_then_dif);
        Plan64 inverse_dit = engine_plan64(n, selected, dif_then_dit);
        CHECK(worst_difference(inverse64(forward_dit, forward64(forward_dit, x)), x) / epsilon64 <= bound);
        CHECK(worst_difference(inverse64(inverse_dit, forward64(inverse_dit, x)), x) / epsilon64 <= bound);

        const std::vector<float> xf = rounded_signal(x);
        const Wide signal_energy32 = signal_energy_of(xf);
        Plan32 narrow = engine_plan32(n, selected, dit_engines);
        const std::vector<Complex32> spectrum32 = forward32(narrow, xf);
        CHECK(worst_difference(inverse32(narrow, spectrum32), xf) / epsilon32 <= bound);
        const double defect32 = energy_defect(spectrum_energy(Mode::plain, widened(spectrum32), n), signal_energy32);
        CHECK(defect32 / epsilon32 <= 4.0 * bound);
        Plan32 forward_dit32 = engine_plan32(n, selected, dit_then_dif);
        Plan32 inverse_dit32 = engine_plan32(n, selected, dif_then_dit);
        CHECK(worst_difference(inverse32(forward_dit32, forward32(forward_dit32, xf)), xf) / epsilon32 <= bound);
        CHECK(worst_difference(inverse32(inverse_dit32, forward32(inverse_dit32, xf)), xf) / epsilon32 <= bound);
    }
}

// F6 for DIT: every tier gives the scalar tier's values.
void dit_tiers_agree() {
    const Level hardware = stxlib::simd::hardware_level();
    const Level levels[2] = {Level::vector128, Level::avx2_fma};
    for (const Level level : levels) {
        if (static_cast<int>(level) > static_cast<int>(hardware)) continue;
        for (usize n = 16; n <= (usize{1} << 18); n <<= 1) {
            const std::vector<double> x = random_signal(n, 7 * n + 5);
            Plan64 reference = engine_plan64(n, Level::scalar, dit_engines);
            Plan64 tier = engine_plan64(n, level, dit_engines);
            const std::vector<Complex64> want = forward64(reference, x);
            CHECK(same64(forward64(tier, x), want));
            CHECK(same_samples(inverse64(tier, want), inverse64(reference, want)));
            const std::vector<float> xf = rounded_signal(x);
            Plan32 reference32 = engine_plan32(n, Level::scalar, dit_engines);
            Plan32 tier32 = engine_plan32(n, level, dit_engines);
            const std::vector<Complex32> want32 = forward32(reference32, xf);
            CHECK(same32(forward32(tier32, xf), want32));
            CHECK(same_samples(inverse32(tier32, want32), inverse32(reference32, want32)));
        }
    }
}

} // namespace

int main() {
    const stxlib::TextView level = stxlib::simd::level_name(stxlib::simd::selected_level());
    std::printf("stx_fft tests: selected level %.*s, long double %d significand bits\n",
                static_cast<int>(level.count()), level.data(), LDBL_MANT_DIG);
    refusals();
    exact_cases();
    subnormal_rule();
    make_runs_the_selected_level();
    against_direct_sum(Mode::plain, 4);
    against_direct_sum(Mode::shifted, 2);
    against_direct_sum(Mode::dip, 4);
    large_bins(Mode::plain);
    large_bins(Mode::shifted);
    large_bins(Mode::dip);
    round_trip_and_parseval(Mode::plain, 4);
    round_trip_and_parseval(Mode::shifted, 2);
    round_trip_and_parseval(Mode::dip, 4);
    tiers_agree(Mode::plain, 4);
    tiers_agree(Mode::shifted, 2);
    tiers_agree(Mode::dip, 4);
    plain_and_dip_agree();
    dit_engine_choice();
    dit_against_direct_sum();
    dit_round_trip_and_parseval();
    dit_tiers_agree();
    const Worst& record = worst();
    std::printf("worst relative L2 error in epsilons: plain64 %.3f shifted64 %.3f dip64 %.3f plain32 %.3f shifted32 %.3f\n",
                record.plain64, record.shifted64, record.dip64, record.plain32, record.shifted32);
    return stxlib_tests::finish("stx_fft");
}
