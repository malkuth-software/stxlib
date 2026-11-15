#!/usr/bin/env python3
"""Error bounds of exp2, expm1 and log1p (src/algorithms/math/completions.cpp
and completions_core.hpp; roadmap R1), proved by interval error analysis
over exact rationals, as tools/math/region_certificate.py proves the
region kernels it builds on (its Value arithmetic, tables and references
are reused here).

Each step prints the relative error of the double-double before its one
rounding, the double bound in ulp (0.5 + relative 2^53), and the float
screen bound that tests/math_completion_tests.cpp --exhaustive checks
every float against. It fails unless every double bound is at most 0.51 ulp
(owner decision D10) and every float screen bound at most 2^-60.

Usage:
  r1_certificate.py [exp2|expm1|log1p|all]
"""

import math
import pathlib
import sys
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import region_certificate as rc  # noqa: E402
import region_reference as reference  # noqa: E402

U = rc.U
Value = rc.Value
add = rc.add
multiply = rc.multiply
constant = rc.constant
symmetric = rc.symmetric
half_ulp = rc.half_ulp
log2_of = rc.log2_of
require = rc.require

D10_DOUBLE = Fraction(51, 100)
FLOAT_SCREEN_LIMIT = Fraction(1, 2 ** 60)
LN2 = rc.LN2


def ulps(relative):
    return Fraction(1, 2) + relative * 2 ** 53 * (1 + U)


def report(name, relative, float_relative=None):
    double_ulps = ulps(relative)
    print('  %s(double): 2^%.2f relative before rounding, %.7f ulp' % (name, log2_of(relative), float(double_ulps)))
    require(double_ulps <= D10_DOUBLE, '%s(double) above 0.51 ulp' % name)
    if float_relative is not None:
        print('  %s(float): screen bound 2^%.2f relative' % (name, log2_of(float_relative)))
        require(float_relative <= FLOAT_SCREEN_LIMIT, '%s(float) screen bound above 2^-60' % name)


# ---- exp2 ------------------------------------------------------------------

_exp_relative = []


def exp_relative():
    """region_exp2's relative error over every table index and |t| <=
    2^-9 + 2^-40 (t_lo bounded by half an ulp of t, as two_sum leaves it;
    exp2 passes p_lo = 0, a special case)."""
    if not _exp_relative:
        worst, _ = rc.exp_relative('region')
        _exp_relative.append(worst)
    return _exp_relative[0]


def exp2_step():
    print('== exp2')
    relative = exp_relative()
    # Double: head + tail rounded once, then scaled exactly (fast path) or
    # by scale_wide (once, on overflow or the subnormal grid). Float: head
    # and tail scaled exactly (exponent in [-150, 128]), rounded once.
    report('exp2', relative, relative)
    return relative


# ---- expm1 -----------------------------------------------------------------

EXPM1_COEFFICIENTS = {k: Fraction(1.0 / math.factorial(k)) for k in range(3, 10)}


def expm1_series_error(x_low, x_high):
    """Absolute error of expm1_series' hi + lo against e^x - 1 for x in
    [x_low, x_high] (one sign, 2^-54 <= |x| < 2^-5)."""
    x = Value(x_low, x_high)
    m = x.magnitude()
    square_low = min(x_low * x_low, x_high * x_high)
    square = rc.rounded(square_low, m * m, Fraction(0))      # sq; its error vs x^2
    # sq + sq_error = x^2 exactly; head + head_error = x + sq/2 exactly
    # (fast two-sum, |x| >= x^2/2).
    require(m < 2, 'expm1 series: fast_two_sum precondition')
    square_error = symmetric(half_ulp(m * m))
    poly = constant(EXPM1_COEFFICIENTS[9])
    for k in range(8, 2, -1):
        poly = add(constant(EXPM1_COEFFICIENTS[k]), multiply(x, poly))
    cubic = multiply(multiply(x, square), poly)
    head_m = m + m * m / 2
    head_error = symmetric(half_ulp(head_m))
    lo = add(head_error, add(rc.scale(square_error, Fraction(1, 2)), cubic))
    # Model: the exact series to x^9/9! with the stored coefficients, against
    # e^x - 1.
    truncation = m ** 10 / math.factorial(10) / (1 - m)
    coefficients = sum(m ** k * abs(EXPM1_COEFFICIENTS[k] - Fraction(1, math.factorial(k))) for k in range(3, 10))
    # hi + lo is then rounded once (double) or to float.
    return lo.error + truncation + coefficients


