#pragma once

// Owned transcendental functions. GUI.Forms and its consumers call these
// instead of <cmath>'s, whose results differ between C libraries. Copied
// from stx_auxlib's include/stx_auxlib/math.hpp at b6e330a with the
// kernels of src/math/ (now src/algorithms/math/): names and paths
// changed, every computation and stated bound unchanged (PROVENANCE.md,
// batch 3c5).
//
// Contract shared by every function here:
// - Results are the same bits on every architecture and compiler. The
//   definitions live in src/algorithms/math/ and compile only with STXlib's own
//   flags, which switch floating-point contraction off; a consumer's flags
//   (fused multiply-add, -ffp-contract=fast) never reach them. The one exact
//   product they use is exact whether formed by a hardware fma or by
//   Dekker's split.
// - Accuracy is stated per function, measured against the double-double
//   references in tests/reference/wide.hpp (tests/math_tests.cpp). The
//   exhaustive counts, sample counts and the sincos certificate below were
//   run on stx_auxlib's build of these same kernels; STXlib's build reruns
//   the frozen-hash tests in ctest, and proofs/REPORT.md (batch 3c5) says
//   which of the larger runs have been repeated on it.
// - Rare inputs leave the fast path through one compare. Every function
//   owns every input; none returns the C++ library's result.
// - No subnormals (STXlib batch 3c8; owner decision 2026-11-13, notes
//   decisions/2026-11-13-locals-float-mutability.md, "Subnormals"): every
//   subnormal argument is read as zero of its sign, and a result that
//   would be subnormal is returned as zero of its sign, so no function here
//   accepts or returns a subnormal value. "Would be subnormal" means the
//   kernel's rounded result is below the smallest normal (2^-126 for float,
//   2^-1022 for double); where a kernel is not correctly rounded, its own
//   rounding decides between zero and the smallest normal for an exact
//   result within an ulp of it. The rule is applied in software, on the
//   bits; the floating-point environment (flush-to-zero, denormals-are-zero,
//   rounding mode, flags) is never changed. The accuracy stated below is
//   for normal results of normal or zero arguments; the subnormal outcomes
//   each function lists follow from this rule. Zero, infinite and NaN
//   values are unaffected. The bits below the rule are stx_auxlib's.

