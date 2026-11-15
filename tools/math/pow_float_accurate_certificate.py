#!/usr/bin/env python3
"""Error certificate for float pow's accurate stage (src/algorithms/math/region.cpp,
pow_float_accurate) and the composition of round, test, decide.

Float pow (region::pow(float), the owner's choice of 2026-11-15) rounds the
region double kernel unless it lies within region_pow_float_test (2^-67) of
a float midpoint, then detects exact results and midpoints exactly, and
otherwise rounds the accurate stage: double-double throughout, fixed
precision, bounded cost. This tool proves:

  1. every double-double constant of the stage is the nearest double plus
     the nearest double to the rest (so its error is known exactly);
  2. a bound on the stage's relative error against x^y, for every positive
     normal float x and normal float y with |y log2 x| <= 129 (every pair
     the stage serves): log2 x per table bucket and per piece of r = z inv -
     1 (pieces down to r's granularity for a float z), with k = 0 and |k| >=
     1 (the relative error decreases in |k|), then p = y log2 x (every term
     scales with |y|), then 2^p over |t| <= 2^-9 + 2^-40;
  3. that the bound is below region_pow_float_accurate_error, which math.hpp
     states, and that stage 1's test distance exceeds stage 1's proved
     error (tools/math/region_certificate.py `pow`, float line: 2^-69.31).

The accounting: a double-double value is (bound on |hi|, bound on |lo|,
bound on its distance from the same expression evaluated exactly on exact
inputs, constants exact); every rounding is bounded by u times the
magnitude of its result (u = 2^-53), every two-sum and two-product is exact,
and every dropped term is counted. Exact rationals (Python fractions), with
the 400-bit references of region_reference.py for ln 2, the table values
and the logarithms.

Usage: pow_float_accurate_certificate.py
"""

import math
import pathlib
import re
import sys
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import region_certificate as rc  # noqa: E402
import region_reference as reference  # noqa: E402

U = Fraction(1, 2 ** 53)
REGION_PATH = rc.MATH / 'region.cpp'
HEX = r'(-?0x[0-9a-fA-F.]+p[+-]?\d+|0\.0)'
P_LIMIT = Fraction(129)                              # |p| for every pair the stage serves
T_LIMIT = Fraction(1, 512) + Fraction(1, 2 ** 40)       # |p_hi - q/256| <= 2^-9, |p_lo| <= 2^-45
STAGE_ONE_ERROR_LOG2 = -69.31                        # region_certificate.py pow, float line
failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


def value_of(text):
    return Fraction(0) if text == '0.0' else Fraction(float.fromhex(text))


def read_constants():
    text = REGION_PATH.read_text()

    def pair(name):
        match = re.search(r'constexpr Wide ' + name + r'\{' + HEX + ', ' + HEX + r'\};', text)
        require(match is not None, 'region.cpp: ' + name + ' not found')
        return value_of(match.group(1)), value_of(match.group(2))

    def table(name, count):
        block = re.search(r'constexpr Wide ' + name + r'\[' + str(count) + r'\] = \{(.*?)\n\};', text, re.S)
        require(block is not None, 'region.cpp: ' + name + ' not found')
        rows = re.findall(r'\{' + HEX + ', ' + HEX + r'\}', block.group(1))
        require(len(rows) == count, name + ': expected %d rows' % count)
        return [(value_of(a), value_of(b)) for a, b in rows]

    def named(name):
        match = re.search(r'constexpr double ' + name + r' = (0x[0-9a-fA-F.]+p[+-]?\d+);', text)
        require(match is not None, 'region.cpp: ' + name + ' not found')
        return value_of(match.group(1))

    for spelling in ('const double r = reduction.z * entry.inv - 1.0;  // exact',
                     'series = wide_add(wide_multiply_double(series, r), pow_accurate_log_coefficients[n]);',
                     'const Wide log1p_r = wide_multiply_double(series, r);',
                     'const Wide log_z = wide_add(Wide{entry.logc_hi, entry.logc_lo}, log1p_r);',
                     'const Wide log2_z = wide_multiply(log_z, pow_accurate_inverse_ln2);',
                     'const Wide log2_x = wide_add(Wide{reduction.k, 0.0}, log2_z);',
                     'const Wide p = wide_multiply_double(log2_x, y);',
                     'const Wide u = wide_multiply(t, pow_accurate_ln2);',
                     'expm1_series = wide_add(wide_multiply(expm1_series, u), pow_accurate_exp_coefficients[n]);',
                     'const Wide expm1_u = wide_multiply(expm1_series, u);',
                     'const Wide value = wide_add(table, wide_multiply(table, expm1_u));',
                     'const double cross = a.hi * b.lo + a.lo * b.hi;',
                     'const double low = sum_error + (a.lo + b.lo);',
                     'const double low = product_error + a.lo * b;'):
        require(spelling in text, 'region.cpp no longer spells ' + spelling)
    return {
        'ln2': pair('pow_accurate_ln2'),
        'inverse_ln2': pair('pow_accurate_inverse_ln2'),
        'log': table('pow_accurate_log_coefficients', 13),
        'exp': table('pow_accurate_exp_coefficients', 9),
        'test': named('region_pow_float_test'),
        'stated': named('region_pow_float_accurate_error'),
    }