def expm1_floor(x_low, x_high):
    """min |e^x - 1| over [x_low, x_high] (one sign): at the end nearer 0."""
    nearer = x_low if abs(x_low) < abs(x_high) else x_high
    return abs(reference.exp_reference(nearer) - 1) * (1 - Fraction(1, 2 ** 300))


def geometric_pieces(low_exponent, high_exponent, per_binade=4):
    """[a, b] pieces covering [2^low_exponent, 2^high_exponent]."""
    pieces = []
    for e in range(low_exponent, high_exponent):
        base = Fraction(2) ** e
        step = base / per_binade
        for n in range(per_binade):
            pieces.append((base + step * n, base + step * (n + 1)))
    return pieces


def expm1_series_relative():
    worst = Fraction(0)
    for low, high in geometric_pieces(-54, -5):
        for sign in (1, -1):
            a, b = (low, high) if sign > 0 else (-high, -low)
            ratio = expm1_series_error(a, b) / expm1_floor(a, b)
            worst = max(worst, ratio)
    return worst


def tail_bound():
    """|tail| of region_exp2: below 2^-40 (its contract); used for the
    rounding of difference_error + scaled_tail."""
    return Fraction(1, 2 ** 40)


def conversion_error(x_magnitude):
    """|p + p_lo - x / ln 2| for |x| <= x_magnitude: p + p_error = x inv_hi
    exactly; x inv_lo and the sum round; inv_hi + inv_lo against 1/ln 2."""
    inverse_hi = rc.RC['region_inverse_ln2_hi']
    inverse_lo = rc.RC['region_inverse_ln2_lo']
    p_error_bound = half_ulp(x_magnitude * inverse_hi)
    product = x_magnitude * abs(inverse_lo)
    return (half_ulp(product) + half_ulp(p_error_bound + product) +
            x_magnitude * abs(inverse_hi + inverse_lo - rc.INVERSE_LN2))


def expm1_exp_relative(high):
    """expm1_exp for 2^-5 <= |x| <= high and x >= -38: e^x's error times
    e^x / |e^x - 1|, which is largest at |x| = 2^-5 on both sides, plus the
    roundings of lo, relative to |e^x - 1|."""
    relative_exp = exp_relative()
    # 2^(p + p_lo) against e^x: |p + p_lo - x/ln2| ln 2 relative (times e^d).
    conversion = conversion_error(high) * LN2 * 2
    worst = Fraction(0)
    for x in (Fraction(1, 32), Fraction(-1, 32)):
        e_x = reference.exp_reference(x)
        amplification = e_x / abs(e_x - 1)
        # lo = difference_error + scaled_tail: |difference_error| <= ulp/2 of
        # |e^x - 1| (about), |scaled_tail| <= 2^-40 e^x (2.003 2^exponent).
        lo_m = half_ulp(abs(e_x - 1) * 2) + tail_bound() * e_x * 2
        lo_rounding = half_ulp(lo_m)
        ratio = (relative_exp + conversion) * amplification * (1 + Fraction(1, 2 ** 40)) + \
            lo_rounding / abs(e_x - 1)
        worst = max(worst, ratio)
    return worst