namespace stxlib::math {

// Angles for sin, cos and tan are supported up to |x| = 2^32 radians;
// larger angles, infinities and NaN are refused with NaN. No GUI.Forms use
// needs more (a full turn is 6.28): a phase that grows without bound must
// reset itself or use its own reduction.
//
// sin and cos of a float, correctly rounded for every float with
// 2^-27 <= |x| <= 2^20 (exhaustively checked, 788,529,154 floats); below
// 2^-27 sin = x and cos = 1, also correctly rounded. A subnormal angle
// gives sin = zero of its sign and cos = 1.
void sincos(float angle, float& s, float& c) noexcept;

// sin and cos of a double, within 0.51 ulp for every double 2^-27 <= |x| <=
// 2^32: proved, not sampled (tools/math/sincos_certificate.py: the worst-case
// remainders of every binade, the rotation's interval error analysis and
// the reduction's exact error bound). The largest error measured over 120
// million sampled inputs is 0.500415 ulp; about 1 in 18,000 results is not
// the correctly rounded double. Below 2^-27 sin = x and cos = 1 (correctly
// rounded, sign of zero kept); a subnormal x gives sin = zero of its sign
// and cos = 1. No angle in [2^-27, 2^32] has a subnormal sine or cosine, so
// the bound above holds for the public function unchanged.
void sincos(double x, double& s, double& c) noexcept;

// sin and cos as single functions: the s and c of sincos above, with its
// contracts and bits (fronts that call it; both are computed).
[[nodiscard]] float sin(float angle) noexcept;
[[nodiscard]] float cos(float angle) noexcept;
[[nodiscard]] double sin(double x) noexcept;
[[nodiscard]] double cos(double x) noexcept;

// e^x for a float, correctly rounded for every float (exhaustively checked,
// 4,278,190,080 finite floats), overflow to infinity included; a correctly
// rounded result below 2^-126 (x below about -87.336) returns +0, and a
// subnormal x gives 1. NaN returns NaN, -infinity returns +0. Owns every
// input.
[[nodiscard]] float exp(float x) noexcept;

// e^x for a double, within 0.500001 ulp (100 million inputs measured; about
// 1 in 6 million results is not the correctly rounded double). Owns every
// input: above ln(DBL_MAX) it returns +infinity; a result below 2^-1022 (x
// below about ln(2^-1022) = -708.396) returns +0, and a subnormal x gives
// 1; NaN returns NaN and -infinity returns +0.
[[nodiscard]] double exp(double x) noexcept;

// Natural, binary and decimal logarithms. Float: correctly rounded for every
// positive finite float (exhaustively checked, 2,139,095,039 floats each).
// Double: within 0.500012 ulp (240 million inputs measured per function;
// about 1 in 24 million results is not the correctly rounded double).
// Own every input: +0 and -0 give -infinity, negative x and -infinity give
// NaN, +infinity gives +infinity, NaN gives NaN; a subnormal x is read as
// zero of its sign and so gives -infinity, a negative one included. No
// result is subnormal. log(1) = +0, log2(2^k) = k and log10(10^n) = n (for the exactly
// representable powers) are exact.
[[nodiscard]] double log(double x) noexcept;
[[nodiscard]] float log(float x) noexcept;
[[nodiscard]] double log2(double x) noexcept;
[[nodiscard]] float log2(float x) noexcept;
[[nodiscard]] double log10(double x) noexcept;
[[nodiscard]] float log10(float x) noexcept;

// x^y. Owns every input, following C99 Annex F: pow(x, +-0) = 1 and
// pow(1, y) = 1 even for NaN; negative x is defined for integer y only (NaN
// otherwise) and takes the sign of the integer's parity; zero and infinite
// x and infinite y give the Annex F zeros and infinities. Exactly
// representable results (2^n, 3^2, 10^n for n <= 22, x^1, ...) are exact.
// Double: within 0.5 + 0.076 ulp by analysis of every rounding, for normal
// results; measured 0.5027 ulp at worst over 2 million inputs, the same bits
// at every SIMD level. A result below the smallest normal returns zero of
// its sign (stx_auxlib's kernel rounded those on the subnormal grid, 1 ulp
// off on 6 of 20 million); a subnormal x is read as zero of its sign (the
// Annex F zero cases then apply: pow(subnormal, -1) = +infinity), and a
// subnormal y as zero (pow(x, subnormal) = 1). Float: correctly rounded at
// every sampled pair (the double result rounded once), then flushed.
[[nodiscard]] double pow(double x, double y) noexcept;
[[nodiscard]] float pow(float x, float y) noexcept;

// sqrt(x^2 + y^2) without intermediate overflow or underflow. Owns every
// input: an infinite operand gives +infinity even when the other is NaN
// (Annex F), otherwise NaN gives NaN; hypot(x, +-0) = |x|. Subnormal
// operands are read as zero: hypot of two subnormals is +0. Accuracy: see
// tests/math_tests.cpp.
[[nodiscard]] double hypot(double x, double y) noexcept;
[[nodiscard]] float hypot(float x, float y) noexcept;

// Inverse trigonometric functions. Own every input, following C99 Annex F:
// atan(+-infinity) = +-pi/2; atan2's zeros, infinities and signs as Annex F
// lists them (atan2(+-0, -0) = +-pi, atan2(+-0, +0) = +-0, ...); asin and
// acos are NaN outside [-1, 1]; acos(1) = +0; signs of zero are kept.
// Subnormal arguments are read as zero of their sign (atan and asin give
// that zero, acos gives pi/2, atan2 follows its zero cases), and a result
// that would be subnormal (atan2 of a tiny y over a huge x) is zero of its
// sign. Accuracy: see tests/math_tests.cpp.
[[nodiscard]] double atan(double x) noexcept;
[[nodiscard]] float atan(float x) noexcept;
[[nodiscard]] double atan2(double y, double x) noexcept;
[[nodiscard]] float atan2(float y, float x) noexcept;
[[nodiscard]] double asin(double x) noexcept;
[[nodiscard]] float asin(float x) noexcept;
[[nodiscard]] double acos(double x) noexcept;
[[nodiscard]] float acos(float x) noexcept;

// tan x for |x| <= 2^32 (refused above, as for sincos). Below 2^-27
// tan x = x (sign of zero kept); a subnormal x gives zero of its sign. Double: within 0.51 ulp for every double
// 2^-27 <= |x| <= 2^32, proved as for sincos (measured worst 0.500154 ulp
// over the sampled regions). Float: correctly rounded, checked exhaustively
// against the test reference for |x| <= 2^20, sampled above.
[[nodiscard]] double tan(double x) noexcept;
[[nodiscard]] float tan(float x) noexcept;

// Cube root. Owns every input: zeros, infinities and NaN are their own cube
// roots; exact cubes (8, 27, 2^(3k), ...) give exact roots. A subnormal x
// is read as zero of its sign and gives that zero (its exact cube root
// would be normal: 2^-1074 has root 2^-358).
[[nodiscard]] double cbrt(double x) noexcept;
[[nodiscard]] float cbrt(float x) noexcept;

// Hyperbolic functions and their inverses. Own every input: sinh and cosh
// overflow to infinity, tanh saturates at +-1, acosh(1) = +0, atanh(+-1) =
// +-infinity, NaN outside acosh's [1, inf) and atanh's [-1, 1], signs of
// zero kept. A subnormal x is read as zero of its sign: sinh, tanh, asinh
// and atanh give that zero, cosh gives 1, acosh gives NaN. Accuracy: see
// tests/math_tests.cpp.
[[nodiscard]] double sinh(double x) noexcept;
[[nodiscard]] float sinh(float x) noexcept;
[[nodiscard]] double cosh(double x) noexcept;
[[nodiscard]] float cosh(float x) noexcept;
[[nodiscard]] double tanh(double x) noexcept;
[[nodiscard]] float tanh(float x) noexcept;
[[nodiscard]] double asinh(double x) noexcept;
[[nodiscard]] float asinh(float x) noexcept;
[[nodiscard]] double acosh(double x) noexcept;
[[nodiscard]] float acosh(float x) noexcept;
[[nodiscard]] double atanh(double x) noexcept;
[[nodiscard]] float atanh(float x) noexcept;

// 2^x. Float: correctly rounded for every float (proved: the core's bound,
// tools/math/r1_certificate.py, and an exhaustive screen of every float on
// the fast path, tests/math_completion_tests.cpp --exhaustive). Double:
// within 0.5000001 ulp, proved (the certificate: 2^-79.9 relative before the one rounding). 2^n is exact for every
// integer n with a normal result. Owns every input: +infinity above the
// range, +0 for -infinity and for results below the smallest normal (the
// subnormal rule), NaN for NaN; a subnormal x gives 1.
[[nodiscard]] double exp2(double x) noexcept;
[[nodiscard]] float exp2(float x) noexcept;

// e^x - 1, accurate for x near 0. Float: correctly rounded for every float
// (proved as exp2's). Double: within 0.5012 ulp, proved (2^-62.8 relative before the one rounding, in the series region below 2^-5). Owns every
// input: expm1(+-0) = +-0, -1 for x below -38 and for -infinity, +infinity
// above ln(DBL_MAX) (ln(FLT_MAX) for float), NaN for NaN; a subnormal x is
// read as zero of its sign and gives that zero.
[[nodiscard]] double expm1(double x) noexcept;
[[nodiscard]] float expm1(float x) noexcept;

// log(1 + x), accurate for x near 0. Float: correctly rounded for every
// float (proved as exp2's). Double: within 0.5000001 ulp, proved (2^-75.8 relative). Owns
// every input: log1p(+-0) = +-0, log1p(-1) = -infinity, NaN below -1 and for
// NaN, +infinity for +infinity; a subnormal x is read as zero of its sign
// and gives that zero. No result is subnormal.
[[nodiscard]] double log1p(double x) noexcept;
[[nodiscard]] float log1p(float x) noexcept;

} // namespace stxlib::math
