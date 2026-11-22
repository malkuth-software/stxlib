// stx_fft against BFFT itself (brief step 4: "against BFFT itself as an
// oracle where it builds"). Built only when CMake is given
// STXLIB_BFFT_ORACLE_DIR, a BFFT checkout (falseywinchnet/bfft; the port
// is of edf5f01): this program compiles BFFT's own kernel headers from
// there, unchanged, beside STXlib's copies, and compares every mode and
// precision over sizes up to 2^16.
//
// The two are not bit-identical and are not meant to be: BFFT fuses
// multiply-adds where its compile-time tier has FMA (NEON, AVX2), builds
// DIF's cosines by a half-angle recurrence and BODFT's and DIP's twiddles
// with libm, and takes other float64 DIF paths (fused scatter, native
// order) that reorder the same arithmetic; STXlib unfuses, takes its
// twiddles from sincospi and runs the residue engine. The check is that
// the two agree within twice fft_tests' error bound, relative L2 over the
// spectrum or the signal: 2 (log2 N + 2) epsilons.
//
// BFFT's headers are foreign code here (MIT, the owner's own); they are
// included as system headers so their style is not this repository's
// concern.

#include "stxlib/fft.hpp"
#include "stxlib/result.hpp"
#include "stxlib/types.hpp"
#include "stxlib/view.hpp"

#include "test_support.hpp"

#include "detail/bodft_kernel.hpp"
#include "detail/bruun_dif_kernel.hpp"
#include "detail/bruun_dip_kernel.hpp"

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

struct Stream final {
    u64 state{0x243F6A8885A308D3ULL};

    [[nodiscard]] double next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const u64 mixed = state * 0x2545F4914F6CDD1DULL;
        return 2.0 * (static_cast<double>(mixed >> 11) * 0x1.0p-53) - 1.0;
    }
};

[[nodiscard]] double log2_of(const usize n) noexcept {
    double result = 0.0;
    for (usize v = n; v > 1; v >>= 1) result = result + 1.0;
    return result;
}

// Relative L2 distance of two interleaved (re, im) or real sequences.
template <typename Value>
[[nodiscard]] double distance(const Value* const got, const Value* const want, const usize count) {
    long double error = 0.0L;
    long double norm = 0.0L;
    for (usize index = 0; index < count; ++index) {
        const long double d = static_cast<long double>(got[index]) - static_cast<long double>(want[index]);
        error = error + d * d;
        norm = norm + static_cast<long double>(want[index]) * static_cast<long double>(want[index]);
    }
    if (norm == 0.0L) return static_cast<double>(std::sqrt(error));
    return static_cast<double>(std::sqrt(error / norm));
}

template <typename Plan>
[[nodiscard]] Plan made(OperationResult<Plan>&& result) {
    CHECK(result.succeeded());
    if (!result.succeeded()) return Plan();
    return static_cast<OperationResult<Plan>&&>(result).take_value();
}

void plain_against_bfft(const usize n) {
    const int ni = static_cast<int>(n);
    Stream stream;
    std::vector<double> x(n);
    for (double& sample : x) sample = stream.next();
    std::vector<float> xf(n);
    for (usize index = 0; index < n; ++index) xf[index] = static_cast<float>(x[index]);
    const double bound = 2.0 * (log2_of(n) + 2.0);

    bruun::DIF_RFFT_kernel oracle;
    CHECK(oracle.init(ni));
    std::vector<bruun::complex_t> want(n / 2 + 1);
    std::vector<double> work(static_cast<usize>(oracle.work_size()));
    std::vector<bruun::complex_t> native(static_cast<usize>(oracle.native_scratch_size()));
    oracle.forward_standard(x.data(), want.data(), work.data(), native.data());

    Plan64 plan = made(Plan64::make(n, Mode::plain));
    std::vector<Complex64> got(plan.bins());
    CHECK(plan.rfft(View<double>(x.data(), n), MutableView<Complex64>(got.data(), got.size())).succeeded());
    CHECK(distance(&got[0].re, &want[0].re, 2 * got.size()) / DBL_EPSILON <= bound);

    std::vector<double> back_want(n);
    oracle.inverse(want.data(), back_want.data());
    std::vector<double> back(n);
    CHECK(plan.irfft(View<Complex64>(got.data(), got.size()), MutableView<double>(back.data(), n)).succeeded());
    CHECK(distance(back.data(), back_want.data(), n) / DBL_EPSILON <= bound);

    std::vector<bruun::complex_f32_t> want32(n / 2 + 1);
    std::vector<float> work32(static_cast<usize>(oracle.work_size_f32()));
    oracle.forward_standard_f32(xf.data(), want32.data(), work32.data(), nullptr);
    Plan32 narrow = made(Plan32::make(n, Mode::plain));
    std::vector<Complex32> got32(narrow.bins());
    CHECK(narrow.rfft(View<float>(xf.data(), n), MutableView<Complex32>(got32.data(), got32.size())).succeeded());
    CHECK(distance(&got32[0].re, &want32[0].re, 2 * got32.size()) / static_cast<double>(FLT_EPSILON) <= bound);
}