def expm1_step():
    print('== expm1')
    series = expm1_series_relative()
    print('  series region (2^-54 <= |x| < 2^-5): 2^%.2f' % log2_of(series))
    exp_double = expm1_exp_relative(Fraction(45))
    print('  exp region (2^-5 <= |x|, -38 <= x <= 45): 2^%.2f' % log2_of(exp_double))
    # x > 45: scale_wide(e^x) against e^x - 1: (eps e^x + 1) / (e^x - 1).
    e45 = reference.exp_reference(Fraction(45))
    large = (exp_relative() + conversion_error(Fraction(710)) * LN2 * 2) * e45 / (e45 - 1) + 1 / (e45 - 1)
    print('  large region (45 < x <= 709.79): 2^%.2f' % log2_of(large))
    # x < -38: -1, against e^x - 1 = -1 + e^x with e^x < e^-38: the error is
    # below half the spacing (2^-53) above -1, so -1 is correctly rounded.
    below = reference.exp_reference(Fraction(-38))
    require(below < Fraction(1, 2 ** 54), 'expm1: e^-38 not below 2^-54')
    print('  x < -38: -1 correctly rounded (e^-38 = 2^%.2f < 2^-54)' % log2_of(below))
    double_relative = max(series, exp_double, large)
    float_relative = max(series, expm1_exp_relative(Fraction(89)))
    report('expm1', double_relative, float_relative)
    return double_relative


# ---- log1p -----------------------------------------------------------------

W_RELATIVE = Fraction(1, 2 ** 52)     # |w| <= inv ulp(u)/2 2^-k <= 2^-52.9 (1 + r): widen r by 2^-52


def log1p_pieces(i, inv):
    """Pieces of r + r_lo + w over bucket i with k = 0, none containing 0;
    in bucket 80 down to |x| = 2^-54 (the fast path's floor)."""
    if inv != 1:
        pieces = []
        for low, high in rc.r_subintervals(i, inv):
            pieces.append((low - W_RELATIVE, high + W_RELATIVE))
        return pieces
    pieces = []
    for low, high in rc.r_subintervals(i, inv):
        pieces.append((low, high))
    # r_subintervals stops at 2^-53 in bucket 80; add [2^-54, 2^-53] each side.
    finest = Fraction(1, 2 ** 53)
    pieces.append((finest / 2, finest))
    pieces.append((-finest, -finest / 2))
    return pieces


def log1p_error(i, d_low, d_high, k_low, k_high):
    """Absolute error of log1p_wide's hi + lo against log(1 + x) for r + r_lo
    + w in [d_low, d_high] and k in [k_low, k_high]."""
    inv, logc_hi, logc_lo, log2c_hi, log2c_lo = rc.REGION[i]
    h_bound, low, model, rho = rc.region_log1p(d_low, d_high, inv)
    a_m = h_bound + low.magnitude()
    a_lo_bound = half_ulp(a_m)
    argument_error = Fraction(0)
    if inv != 1:
        # w = (u_lo 2^-k) inv rounds once; r_head_lo + w rounds once; the
        # clamp at k > 1000 changes w by below 2^-999.
        w_m = inv * Fraction(1, 2 ** 53) * (1 + abs(d_high) + abs(d_low))
        argument_error = half_ulp(w_m) + half_ulp(w_m + half_ulp(1)) + Fraction(1, 2 ** 990)
    else:
        argument_error = Fraction(1, 2 ** 990)
    m = rc.magnitude(d_low, d_high)
    log1p_error_of_argument = argument_error / (1 - m - argument_error)
    k = Value(k_low, k_high)
    km = rc.magnitude(k_low, k_high)
    ln2_hi = rc.RC['region_ln2_hi']
    ln2_lo = rc.RC['region_ln2_lo']
    b_m = km * ln2_hi + abs(logc_hi)
    c_m = b_m + a_m
    e1 = symmetric(half_ulp(b_m))
    e2 = symmetric(half_ulp(c_m))
    lo = add(add(e1, e2), add(add(constant(logc_lo), multiply(k, constant(ln2_lo))), symmetric(a_lo_bound)))
    logc_true = -reference.log_reference(inv)
    error = (lo.error + abs(logc_hi + logc_lo - logc_true) + km * abs(ln2_hi + ln2_lo - LN2) +
             low.error + model + log1p_error_of_argument)
    return error