C = read_constants()
LN2 = reference.log_reference(2)
LOG_EXACT = [Fraction((-1) ** (n + 1), n) for n in range(1, 14)]
EXP_EXACT = [Fraction(1, math.factorial(n)) for n in range(1, 10)]


def nearest_pair(value):
    hi = Fraction(float(value))
    lo = Fraction(float(value - hi))
    return hi, lo


def check_constants():
    for name, exact_value in (('ln2', LN2), ('inverse_ln2', 1 / LN2)):
        require(C[name] == nearest_pair(exact_value), name + ' is not the nearest pair')
    for n in range(13):
        require(C['log'][n] == nearest_pair(LOG_EXACT[n]), 'log coefficient %d is not the nearest pair' % (n + 1))
    for n in range(9):
        require(C['exp'][n] == nearest_pair(EXP_EXACT[n]), 'exp coefficient %d is not the nearest pair' % (n + 1))
    print('step 1: every double-double constant is the nearest pair')


# ---- double-double accounting -------------------------------------------------

class DD:
    """hi: bound on |hi|; lo: bound on |lo|; error: bound on the distance of
    hi + lo from the expression evaluated exactly on exact inputs."""

    def __init__(self, hi, lo, error):
        self.hi = Fraction(hi)
        self.lo = Fraction(lo)
        self.error = Fraction(error)

    def magnitude(self):
        return self.hi + self.lo


def exact_double(magnitude):
    return DD(magnitude, 0, 0)


def constant_pair(pair, exact_value):
    hi, lo = pair
    return DD(abs(hi), abs(lo), abs(hi + lo - exact_value))


def normalized(value_bound, error):
    """A two-sum's result: |hi| <= |s + low| (1 + u), |lo| <= u |hi|."""
    hi = value_bound * (1 + U)
    return DD(hi, U * hi, error)


def wide_add(a, b):
    s = (a.hi + b.hi) * (1 + U)
    e = U * s
    lows = a.lo + b.lo
    t1 = lows * (1 + U)
    low = (e + t1) * (1 + U)
    error = a.error + b.error + U * lows + U * (e + t1)
    return normalized(s + low, error)


def wide_multiply(a, b):
    p = a.hi * b.hi * (1 + U)
    pe = U * p
    c1 = a.hi * b.lo
    c2 = a.lo * b.hi
    cross = (c1 + c2) * (1 + U) ** 2
    low = (pe + cross) * (1 + U)
    error = (U * c1 + U * c2 + U * (c1 + c2) * (1 + U) + U * (pe + cross) + a.lo * b.lo +
             a.magnitude() * b.error + b.magnitude() * a.error + a.error * b.error)
    return normalized(p + low, error)


def wide_multiply_double(a, d):
    """d an exact double with |d| <= d."""
    p = a.hi * d * (1 + U)
    pe = U * p
    t = a.lo * d
    low = (pe + t * (1 + U)) * (1 + U)
    error = U * t + U * (pe + t * (1 + U)) + d * a.error
    return normalized(p + low, error)


# ---- step 2: the log side -----------------------------------------------------

LOG_DD = [constant_pair(C['log'][n], LOG_EXACT[n]) for n in range(13)]
INVERSE_LN2_DD = constant_pair(C['inverse_ln2'], 1 / LN2)


def log2_error(i, r_low, r_high):
    """(error of log2 x with k = 0, with k = 1) over r in [r_low, r_high] of
    bucket i: absolute bounds, against log2 x exactly."""
    inv, logc_hi, logc_lo, _, _ = rc.REGION[i]
    m = rc.magnitude(r_low, r_high)
    series = LOG_DD[12]
    for n in range(11, -1, -1):
        series = wide_add(wide_multiply_double(series, m), LOG_DD[n])
    log1p_r = wide_multiply_double(series, m)
    if m > 0:
        # The series against log1p(r): the terms from r^14 on.
        log1p_r.error += m ** 14 / 14 / (1 - m)
    logc_true = -reference.log_reference(inv)
    logc = DD(abs(logc_hi), abs(logc_lo), abs(logc_hi + logc_lo - logc_true) + Fraction(1, 2 ** 380))
    log_z = wide_add(logc, log1p_r)
    log2_z = wide_multiply(log_z, INVERSE_LN2_DD)
    k0 = wide_add(exact_double(0), log2_z)
    k1 = wide_add(exact_double(1), log2_z)
    return k0, k1


