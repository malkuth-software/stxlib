// Tests of stx_fft's walk (src/algorithms/fft/walk.inc, walk_plan.cpp): the
// plain mode's kernel from N = 16, in both of its builds, at every walk tier
// this machine runs.
//
// Properties checked:
//   W1  every size from 16 to 2^20, both precisions: against a long-double
//       direct DFT up to 4096 (the largest error over the bins, over the
//       largest |X|), the round trip irfft(rfft(x)) = x, and magnitude
//       against |X| of rfft (straight-line sizes included: the small
//       kernels' magnitude check); the prototype's bounds, 1e-11 in float64
//       and 3e-4 in float32;
//   W2  frozen hashes: FNV-1a 64 over the bytes of X, then y = irfft(X),
//       then the magnitudes, for each size from 16 to 2^20 in order, per
//       family. Each build is bit-identical on every machine that runs it
//       (docs/CODE_STYLE_APPENDIX.md, A1 rule 1's exception): the exact
//       build at exact_portable and exact128, the fused build at fused128.
//       The values are the prototype's (~/Developer/work/fft/ideal,
//       final_check.cpp, with STXlib's sincospi twiddles; 2026-11-23);
//   W3  the layout changes only placement and order: without gaps, with
//       every pass one sweep and with other granules, the bits are the
//       default plan's;
//   W4  the straight-line kernels and the walk agree within the bounds
//       (N = 16 .. 128 in float64, 16 .. 256 in float32);
//   W5  eight threads run one plan at once through the const calls, each
//       with its own workspace, 200 times each, and every result has the
//       single-thread bits (float64 64 and 4096, float32 65536);
//   W6  the const calls' refusals: the empty plan, counts, too little work,
//       work overlapping the input or output, a mode other than plain
//       (unsupported); work_size() is 0 outside the plain mode; nothing is
//       written on a refusal;
//   W7  make() runs the walk from N = 16 (DIF below) at
//       walk_tier_for(selected_level()); the plan's own workspace gives the
//       const calls' bits; a subnormal input reads as zero of its sign.

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
#include <cstring>
#include <thread>
#include <vector>

