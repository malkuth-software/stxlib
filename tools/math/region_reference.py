#!/usr/bin/env python3
"""Exact references for the region kernels (src/algorithms/math/region*.{hpp,cpp}).

Natural logarithm, exponential and powers of rationals in big-integer fixed
point, with a stated error bound, for three uses: generating the region
tables (tools/math/gen_region_tables.py), proving the kernels' error bounds
(tools/math/region_certificate.py), and deciding the correctly rounded
float wherever an exhaustive screen cannot (tools/math/region_exhaustive.py).

Only the Python standard library (integers and fractions). Every function
returns an exact Fraction within a stated absolute or relative distance of
the true value; no binary floating point is used in any computed value.

  region_reference.py --self-test   checks the identities below and exits
"""

import sys
from fractions import Fraction

# Working precision: values are integers scaled by 2^PRECISION. Each series
# below truncates every term toward zero, so its error is at most one unit
# (2^-PRECISION) per term plus the series tail; with fewer than 400 terms
# that stays below 2^-(PRECISION - 10).
PRECISION = 400
ONE = 1 << PRECISION
# A bound on |computed - true| for ln2_fixed and log_fixed, in units of
# 2^-PRECISION (counted below: terms, tails and the final combinations).
FIXED_ERROR_UNITS = 1 << 12


def to_fixed(value):
    """floor(value * 2^PRECISION) for a Fraction (or int)."""
    value = Fraction(value)
    return (value.numerator << PRECISION) // value.denominator


def atanh_inverse_fixed(n):
    """atanh(1/n) for an integer n >= 2, in fixed point."""
    total = 0
    power = ONE // n          # 1/n^(2k+1), truncated
    n_squared = n * n
    k = 0
    while power != 0:
        total += power // (2 * k + 1)
        power //= n_squared
        k += 1
    return total


_ln2_cache = []


def ln2_fixed():
    """ln 2 = 2 atanh(1/3), in fixed point (error below 2^-(PRECISION-8))."""
    if not _ln2_cache:
        _ln2_cache.append(2 * atanh_inverse_fixed(3))
    return _ln2_cache[0]


def atanh_fixed(w):
    """atanh(w) for a fixed-point w with |w| <= 1/2 (series in w^2)."""
    total = 0
    w_squared = (w * w) >> PRECISION
    power = w
    k = 0
    while power != 0:
        total += power // (2 * k + 1)
        power = (power * w_squared) >> PRECISION
        k += 1
        if abs(power) <= 1:
            break
    return total


def log_fixed(value):
    """ln(value) for a positive Fraction, in fixed point: value = 2^e m with m
    in [2/3, 4/3), ln m = 2 atanh((m - 1)/(m + 1)) (|w| <= 1/7)."""
    value = Fraction(value)
    if value <= 0:
        raise ValueError('log of a non-positive value')
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    m = value / Fraction(2) ** exponent
    while m >= Fraction(4, 3):
        m /= 2
        exponent += 1
    while m < Fraction(2, 3):
        m *= 2
        exponent -= 1
    w = to_fixed((m - 1) / (m + 1))
    return exponent * ln2_fixed() + 2 * atanh_fixed(w)


def log_reference(value):
    """ln(value) as a Fraction within 2^-(PRECISION - 12) absolute (exponent
    of value below 2^20 in magnitude)."""
    return Fraction(log_fixed(value), ONE)


def exp_fixed(v):
    """e^v for a fixed-point v (|v| < 2^20), as (mantissa_fixed, e) with
    e^v ~ mantissa_fixed * 2^-PRECISION * 2^e and mantissa in [1/2, 2):
    v = e ln2 + f, |f| <= ln2/2 + tiny, e^f by its Taylor series."""
    ln2 = ln2_fixed()
    e = (v + ln2 // 2) // ln2
    f = v - e * ln2
    total = ONE
    term = ONE
    n = 1
    while abs(term) > 1:
        term = ((term * f) >> PRECISION) // n
        total += term
        n += 1
    return total, e


def exp_reference(value):
    """e^value for a Fraction value (|value| < 2^19), as a Fraction with
    relative error below 2^-(PRECISION - 40)."""
    mantissa, e = exp_fixed(to_fixed(value))
    return Fraction(mantissa, ONE) * Fraction(2) ** e


def pow_reference(x, y):
    """x^y = e^(y ln x) for positive rational x and rational y with |y ln x|
    < 2^19, relative error below 2^-(PRECISION - 120) (the product's error
    grows with |y| < 2^64)."""
    x = Fraction(x)
    y = Fraction(y)
    v = log_fixed(x) * y          # exact product of fixed point and rational
    v_fixed = v.numerator // v.denominator
    mantissa, e = exp_fixed(v_fixed)
    return Fraction(mantissa, ONE) * Fraction(2) ** e


def self_test():
    failures = 0
    # ln 2 against the known decimal expansion (first 60 digits).
    known = Fraction(693147180559945309417232121458176568075500134360255254120680,
                     10 ** 60)
    if abs(Fraction(ln2_fixed(), ONE) - known) > Fraction(1, 10 ** 59):
        print('FAIL: ln 2')
        failures += 1
    # ln(a b) = ln a + ln b, ln(2^k) = k ln 2.
    for a, b in ((Fraction(3), Fraction(5, 7)), (Fraction(1001, 1000), Fraction(999, 1000)),
                 (Fraction(123456789, 2 ** 20), Fraction(2 ** 40, 987654321))):
        left = log_fixed(a * b)
        right = log_fixed(a) + log_fixed(b)
        if abs(left - right) > 3 * FIXED_ERROR_UNITS:
            print('FAIL: ln(ab) = ln a + ln b for', a, b)
            failures += 1
    if abs(log_fixed(Fraction(2) ** 37) - 37 * ln2_fixed()) > FIXED_ERROR_UNITS:
        print('FAIL: ln 2^37')
        failures += 1
    # e^(ln v) = v.
    for v in (Fraction(3), Fraction(1, 7), Fraction(10 ** 20), Fraction(2 ** 60 + 1, 2 ** 30)):
        back = exp_reference(Fraction(log_fixed(v), ONE))
        if abs(back / v - 1) > Fraction(1, 2 ** (PRECISION - 60)):
            print('FAIL: e^(ln v) = v for', v)
            failures += 1
    # 3^5 and 10^22 exactly (to the reference's precision).
    for x, y, exact in ((3, 5, 243), (10, 22, 10 ** 22), (Fraction(1, 4), Fraction(1, 2), Fraction(1, 2))):
        value = pow_reference(x, y)
        if abs(value / exact - 1) > Fraction(1, 2 ** (PRECISION - 140)):
            print('FAIL: pow', x, y)
            failures += 1
    print('region_reference self-test: %s' % ('FAIL' if failures else 'ok'))
    return failures == 0


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        sys.exit(0 if self_test() else 1)
    print(__doc__)
    sys.exit(2)