def log2_floor(i, r_low, r_high, k):
    """min |log2 x| over the piece (k = 0 or |k| = 1)."""
    return rc.float_log_floor(i, r_low, r_high, k, k) / LN2


def dyadic_pieces(pieces):
    """Splits every piece that does not hold 0 until |r_high| <= 2 |r_low|:
    the bounds take |r| at its largest and the floor at its smallest."""
    result = []
    for low, high in pieces:
        if low < 0 < high or low == high:
            result.append((low, high))
            continue
        sign = 1 if high > 0 else -1
        a, b = sorted((abs(low), abs(high)))
        while b > 2 * a:
            result.append(tuple(sorted((sign * (b / 2), sign * b))))
            b = b / 2
        result.append(tuple(sorted((sign * a, sign * b))))
    return result


def log_side():
    """The worst relative error of p = y log2 x over every bucket and piece."""
    worst = Fraction(0)
    worst_at = None
    for i in range(128):
        inv = rc.REGION[i][0]
        pieces = dyadic_pieces(rc.float_r_subintervals(i, inv))
        if inv == 1:
            # r = 0: x = 2^k, k != 0 (x = 1 never reaches the stage).
            pieces = pieces + [(Fraction(0), Fraction(0))]
        for r_low, r_high in pieces:
            k0, k1 = log2_error(i, r_low, r_high)
            for k, log2_x in ((0, k0), (1, k1)):
                if k == 0 and r_low == r_high == 0:
                    continue
                floor = log2_floor(i, r_low, r_high, k)
                require(floor > 0, 'log2 x has no floor in bucket %d, k = %d' % (i, k))
                # p = log2_x * y, |y| scaled to 1: every term scales with |y|.
                p = wide_multiply_double(log2_x, Fraction(1))
                relative = p.error / floor
                if relative > worst:
                    worst = relative
                    worst_at = (i, float(r_low), float(r_high), k)
    print('step 2 (log): p = y log2 x within 2^%.2f of itself (worst: bucket %d, r in [%.3g, %.3g], k = %d)'
          % (rc.log2_of(worst), worst_at[0], worst_at[1], worst_at[2], worst_at[3]))
    return worst


# ---- step 2: the exp side ------------------------------------------------------

def exp_side():
    """The worst relative error of (hi + lo) against 2^(j/256) e^(t ln 2),
    t the computed p - q/256 (exact)."""
    t = DD(T_LIMIT, U * T_LIMIT, 0)
    u = wide_multiply(t, constant_pair(C['ln2'], LN2))
    u_max = u.magnitude() + u.error
    exp_dd = [constant_pair(C['exp'][n], EXP_EXACT[n]) for n in range(9)]
    series = exp_dd[8]
    for n in range(7, -1, -1):
        series = wide_add(wide_multiply(series, u), exp_dd[n])
    expm1_u = wide_multiply(series, u)
    # e^u - 1 against the polynomial: the terms from u^10 on.
    expm1_u.error += u_max ** 10 / math.factorial(10) / (1 - u_max)
    table = DD(2, 2 * U, rc.EXP_TABLE_ERROR)
    value = wide_add(table, wide_multiply(table, expm1_u))
    floor = 1 - u_max                                   # 2^(j/256) >= 1, e^u >= 1 - |u|
    relative = value.error / floor
    print('step 2 (exp): 2^(j/256) e^u within 2^%.2f of itself (|t| <= 2^-9 + 2^-40)' % rc.log2_of(relative))
    return relative


def main(arguments):
    check_constants()
    log_relative = log_side()
    exp_relative = exp_side()
    delta_p = log_relative * P_LIMIT
    # 2^(p + delta) = 2^p (1 + epsilon) with |epsilon| <= 2^|delta| - 1 <= |delta| ln 2 (1 + |delta|).
    from_p = delta_p * LN2 * (1 + delta_p)
    total = (1 + exp_relative) * (1 + from_p) - 1
    print('step 2: the accurate stage within 2^%.2f of x^y for |p| <= 129' % rc.log2_of(total))
    require(total <= C['stated'], 'the accurate stage exceeds region_pow_float_accurate_error')
    print('step 3: region_pow_float_accurate_error = 2^%d holds' % round(rc.log2_of(C['stated'])))
    require(rc.log2_of(C['test']) > STAGE_ONE_ERROR_LOG2,
            'stage 1 test distance below its proved error')
    print('step 3: stage 1 test distance 2^%d exceeds its proved error 2^%.2f' % (round(rc.log2_of(C['test'])),
                                                                              STAGE_ONE_ERROR_LOG2))
    if failures:
        print('%d failure(s)' % len(failures))
        return 1
    print('float pow accurate stage certificate holds')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