def log1p_floor(i, d_low, d_high, k_low, k_high):
    """min |log(1 + x)| = min |k ln 2 + ln(z (1 + u_lo/u))| over the piece."""
    inv = rc.REGION[i][0]
    if k_low == k_high == 0:
        z_low = (1 + d_low) / inv
        z_high = (1 + d_high) / inv
        return rc.true_log2_bounds(None, z_low, z_high) * LN2
    first, last = rc.bucket_range(i)
    ln_z = max(abs(reference.log_reference(first)), abs(reference.log_reference(last))) + Fraction(1, 2 ** 50)
    return min(abs(k_low), abs(k_high)) * LN2 - ln_z


K_GROUPS = [(1, 1), (2, 3), (4, 7), (8, 15), (16, 31), (32, 63), (64, 127), (128, 255), (256, 511), (512, 1024),
            (-1, -1), (-3, -2), (-7, -4), (-15, -8), (-31, -16), (-53, -32)]


def log1p_preconditions(i):
    """fast_two_sum(k ln2_hi, logc_hi) and fast_two_sum(b, a): |a| <= the
    log1p of the bucket's largest |r + w|."""
    inv, logc_hi = rc.REGION[i][0], rc.REGION[i][1]
    first, last = rc.bucket_range(i)
    m = max(abs(first * inv - 1), abs(last * inv - 1)) + W_RELATIVE
    a_max = m / (1 - m)                  # |log1p(r)| <= |r| / (1 - |r|)
    require(rc.RC['region_ln2_hi'] >= abs(logc_hi), 'log1p: |ln2_hi| < |logc_hi| in bucket %d' % i)
    require(rc.RC['region_ln2_hi'] - abs(logc_hi) >= a_max, 'log1p: |b| < |a| (k != 0) in bucket %d' % i)
    if inv != 1:
        require(abs(logc_hi) >= a_max, 'log1p: |logc_hi| < |a| (k = 0) in bucket %d' % i)


def log1p_step():
    print('== log1p')
    worst_double = (Fraction(0), None)
    worst_float = (Fraction(0), None)
    for i in range(128):
        inv = rc.REGION[i][0]
        log1p_preconditions(i)
        first, last = rc.bucket_range(i)
        full = (first * inv - 1 - W_RELATIVE, last * inv - 1 + W_RELATIVE)
        cases = [(low, high, 0, 0) for low, high in log1p_pieces(i, inv)]
        cases += [(full[0], full[1], k_low, k_high) for k_low, k_high in K_GROUPS]
        for d_low, d_high, k_low, k_high in cases:
            floor = log1p_floor(i, d_low, d_high, k_low, k_high)
            require(floor > 0, 'log1p: |log(1 + x)| not bounded below in bucket %d' % i)
            ratio = log1p_error(i, d_low, d_high, k_low, k_high) / floor
            if ratio > worst_double[0]:
                worst_double = (ratio, i)
            # Float x: u >= 2^-24 and u < 2^128.5, so k in [-24, 128].
            if k_low <= 128 and k_high >= -24:
                if ratio > worst_float[0]:
                    worst_float = (ratio, i)
    print('  worst bucket %s' % worst_double[1])
    report('log1p', worst_double[0], worst_float[0])
    return worst_double[0]


def main(arguments):
    step = arguments[0] if arguments else 'all'
    if step not in ('exp2', 'expm1', 'log1p', 'all'):
        print(__doc__)
        return 2
    if step in ('exp2', 'all'):
        exp2_step()
    if step in ('expm1', 'all'):
        expm1_step()
    if step in ('log1p', 'all'):
        log1p_step()
    print('all checks hold')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