void shifted_against_bfft(const usize n) {
    Stream stream;
    std::vector<double> x(n);
    for (double& sample : x) sample = stream.next();
    std::vector<float> xf(n);
    for (usize index = 0; index < n; ++index) xf[index] = static_cast<float>(x[index]);
    const double bound = 2.0 * (log2_of(n) + 2.0);

    const bodft::plan oracle(static_cast<int>(n));
    std::vector<bruun::complex_t> want(n / 2);
    oracle.forward(x.data(), want.data());
    Plan64 plan = made(Plan64::make(n, Mode::shifted));
    std::vector<Complex64> got(plan.bins());
    CHECK(plan.rfft(View<double>(x.data(), n), MutableView<Complex64>(got.data(), got.size())).succeeded());
    CHECK(distance(&got[0].re, &want[0].re, 2 * got.size()) / DBL_EPSILON <= bound);
    std::vector<double> back_want(n);
    oracle.inverse(want.data(), back_want.data());
    std::vector<double> back(n);
    CHECK(plan.irfft(View<Complex64>(got.data(), got.size()), MutableView<double>(back.data(), n)).succeeded());
    CHECK(distance(back.data(), back_want.data(), n) / DBL_EPSILON <= bound);

    const bodft::plan_f32 oracle32(static_cast<int>(n));
    std::vector<bruun::complex_f32_t> want32(n / 2);
    oracle32.forward(xf.data(), want32.data());
    Plan32 narrow = made(Plan32::make(n, Mode::shifted));
    std::vector<Complex32> got32(narrow.bins());
    CHECK(narrow.rfft(View<float>(xf.data(), n), MutableView<Complex32>(got32.data(), got32.size())).succeeded());
    CHECK(distance(&got32[0].re, &want32[0].re, 2 * got32.size()) / static_cast<double>(FLT_EPSILON) <= bound);
}

void dip_against_bfft(const usize n) {
    Stream stream;
    std::vector<double> x(n);
    for (double& sample : x) sample = stream.next();
    const double bound = 2.0 * (log2_of(n) + 2.0);

    bruun::DIP_RFFT_kernel oracle;
    CHECK(oracle.init(static_cast<int>(n)));
    std::vector<bruun::complex_t> want(n / 2 + 1);
    std::vector<double> work(n);
    oracle.forward_standard(x.data(), want.data(), work.data());
    Plan64 plan = made(Plan64::make(n, Mode::dip));
    std::vector<Complex64> got(plan.bins());
    CHECK(plan.rfft(View<double>(x.data(), n), MutableView<Complex64>(got.data(), got.size())).succeeded());
    CHECK(distance(&got[0].re, &want[0].re, 2 * got.size()) / DBL_EPSILON <= bound);
    std::vector<double> back_want(n);
    oracle.inverse_standard(want.data(), back_want.data(), work.data());
    std::vector<double> back(n);
    CHECK(plan.irfft(View<Complex64>(got.data(), got.size()), MutableView<double>(back.data(), n)).succeeded());
    CHECK(distance(back.data(), back_want.data(), n) / DBL_EPSILON <= bound);
}

} // namespace

int main() {
    for (usize n = 4; n <= (usize{1} << 16); n <<= 1) {
        plain_against_bfft(n);
        dip_against_bfft(n);
    }
    for (usize n = 2; n <= (usize{1} << 16); n <<= 1) shifted_against_bfft(n);
    return stxlib_tests::finish("stx_fft against BFFT");
}