namespace {

using stxlib::MutableView;
using stxlib::OperationResult;
using stxlib::OperationStatus;
using stxlib::u32;
using stxlib::u64;
using stxlib::usize;
using stxlib::View;
using stxlib::fft::Complex32;
using stxlib::fft::Complex64;
using stxlib::fft::Mode;
using stxlib::fft::Plan32;
using stxlib::fft::Plan64;
using stxlib::fft::detail::Engines;
using stxlib::fft::detail::PlanAccess;
using stxlib::fft::detail::WalkOptions;
using stxlib::fft::detail::WalkTier;
using stxlib::simd::Level;

// The frozen hashes (W2).
constexpr u64 exact_hash64 = 0xe22e6c7a68a2521dULL;
constexpr u64 exact_hash32 = 0x466bd237950ebaf6ULL;
constexpr u64 fused_hash64 = 0x1c022fd0b19e00ccULL;
constexpr u64 fused_hash32 = 0xaed487c31d2d81f1ULL;

constexpr double bound64 = 1e-11;
constexpr double bound32 = 3e-4;
constexpr usize largest_log2 = 20;
constexpr usize oracle_limit = 4096;

const char* tier_name(const WalkTier tier) noexcept {
    if (tier == WalkTier::fused128) return "fused128";
    if (tier == WalkTier::exact128) return "exact128";
    return "exact_portable";
}

// The prototype's input: x[i] = ((i 2654435761 mod 2^32) >> 8 & 0xffff) / 65536 - 0.5,
// exact in float and double.
STXLIB_MODULAR double sample_at(const usize index) noexcept {
    const u32 mixed = static_cast<u32>(index) * 2654435761U;
    return static_cast<double>((mixed >> 8) & 0xffffU) / 65536.0 - 0.5;
}

struct Fnv final {
    u64 value{1469598103934665603ULL};
    STXLIB_MODULAR void mix(const void* const data, const usize bytes) noexcept {
        const unsigned char* const at = static_cast<const unsigned char*>(data);
        for (usize index = 0; index < bytes; ++index) {
            value = value ^ at[index];
            value = value * 1099511628211ULL;
        }
    }
};

// The long-double direct DFT of the prototype's input (W1), N <= 4096: re
// and im of X[k], k = 0 .. N/2. The input is the same in both precisions.
struct Oracle final {
    std::vector<long double> re;
    std::vector<long double> im;
};

Oracle oracle_of(const usize n) {
    Oracle result;
    result.re.assign(n / 2 + 1, 0.0L);
    result.im.assign(n / 2 + 1, 0.0L);
    std::vector<long double> cosine(n);
    std::vector<long double> sine(n);
    const long double tau = 2.0L * 3.141592653589793238462643383279502884L;
    for (usize index = 0; index < n; ++index) {
        const long double angle = -tau * static_cast<long double>(index) / static_cast<long double>(n);
        cosine[index] = std::cos(angle);
        sine[index] = std::sin(angle);
    }
    for (usize k = 0; k <= n / 2; ++k) {
        long double re = 0.0L;
        long double im = 0.0L;
        for (usize j = 0; j < n; ++j) {
            const usize at = (k * j) % n;
            const long double x = static_cast<long double>(sample_at(j));
            re = re + x * cosine[at];
            im = im + x * sine[at];
        }
        result.re[k] = re;
        result.im[k] = im;
    }
    return result;
}

Engines walk_engines() noexcept {
    const Engines result{.forward = stxlib::fft::detail::Engine::walk, .inverse = stxlib::fft::detail::Engine::walk};
    return result;
}

WalkOptions tier_options(const WalkTier tier) noexcept {
    WalkOptions options{};
    options.automatic_tier = false;
    options.tier = tier;
    return options;
}

// Plans made at the scalar level with a chosen walk tier: the level only
// picks DIF's tier, which no size here runs.
Plan64 walk_plan64(const usize n, const WalkOptions& options) {
    OperationResult<Plan64> made = PlanAccess::make64(n, Mode::plain, Level::scalar, walk_engines(), options);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan64();
    return static_cast<OperationResult<Plan64>&&>(made).take_value();
}

Plan32 walk_plan32(const usize n, const WalkOptions& options) {
    OperationResult<Plan32> made = PlanAccess::make32(n, Mode::plain, Level::scalar, walk_engines(), options);
    CHECK(made.succeeded());
    if (!made.succeeded()) return Plan32();
    return static_cast<OperationResult<Plan32>&&>(made).take_value();
}

// One plan's outputs for the prototype's input.
template <typename Real, typename Complex>
struct Outputs final {
    std::vector<Complex> spectrum;
    std::vector<Real> back;
    std::vector<Real> magnitude;
};

template <typename Plan, typename Real, typename Complex>
Outputs<Real, Complex> outputs_of(const Plan& plan, const std::vector<Real>& x) {
    const usize n = x.size();
    Outputs<Real, Complex> result;
    result.spectrum.assign(n / 2 + 1, Complex{});
    result.back.assign(n, Real{0});
    result.magnitude.assign(n / 2 + 1, Real{0});
    std::vector<Real> work(plan.work_size());
    const MutableView<Real> work_view(work.data(), work.size());
    const MutableView<Complex> spectrum(result.spectrum.data(), result.spectrum.size());
    CHECK(plan.rfft_with_work(View<Real>(x.data(), n), spectrum, work_view).succeeded());
    CHECK(plan.irfft_with_work(View<Complex>(result.spectrum.data(), result.spectrum.size()),
                     MutableView<Real>(result.back.data(), n), work_view)
              .succeeded());
    const MutableView<Real> magnitude(result.magnitude.data(), result.magnitude.size());
    CHECK(plan.magnitude_with_work(View<Real>(x.data(), n), magnitude, work_view).succeeded());
    return result;
}

template <typename Real>
std::vector<Real> prototype_input(const usize n) {
    std::vector<Real> x(n);
    for (usize index = 0; index < n; ++index) x[index] = static_cast<Real>(sample_at(index));
    return x;
}

template <typename Real, typename Complex>
bool same_bits(const Outputs<Real, Complex>& a, const Outputs<Real, Complex>& b) {
    return std::memcmp(a.spectrum.data(), b.spectrum.data(), a.spectrum.size() * sizeof(Complex)) == 0 &&
           std::memcmp(a.back.data(), b.back.data(), a.back.size() * sizeof(Real)) == 0 &&
           std::memcmp(a.magnitude.data(), b.magnitude.data(), a.magnitude.size() * sizeof(Real)) == 0;
}

// W1's three measures, each over the largest |X| (the round trip absolute).
template <typename Real, typename Complex>
bool within_bounds(const Outputs<Real, Complex>& got, const std::vector<Real>& x, const Oracle* const oracle,
                   const double bound, const char* const what, const usize n) {
    double scale = 0.0;
    double magnitude_error = 0.0;
    for (usize k = 0; k < got.spectrum.size(); ++k) {
        const double re = static_cast<double>(got.spectrum[k].re);
        const double im = static_cast<double>(got.spectrum[k].im);
        const double size = std::hypot(re, im);
        if (size > scale) scale = size;
        const double error = std::fabs(static_cast<double>(got.magnitude[k]) - size);
        if (error > magnitude_error) magnitude_error = error;
    }
    double round_trip = 0.0;
    for (usize index = 0; index < x.size(); ++index) {
        const double error = std::fabs(static_cast<double>(got.back[index]) - static_cast<double>(x[index]));
        if (error > round_trip) round_trip = error;
    }
    double forward = 0.0;
    if (oracle != nullptr) {
        for (usize k = 0; k < got.spectrum.size(); ++k) {
            const long double dre = (*oracle).re[k] - static_cast<long double>(got.spectrum[k].re);
            const long double dim = (*oracle).im[k] - static_cast<long double>(got.spectrum[k].im);
            const double error = static_cast<double>(std::sqrt(dre * dre + dim * dim));
            if (error > forward) forward = error;
        }
        forward = forward / scale;
    }
    const bool good = forward < bound && round_trip < bound && magnitude_error / scale < bound;
    if (!good) {
        std::printf("  %s N=%zu: forward %.2e round trip %.2e magnitude %.2e\n", what, n, forward, round_trip,
                    magnitude_error / scale);
    }
    return good;
}

// The walk tiers this machine runs.
usize runnable_tiers(WalkTier (&tiers)[3]) noexcept {
    usize count = 0;
    const WalkTier all[3] = {WalkTier::exact_portable, WalkTier::exact128, WalkTier::fused128};
    for (const WalkTier tier : all) {
        if (!stxlib::fft::detail::walk_tier_runs(tier)) continue;
        tiers[count] = tier;
        count = count + 1;
    }
    return count;
}

// ---------------------------------------------------------------------------
// W1, W2.
// ---------------------------------------------------------------------------

void sizes_and_hashes(const WalkTier* const tiers, const usize tier_count) {
    u64 hash64[3]{};
    u64 hash32[3]{};
    Fnv fnv64[3];
    Fnv fnv32[3];
    for (usize lg = 4; lg <= largest_log2; ++lg) {
        const usize n = usize{1} << lg;
        const std::vector<double> x = prototype_input<double>(n);
        const std::vector<float> xf = prototype_input<float>(n);
        Oracle oracle;
        const bool checked = n <= oracle_limit;
        if (checked) oracle = oracle_of(n);
        for (usize which = 0; which < tier_count; ++which) {
            const WalkOptions options = tier_options(tiers[which]);
            const Plan64 wide = walk_plan64(n, options);
            const Outputs<double, Complex64> got = outputs_of<Plan64, double, Complex64>(wide, x);
            CHECK(within_bounds(got, x, checked ? &oracle : nullptr, bound64, tier_name(tiers[which]), n));
            fnv64[which].mix(got.spectrum.data(), got.spectrum.size() * sizeof(Complex64));
            fnv64[which].mix(got.back.data(), got.back.size() * sizeof(double));
            fnv64[which].mix(got.magnitude.data(), got.magnitude.size() * sizeof(double));

            const Plan32 narrow = walk_plan32(n, options);
            const Outputs<float, Complex32> got32 = outputs_of<Plan32, float, Complex32>(narrow, xf);
            CHECK(within_bounds(got32, xf, checked ? &oracle : nullptr, bound32, tier_name(tiers[which]), n));
            fnv32[which].mix(got32.spectrum.data(), got32.spectrum.size() * sizeof(Complex32));
            fnv32[which].mix(got32.back.data(), got32.back.size() * sizeof(float));
            fnv32[which].mix(got32.magnitude.data(), got32.magnitude.size() * sizeof(float));
        }
    }
    for (usize which = 0; which < tier_count; ++which) {
        hash64[which] = fnv64[which].value;
        hash32[which] = fnv32[which].value;
        const bool fused = stxlib::fft::detail::walk_fused(tiers[which]);
        const u64 want64 = fused ? fused_hash64 : exact_hash64;
        const u64 want32 = fused ? fused_hash32 : exact_hash32;
        std::printf("walk %s: hash f64 %016llx (want %016llx), f32 %016llx (want %016llx)\n", tier_name(tiers[which]),
                    static_cast<unsigned long long>(hash64[which]), static_cast<unsigned long long>(want64),
                    static_cast<unsigned long long>(hash32[which]), static_cast<unsigned long long>(want32));
        CHECK(hash64[which] == want64);
        CHECK(hash32[which] == want32);
    }
}

// ---------------------------------------------------------------------------
// W3, W4.
// ---------------------------------------------------------------------------

void layouts_agree(const WalkTier* const tiers, const usize tier_count) {
    const usize sizes[4] = {usize{1} << 9, usize{1} << 12, usize{1} << 15, usize{1} << 17};
    const u32 granules[4] = {0, 64, 1024, 1U << 14};
    for (usize which = 0; which < tier_count; ++which) {
        for (const usize n : sizes) {
            const std::vector<double> x = prototype_input<double>(n);
            const std::vector<float> xf = prototype_input<float>(n);
            const WalkOptions base = tier_options(tiers[which]);
            const Outputs<double, Complex64> want = outputs_of<Plan64, double, Complex64>(walk_plan64(n, base), x);
            const Outputs<float, Complex32> want32 = outputs_of<Plan32, float, Complex32>(walk_plan32(n, base), xf);
            for (usize variant = 0; variant < 5; ++variant) {
                WalkOptions options = base;
                if (variant == 4) {
                    options.gaps = false;
                } else {
                    options.granule = granules[variant];
                }
                CHECK(same_bits(outputs_of<Plan64, double, Complex64>(walk_plan64(n, options), x), want));
                CHECK(same_bits(outputs_of<Plan32, float, Complex32>(walk_plan32(n, options), xf), want32));
            }
        }
    }
}

void straight_line_agrees(const WalkTier* const tiers, const usize tier_count) {
    for (usize which = 0; which < tier_count; ++which) {
        for (usize n = 16; n <= 256; n <<= 1) {
            const WalkOptions small = tier_options(tiers[which]);
            WalkOptions passes = small;
            passes.straight_line = false;
            const std::vector<double> x = prototype_input<double>(n);
            const std::vector<float> xf = prototype_input<float>(n);
            const Oracle oracle = oracle_of(n);
            const Plan64 wide_small = walk_plan64(n, small);
            const Plan64 wide_passes = walk_plan64(n, passes);
            CHECK(PlanAccess::straight_line(wide_small) == (n <= 128));
            CHECK(!PlanAccess::straight_line(wide_passes));
            CHECK(within_bounds(outputs_of<Plan64, double, Complex64>(wide_passes, x), x, &oracle, bound64,
                                "passes", n));
            const Plan32 narrow_small = walk_plan32(n, small);
            const Plan32 narrow_passes = walk_plan32(n, passes);
            CHECK(PlanAccess::straight_line(narrow_small));
            CHECK(!PlanAccess::straight_line(narrow_passes));
            CHECK(within_bounds(outputs_of<Plan32, float, Complex32>(narrow_passes, xf), xf, &oracle, bound32,
                                "passes", n));
        }
    }
}

// ---------------------------------------------------------------------------
// W5.
// ---------------------------------------------------------------------------

template <typename Plan, typename Real, typename Complex>
struct Shared final {
    const Plan* plan{nullptr};
    const std::vector<Real>* x{nullptr};
    const std::vector<Complex>* want{nullptr};
    int wrong{0};
};

template <typename Plan, typename Real, typename Complex>
void shared_worker(Shared<Plan, Real, Complex>* const shared) {
    Shared<Plan, Real, Complex>& job = *shared;
    const Plan& plan = *job.plan;
    const usize n = (*job.x).size();
    std::vector<Real> work(plan.work_size());
    std::vector<Real> back(n);
    std::vector<Complex> spectrum(n / 2 + 1);
    const MutableView<Real> work_view(work.data(), work.size());
    for (int iteration = 0; iteration < 200; ++iteration) {
        const OperationStatus forward = plan.rfft_with_work(View<Real>((*job.x).data(), n),
                                                  MutableView<Complex>(spectrum.data(), spectrum.size()), work_view);
        if (forward.failed() ||
            std::memcmp(spectrum.data(), (*job.want).data(), spectrum.size() * sizeof(Complex)) != 0) {
            job.wrong = 1;
        }
        const OperationStatus inverse = plan.irfft_with_work(View<Complex>(spectrum.data(), spectrum.size()),
                                                   MutableView<Real>(back.data(), n), work_view);
        if (inverse.failed()) job.wrong = 1;
    }
}

template <typename Plan, typename Real, typename Complex>
void threads_share(const Plan& plan, const usize n, const char* const what) {
    const std::vector<Real> x = prototype_input<Real>(n);
    const Outputs<Real, Complex> want = outputs_of<Plan, Real, Complex>(plan, x);
    Shared<Plan, Real, Complex> jobs[8];
    std::vector<std::thread> threads;
    for (Shared<Plan, Real, Complex>& job : jobs) {
        job.plan = &plan;
        job.x = &x;
        job.want = &want.spectrum;
        threads.emplace_back(&shared_worker<Plan, Real, Complex>, &job);
    }
    for (std::thread& thread : threads) thread.join();
    int wrong = 0;
    for (const Shared<Plan, Real, Complex>& job : jobs) wrong = wrong + job.wrong;
    std::printf("%s N=%zu: 8 threads sharing one plan, %d with a wrong result\n", what, n, wrong);
    CHECK(wrong == 0);
}

void threads_share_one_plan() {
    for (const usize n : {usize{64}, usize{4096}}) {
        OperationResult<Plan64> made = Plan64::make(n, Mode::plain);
        CHECK(made.succeeded());
        if (!made.succeeded()) continue;
        const Plan64 plan = static_cast<OperationResult<Plan64>&&>(made).take_value();
        threads_share<Plan64, double, Complex64>(plan, n, "f64");
    }
    OperationResult<Plan32> made = Plan32::make(65536, Mode::plain);
    CHECK(made.succeeded());
    if (!made.succeeded()) return;
    const Plan32 plan = static_cast<OperationResult<Plan32>&&>(made).take_value();
    threads_share<Plan32, float, Complex32>(plan, 65536, "f32");
}

// ---------------------------------------------------------------------------
// W6.
// ---------------------------------------------------------------------------

bool refused_as(const OperationStatus status, const stxlib::ErrorKind kind) {
    return status.failed() && (*status.error()).kind() == kind;
}

void const_refusals() {
    constexpr stxlib::ErrorKind invalid = stxlib::ErrorKind::invalid_argument;
    const usize n = 1024;
    OperationResult<Plan64> made = Plan64::make(n, Mode::plain);
    CHECK(made.succeeded());
    if (!made.succeeded()) return;
    const Plan64 plan = static_cast<OperationResult<Plan64>&&>(made).take_value();
    const usize work_size = plan.work_size();
    CHECK(work_size >= n + 2);
    std::vector<double> x = prototype_input<double>(n);
    std::vector<Complex64> spectrum(n / 2 + 1, Complex64{.re = 7.0, .im = 7.0});
    std::vector<double> back(n, 7.0);
    std::vector<double> magnitude(n / 2 + 1, 7.0);
    std::vector<double> work(work_size + n);
    const View<double> input(x.data(), n);
    const MutableView<Complex64> output(spectrum.data(), spectrum.size());
    const MutableView<double> enough(work.data(), work_size);

    CHECK(refused_as(plan.rfft_with_work(input, output, MutableView<double>(work.data(), work_size - 1)), invalid));
    CHECK(refused_as(plan.rfft_with_work(View<double>(x.data(), n - 1), output, enough), invalid));
    CHECK(refused_as(plan.rfft_with_work(input, MutableView<Complex64>(spectrum.data(), spectrum.size() - 1), enough), invalid));
    // Work overlapping the input, then the output.
    std::vector<double> joined(work_size + n);
    for (usize index = 0; index < n; ++index) joined[index] = x[index];
    CHECK(refused_as(
        plan.rfft_with_work(View<double>(joined.data(), n), output, MutableView<double>(joined.data() + n - 1, work_size)), invalid));
    CHECK(refused_as(plan.magnitude_with_work(input, MutableView<double>(joined.data(), n / 2 + 1),
                                    MutableView<double>(joined.data() + n / 2, work_size)),
                     invalid));
    std::vector<double> region(work_size + n);
    CHECK(refused_as(plan.irfft_with_work(View<Complex64>(spectrum.data(), spectrum.size()), MutableView<double>(region.data(), n),
                                MutableView<double>(region.data() + 1, work_size)),
                     invalid));
    bool untouched = true;
    for (const Complex64& value : spectrum) untouched = untouched && value.re == 7.0 && value.im == 7.0;
    for (const double value : back) untouched = untouched && value == 7.0;
    CHECK(untouched);

    const Plan64 empty;
    CHECK(empty.work_size() == 0);
    CHECK(refused_as(empty.rfft_with_work(input, output, enough), invalid));
    CHECK(refused_as(empty.magnitude_with_work(input, MutableView<double>(magnitude.data(), magnitude.size()), enough), invalid));

    OperationResult<Plan64> shifted_made = Plan64::make(n, Mode::shifted);
    CHECK(shifted_made.succeeded());
    if (!shifted_made.succeeded()) return;
    const Plan64 shifted = static_cast<OperationResult<Plan64>&&>(shifted_made).take_value();
    CHECK(shifted.work_size() == 0);
    CHECK(refused_as(shifted.rfft_with_work(input, MutableView<Complex64>(spectrum.data(), n / 2), enough),
                     stxlib::ErrorKind::unsupported));
    // The counts come first (shifted has N/2 bins), then the mode.
    CHECK(refused_as(shifted.magnitude_with_work(input, MutableView<double>(magnitude.data(), n / 2), enough),
                     stxlib::ErrorKind::unsupported));
    CHECK(refused_as(shifted.magnitude_with_work(input, MutableView<double>(magnitude.data(), n / 2 + 1), enough), invalid));
}

// ---------------------------------------------------------------------------
// W7.
// ---------------------------------------------------------------------------

void make_runs_the_walk() {
    const Level selected = stxlib::simd::selected_level();
    const WalkTier tier = stxlib::fft::detail::walk_tier_for(selected);
    for (usize n = 4; n <= (usize{1} << 16); n <<= 2) {
        OperationResult<Plan64> made = Plan64::make(n, Mode::plain);
        CHECK(made.succeeded());
        if (!made.succeeded()) continue;
        Plan64 plan = static_cast<OperationResult<Plan64>&&>(made).take_value();
        const Engines engines = PlanAccess::engines(plan);
        const stxlib::fft::detail::Engine want =
            n < 16 ? stxlib::fft::detail::Engine::dif : stxlib::fft::detail::Engine::walk;
        CHECK(engines.forward == want && engines.inverse == want);
        CHECK(PlanAccess::walk_tier(plan) == tier);

        // The plan's own workspace gives the const calls' bits.
        const std::vector<double> x = prototype_input<double>(n);
        const Outputs<double, Complex64> want_bits = outputs_of<Plan64, double, Complex64>(plan, x);
        Outputs<double, Complex64> own;
        own.spectrum.assign(n / 2 + 1, Complex64{});
        own.back.assign(n, 0.0);
        own.magnitude.assign(n / 2 + 1, 0.0);
        CHECK(plan.rfft(View<double>(x.data(), n), MutableView<Complex64>(own.spectrum.data(), own.spectrum.size()))
                  .succeeded());
        CHECK(plan.irfft(View<Complex64>(own.spectrum.data(), own.spectrum.size()),
                         MutableView<double>(own.back.data(), n))
                  .succeeded());
        CHECK(plan.magnitude(View<double>(x.data(), n), MutableView<double>(own.magnitude.data(), own.magnitude.size()))
                  .succeeded());
        CHECK(same_bits(own, want_bits));

        // A subnormal input reads as zero of its sign.
        std::vector<double> with_subnormal = x;
        std::vector<double> with_zero = x;
        with_subnormal[n / 2] = DBL_MIN / 8.0;
        with_zero[n / 2] = 0.0;
        CHECK(same_bits(outputs_of<Plan64, double, Complex64>(plan, with_subnormal),
                        outputs_of<Plan64, double, Complex64>(plan, with_zero)));
    }
}

} // namespace

int main() {
    const stxlib::TextView level = stxlib::simd::level_name(stxlib::simd::selected_level());
    WalkTier tiers[3]{};
    const usize tier_count = runnable_tiers(tiers);
    std::printf("stx_fft walk tests: selected level %.*s, %zu walk tiers, long double %d significand bits\n",
                static_cast<int>(level.count()), level.data(), tier_count, LDBL_MANT_DIG);
    const_refusals();
    make_runs_the_walk();
    sizes_and_hashes(tiers, tier_count);
    layouts_agree(tiers, tier_count);
    straight_line_agrees(tiers, tier_count);
    threads_share_one_plan();
    return stxlib_tests::finish("stx_fft walk");
}
