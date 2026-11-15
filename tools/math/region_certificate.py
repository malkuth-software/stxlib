#!/usr/bin/env python3
"""Error bounds of the region kernels (src/algorithms/math/region_core.hpp,
region.cpp), and the same accounting for the kernels they would replace.

Interval error analysis in the manner of tools/math/sincos_certificate.py:
every computed double is a Value, an interval holding it plus a bound on its
distance from the same expression evaluated exactly on the same inputs;
every rounding is counted (round to nearest, half an ulp of the largest
magnitude the result can have), every truncation and every table or
constant error is bounded from the exact references of
tools/math/region_reference.py. Operations the kernels perform exactly
(cut products, fast two-sums, exact differences) are listed with the bit
counts that make them exact.

Steps:
  pow   double pow: the relative error of log2 x per log bucket (the part
        that |y| multiplies), the relative error of 2^t per exp-table
        index, and the composed bound in ulps, for the current kernel
        (src/algorithms/math/pow.cpp, measured here for comparison) and for
        the region kernel.
  logf  float log, log2, log10: the absolute error bound of the region
        kernel's double-double per bucket, as a relative bound the
        exhaustive screen (tools/math/region_exhaustive.py) uses.

Usage:
  region_certificate.py pow    print the pow budget; exit 1 on a failed check
  region_certificate.py logf   print the float log bounds
  region_certificate.py all

Only the Python standard library; no binary floating point in any bound
(floats appear only as exact stored constants and in printed summaries).
"""

import math
import pathlib
import re
import struct
import sys
from fractions import Fraction

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import region_reference as reference  # noqa: E402

ROOT = HERE.parent.parent
MATH = ROOT / 'src' / 'algorithms' / 'math'

U = Fraction(1, 2 ** 53)
failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


def log2_of(value):
    value = abs(Fraction(value))
    if value == 0:
        return float('-inf')
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    return exponent + math.log2(float(value / Fraction(2) ** exponent))


def bits_of_double(value):
    return struct.unpack('<Q', struct.pack('<d', float(value)))[0]


def double_of_bits(bits):
    return Fraction(struct.unpack('<d', struct.pack('<Q', bits))[0])


def significant_bits(value):
    value = abs(Fraction(value))
    if value == 0:
        return 0
    numerator = value.numerator
    trailing = (numerator & -numerator).bit_length() - 1
    return (numerator >> trailing).bit_length()


# ---- Values ---------------------------------------------------------------

def magnitude(low, high):
    return max(abs(low), abs(high))


def half_ulp(m):
    """A bound on |fl(v) - v| for every |v| <= m (normal range)."""
    m = Fraction(m)
    if m == 0:
        return Fraction(0)
    exponent = m.numerator.bit_length() - m.denominator.bit_length()
    if Fraction(2) ** exponent > m:
        exponent -= 1
    return Fraction(2) ** (exponent - 53)


class Value:
    """A computed double: [low, high] holds it; error bounds its distance from
    the same expression evaluated exactly."""

    def __init__(self, low, high, error=Fraction(0)):
        self.low = Fraction(low)
        self.high = Fraction(high)
        self.error = Fraction(error)

    def magnitude(self):
        return magnitude(self.low, self.high)

    def __repr__(self):
        return 'Value([%.3g, %.3g], err 2^%.1f)' % (float(self.low), float(self.high), log2_of(self.error))


def constant(value):
    value = Fraction(value)
    return Value(value, value)


def symmetric(bound, error=Fraction(0)):
    return Value(-bound, bound, error)


def rounded(low, high, error):
    widen = half_ulp(magnitude(low, high))
    return Value(low - widen, high + widen, error + widen)


def add(a, b):
    return rounded(a.low + b.low, a.high + b.high, a.error + b.error)


def subtract(a, b):
    return rounded(a.low - b.high, a.high - b.low, a.error + b.error)


def multiply(a, b):
    products = (a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high)
    error = a.magnitude() * b.error + b.magnitude() * a.error + a.error * b.error
    return rounded(min(products), max(products), error)


def exact_multiply(a, b):
    """A product the kernel forms exactly (its bit count is checked where it
    is used)."""
    products = (a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high)
    error = a.magnitude() * b.error + b.magnitude() * a.error + a.error * b.error
    return Value(min(products), max(products), error)


def negate(a):
    return Value(-a.high, -a.low, a.error)


def scale(a, factor):
    """a times a power of two (exact)."""
    factor = Fraction(factor)
    if factor > 0:
        return Value(a.low * factor, a.high * factor, a.error * factor)
    return Value(a.high * factor, a.low * factor, a.error * -factor)


# ---- constants ------------------------------------------------------------

HEX = r'(-?0x[0-9a-fA-F.]+p[+-]?\d+)'


def exact(text):
    return Fraction(float.fromhex(text))


def named_constant(name, text):
    match = re.search(r'inline constexpr double ' + name + r' = ' + HEX + ';', text)
    require(match is not None, 'constant %s not found' % name)
    return exact(match.group(1))


def read_tables():
    region_text = (MATH / 'region_log_table128.hpp').read_text()
    rows = re.findall(r'\{' + HEX + ', ' + HEX + ', ' + HEX + ', ' + HEX + ', ' + HEX + r'\}', region_text)
    require(len(rows) == 128, 'region_log_table128.hpp: expected 128 entries')
    region = [tuple(exact(v) for v in row) for row in rows]
    names = ('region_third_hi', 'region_third_lo', 'region_ln2_1', 'region_ln2_2', 'region_ln2_3',
             'region_inverse_ln2_hi', 'region_inverse_ln2_lo', 'region_inverse_ln2_cut',
             'region_inverse_ln2_rest', 'region_inverse_ln10_cut', 'region_inverse_ln10_rest',
             'region_inverse_ln10_hi',
             'region_ln2_hi', 'region_ln2_lo', 'region_log10_2_hi', 'region_log10_2_lo')
    region_constants = {name: named_constant(name, region_text) for name in names}
    log_text = (MATH / 'log_table128.hpp').read_text()
    rows = re.findall(r'\{' + HEX + ', ' + HEX + ', ' + HEX + r'\}', log_text)
    require(len(rows) == 128, 'log_table128.hpp: expected 128 entries')
    log_table = [tuple(exact(v) for v in row) for row in rows]
    log_constants = {name: named_constant(name, log_text)
                     for name in ('log_inv_ln2_hi', 'log_inv_ln2_lo')}
    exp_text = (MATH / 'exp_table256.hpp').read_text()
    rows = re.findall(r'\{' + HEX + ', ' + HEX + r'\}', exp_text)
    exp_rows = [(Fraction(1), Fraction(0))] + [tuple(exact(v) for v in row) for row in rows[1:]]
    require(len(exp_rows) == 256, 'exp_table256.hpp: expected 256 entries')
    return region, region_constants, log_table, log_constants, exp_rows


REGION, RC, LOG_TABLE, LOG_CONSTANTS, EXP_TABLE = read_tables()


def exp_table_error():
    """max over j of |T_hi + T_lo - 2^(j/256)| (T in [1, 2))."""
    ln2 = reference.log_reference(2)
    worst = Fraction(0)
    for j, (hi, lo) in enumerate(EXP_TABLE):
        exact_value = reference.exp_reference(Fraction(j, 256) * ln2)
        worst = max(worst, abs(hi + lo - exact_value))
    return worst + Fraction(1, 2 ** 300)


EXP_TABLE_ERROR = exp_table_error()
LOG_OFFSET_BITS = 0x3fe6000000000000 - (1 << 44)
BUCKET_BITS = 1 << 45
LN2 = reference.log_reference(2)
INVERSE_LN2 = 1 / LN2
# Each coefficient spelled 1.0 / n in the kernels: the nearest double.
LOG_COEFFICIENTS = {k: Fraction((1.0 if k % 2 == 1 else -1.0) / k) for k in range(3, 11)}
EXP_COEFFICIENTS = {k: Fraction(1.0 / math.factorial(k)) for k in range(2, 8)}
EXACT_LN2_CONSTANTS = (Fraction(float.fromhex('0x1.62e42fefa39efp-1')), Fraction(float.fromhex('0x1.abc9e3b39803fp-56')))


def bucket_range(i):
    first = double_of_bits(LOG_OFFSET_BITS + i * BUCKET_BITS)
    last = double_of_bits(LOG_OFFSET_BITS + (i + 1) * BUCKET_BITS - 1)
    return first, last


def cut(value, bits):
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    m = abs(value)
    exponent = m.numerator.bit_length() - m.denominator.bit_length()
    if Fraction(2) ** exponent > m:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - bits + 1)
    kept = (m // quantum) * quantum
    return kept if value > 0 else -kept


def nearest_double(value):
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    return Fraction(value.numerator / value.denominator)


def current_log2_table():
    """pow.cpp's make_log2_table, evaluated as its constant evaluation
    rounds (Python floats are IEEE doubles, rounded to nearest)."""
    inverse_hi = float(LOG_CONSTANTS['log_inv_ln2_hi'])
    inverse_lo = float(LOG_CONSTANTS['log_inv_ln2_lo'])

    def split(a):
        c = a * 134217729.0
        hi = c - (c - a)
        return hi, a - hi
    i_hi, i_lo = split(inverse_hi)
    table = []
    for inv, logc_hi, logc_lo in LOG_TABLE:
        logc_hi = float(logc_hi)
        logc_lo = float(logc_lo)
        c_hi, c_lo = split(logc_hi)
        product = logc_hi * inverse_hi
        product_error = ((c_hi * i_hi - product) + c_hi * i_lo + c_lo * i_hi) + c_lo * i_lo
        low = product_error + (logc_hi * inverse_lo + logc_lo * inverse_hi)
        scaled = product * 2.0 ** 41 + float.fromhex('0x1.8p52')
        head = (scaled - float.fromhex('0x1.8p52')) * 2.0 ** -41
        mid = (product - head) + low
        table.append((Fraction(head), Fraction(mid)))
    return table


CURRENT_LOG2 = current_log2_table()


# ---- log1p models -----------------------------------------------------------

def log1p_truncation(m, first_omitted):
    """sum over k >= first_omitted of m^k / k, for m <= 1/64."""
    return m ** first_omitted / first_omitted / (1 - m)


def coefficient_error(m, first, last):
    total = Fraction(0)
    for k in range(first, last + 1):
        exact_coefficient = Fraction(1 if k % 2 == 1 else -1, k)
        total += m ** k * abs(LOG_COEFFICIENTS[k] - exact_coefficient)
    return total


_inverse_log2 = {}


def inverse_log2(inv):
    """-log2(inv) from the reference, cached."""
    if inv not in _inverse_log2:
        _inverse_log2[inv] = -reference.log_reference(inv) * INVERSE_LN2
    return _inverse_log2[inv]


def true_log2_bounds(inv_true_log2, z_low, z_high):
    """min |log2 z| over [z_low, z_high] (bucket not containing 1), from the
    references; also used inside bucket 80 on subintervals away from 1."""
    a = reference.log_reference(z_low) * INVERSE_LN2
    b = reference.log_reference(z_high) * INVERSE_LN2
    slack = Fraction(1, 2 ** 300)
    if a > 0 and b > 0:
        return min(a, b) - slack
    if a < 0 and b < 0:
        return min(-a, -b) - slack
    return Fraction(0)


def r_subintervals(i, inv, pieces=16):
    """Subintervals [low, high] of r + r_lo = z inv - 1 over bucket i, none
    containing 0: linear down to 2^-12 of the edge on each side of 0, then
    geometric down to the granularity of z inv - 1 (2^-62 for a 10-bit inv;
    2^-53 in bucket 80, where inv = 1)."""
    first, last = bucket_range(i)
    low = first * inv - 1
    high = last * inv - 1
    finest = 53 if inv == 1 else 62
    result = []
    for side_low, side_high in ((low, min(high, Fraction(0))), (max(low, Fraction(0)), high)):
        if side_low >= side_high:
            continue
        sign = 1 if side_high > 0 else -1
        edge = magnitude(side_low, side_high)
        inner = magnitude(side_low, side_high) - abs(side_high - side_low)   # the side's end nearer 0
        linear_floor = max(inner, edge * Fraction(1, 2 ** 12))
        step = (edge - linear_floor) / pieces
        magnitudes = [(linear_floor + step * n, linear_floor + step * (n + 1)) for n in range(pieces)]
        upper = linear_floor
        while upper > inner and upper > Fraction(1, 2 ** finest):
            lower = max(upper / 2, inner, Fraction(1, 2 ** finest))
            magnitudes.append((lower, upper))
            upper = lower
        for m_low, m_high in magnitudes:
            if sign > 0:
                result.append((m_low, m_high))
            else:
                result.append((-m_high, -m_low))
    return result


# ---- the current pow kernel's log2 x (pow.cpp, pow_positive) ----------------

def current_log2_error(i, d_low, d_high):
    """Absolute error bound of log2 z = (k + H) + s + s_lo for k = 0 and
    r + r_lo in [d_low, d_high]; with the true min |log2 z| there."""
    inv, logc_hi, logc_lo = LOG_TABLE[i]
    head, mid = CURRENT_LOG2[i]
    rho = Fraction(1, 2 ** 53)                      # |r_lo| <= ulp(z inv)/2
    if inv == 1:
        rho = Fraction(0)                           # z * 1 is exact
    r = Value(d_low - rho, d_high + rho)
    m = r.magnitude()
    r_lo = symmetric(rho)
    r_squared = rounded(min(r.low * r.low, r.high * r.high) if r.low * r.high > 0 else Fraction(0),
                        m * m, Fraction(0))
    # cubic = (r * r_squared) * (log_3 + r*(log_4 + ... + r*log_10))
    poly = constant(LOG_COEFFICIENTS[10])
    for k in range(9, 2, -1):
        poly = add(constant(LOG_COEFFICIENTS[k]), multiply(r, poly))
    cubic = multiply(multiply(r, r_squared), poly)
    # a + a_error = r - r_squared/2 exactly (fast two-sum); r_squared_error
    # = r^2 - r_squared exactly, |.| <= half_ulp(r^2).
    a_magnitude = m + m * m / 2
    a_error = symmetric(half_ulp(a_magnitude))
    r_squared_error = symmetric(half_ulp(m * m))
    inner = multiply(r, subtract(constant(1), r))                   # r (1 - r)
    inner = multiply(r, subtract(constant(1), inner))               # r (1 - r (1 - r))
    r_lo_scaled = multiply(r_lo, subtract(constant(1), inner))
    a_lo = add(a_error, add(r_lo_scaled, subtract(cubic, scale(r_squared_error, Fraction(1, 2)))))
    s_magnitude = (a_magnitude + a_lo.magnitude()) * LOG_CONSTANTS['log_inv_ln2_hi']
    s_error = symmetric(half_ulp(s_magnitude))
    a = Value(-a_magnitude, a_magnitude)
    s_lo = add(s_error, add(add(multiply(a, constant(LOG_CONSTANTS['log_inv_ln2_lo'])),
                                multiply(a_lo, constant(LOG_CONSTANTS['log_inv_ln2_hi']))),
                            constant(mid)))
    model = (log1p_truncation(m, 11) + coefficient_error(m, 3, 10) + rho * m ** 4 / (1 - m) +
             4 * rho * rho)
    # Table: -log2(inv) = H + mid_true.
    mid_true = inverse_log2(inv) - head
    inverse_sum = LOG_CONSTANTS['log_inv_ln2_hi'] + LOG_CONSTANTS['log_inv_ln2_lo']
    error = (s_lo.error + abs(mid - mid_true) + a_lo.magnitude() * abs(LOG_CONSTANTS['log_inv_ln2_lo']) +
             (a_magnitude + a_lo.magnitude()) * abs(inverse_sum - INVERSE_LN2) +
             (a_lo.error + model) * inverse_sum)
    parts = {'a_lo (cubic and sums)': a_lo.error * inverse_sum, 's_lo sums': s_lo.error,
             'polynomial model': model * inverse_sum, 'table mid': abs(mid - mid_true)}
    return error, s_lo.magnitude(), parts


# ---- the region kernel's log2 x (region_core.hpp, region_log2_wide) ----------

def region_log1p(d_low, d_high, inv):
    """log1p(r + r_lo) = h + low as region_log1p forms it, for r + r_lo in
    [d_low, d_high] (r the two-sum's head, |r_lo| <= ulp(r)/2). Returns
    (h interval bound, low Value, model error, rho)."""
    m = magnitude(d_low, d_high)
    rho = half_ulp(m)                               # after the two-sum
    if inv == 1:
        # Bucket 80: z - 1 = (z_hi - 1) + z_lo is exact and the two-sum
        # leaves |r_lo| <= ulp(r)/2 <= 2^-53 |r|.
        rho = m * Fraction(1, 2 ** 53)
    r = Value(d_low - rho, d_high + rho)
    m = r.magnitude()
    # Exactness (bit counts): r1 = cut13(r) has 13 bits; r2 = r - r1 is
    # exact with |r2| < 2^-12 |r| and at most 40 bits; square1 = r1^2 (26
    # bits), cross = r1 r2 (53), cube1 = r1^3 (39), cube1 * third_hi (39 +
    # significant_bits(third_hi) <= 53): all exact.
    require(significant_bits(RC['region_third_hi']) <= 14, 'third_hi has more than 14 bits')
    r2_bound = m * Fraction(1, 2 ** 12)
    r2 = symmetric(r2_bound)
    r1_bound = m
    square1 = Value(0, r1_bound ** 2)
    cross_bound = r1_bound * r2_bound
    cube1 = symmetric(r1_bound ** 3)
    cubic_head_bound = r1_bound ** 3 * RC['region_third_hi']
    # Head: three fast two-sums, each exact; their preconditions.
    h_bound = m + r1_bound ** 2 / 2 + cross_bound + cubic_head_bound
    h_low_bound = (min(abs(d_low), abs(d_high)) if d_low * d_high > 0 else Fraction(0))
    require(m >= r1_bound ** 2 / 2, 'fast_two_sum(r, -square1/2) precondition')
    if h_low_bound > 0:
        require(h_low_bound - r1_bound ** 2 / 2 >= cross_bound, 'fast_two_sum(h, -cross) precondition')
        require(h_low_bound - r1_bound ** 2 / 2 - cross_bound >= cubic_head_bound,
                'fast_two_sum(h, cubic_head) precondition')
    e_one = symmetric(half_ulp(h_bound))
    e = add(add(e_one, e_one), e_one)                # (e1 + e2) + e3
    third = constant(Fraction(1.0 / 3.0))
    # cubic_rest = cube1 * third_lo + r2 * (square1 + (cross + (r2 * r2) * third))
    cubic_rest = add(multiply(cube1, constant(RC['region_third_lo'])),
                     multiply(r2, add(square1, add(symmetric(cross_bound), multiply(multiply(r2, r2), third)))))
    square_rest = scale(multiply(r2, r2), Fraction(1, 2))
    r_squared = rounded(Fraction(0), m * m, Fraction(0))
    poly = constant(LOG_COEFFICIENTS[10])
    for k in range(9, 3, -1):
        poly = add(constant(LOG_COEFFICIENTS[k]), multiply(r, poly))
    quartic = multiply(multiply(r_squared, r_squared), poly)
    r_lo = symmetric(rho)
    # linear_lo = r_lo * (1 - r * (1 - r * (1 - r))): r_lo / (1 + r) to r_lo r^3.
    inner = multiply(r, subtract(constant(1), r))
    linear_lo = multiply(r_lo, subtract(constant(1), multiply(r, subtract(constant(1), inner))))
    low = add(e, add(add(subtract(cubic_rest, square_rest), quartic), linear_lo))
    # The exact expression h + low equals r - r^2/2 + r^3/3 (with the stored
    # third_lo and third) + r^4 P(r) + r_lo (1 - r + r^2) exactly; against
    # log1p(r + r_lo):
    third_error = abs(RC['region_third_hi'] + RC['region_third_lo'] - Fraction(1, 3))
    model = (log1p_truncation(m, 11) + coefficient_error(m, 4, 10) + m ** 3 * third_error +
             r2_bound ** 3 * abs(Fraction(1.0 / 3.0) - Fraction(1, 3)) +
             rho * m ** 4 / (1 - m) + rho * rho)
    require(h_low_bound == 0 or h_low_bound / 2 >= low.magnitude(), 'fast_two_sum(h, low) precondition')
    return h_bound, low, model, rho


def region_log2_error(i, d_low, d_high):
    """Absolute error bound of log2 z = log2c_hi + s + s_lo for k = 0 (pow's
    region kernel), r + r_lo in [d_low, d_high]."""
    inv, logc_hi, logc_lo, log2c_hi, log2c_lo = REGION[i]
    h_bound, low, model, rho = region_log1p(d_low, d_high, inv)
    a_magnitude = h_bound + low.magnitude()
    a_lo_bound = half_ulp(a_magnitude)              # fast_two_sum(h, low) normalizes
    a = Value(-a_magnitude, a_magnitude)
    a_lo = symmetric(a_lo_bound)
    inverse_hi = RC['region_inverse_ln2_hi']
    inverse_lo = RC['region_inverse_ln2_lo']
    s_error = symmetric(half_ulp(a_magnitude * inverse_hi))
    s_lo = add(s_error, add(add(multiply(a, constant(inverse_lo)), multiply(a_lo, constant(inverse_hi))),
                            constant(log2c_lo)))
    mid_true = inverse_log2(inv) - log2c_hi
    inverse_sum = inverse_hi + inverse_lo
    error = (s_lo.error + abs(log2c_lo - mid_true) + a_lo_bound * abs(inverse_lo) +
             a_magnitude * abs(inverse_sum - INVERSE_LN2) + (low.error + model) * inverse_sum)
    parts = {'low (roundings)': low.error * inverse_sum, 's_lo sums': s_lo.error,
             'polynomial model': model * inverse_sum, 'table mid': abs(log2c_lo - mid_true)}
    return error, s_lo.magnitude(), parts


def log2_relative(kind):
    """max over buckets and subintervals of error / min|log2 z| (k = 0), the
    worst bucket, and the largest |s_lo|."""
    worst = (Fraction(0), None, None)
    largest_s_lo = Fraction(0)
    per_bucket = []
    for i in range(128):
        inv = LOG_TABLE[i][0] if kind == 'current' else REGION[i][0]
        bucket_worst = Fraction(0)
        bucket_parts = None
        for d_low, d_high in r_subintervals(i, inv):
            if kind == 'current':
                error, s_lo_bound, parts = current_log2_error(i, d_low, d_high)
            else:
                error, s_lo_bound, parts = region_log2_error(i, d_low, d_high)
            largest_s_lo = max(largest_s_lo, s_lo_bound)
            z_low = (1 + d_low) / inv
            z_high = (1 + d_high) / inv
            floor = true_log2_bounds(None, z_low, z_high)
            require(floor > 0, 'log2 z not bounded away from 0 in bucket %d' % i)
            ratio = error / floor
            if ratio > bucket_worst:
                bucket_worst = ratio
                bucket_parts = {name: value / floor for name, value in parts.items()}
            if ratio > worst[0]:
                worst = (ratio, i, bucket_parts)
        per_bucket.append(bucket_worst)
    return worst, largest_s_lo, per_bucket


# ---- 2^t: the exp side --------------------------------------------------------

T_LIMIT = Fraction(1, 512) + Fraction(1, 2 ** 40)    # |t| = |p + p_lo - q/256|


def current_exp_error(j, t_low, t_high):
    """Relative error bound of pow.cpp's 2^(t + t_lo) ~ head + tail (before
    scaling) against 2^(j/256) e^((t + t_lo) ln 2)."""
    t_hi_table, t_lo_table = EXP_TABLE[j]
    ln2_hi, ln2_lo = EXACT_LN2_CONSTANTS
    t = Value(t_low, t_high)
    t_lo = symmetric(half_ulp(magnitude(t_low, t_high)))
    u_m = magnitude(t_low, t_high) * ln2_hi * (1 + U)
    u = Value(-u_m, u_m)
    u_error = symmetric(half_ulp(u_m))
    u_lo = add(u_error, add(multiply(t, constant(ln2_lo)), multiply(t_lo, constant(ln2_hi))))
    poly = constant(EXP_COEFFICIENTS[6])
    for k in (5, 4, 3, 2):
        poly = add(constant(EXP_COEFFICIENTS[k]), multiply(u, poly))
    u_squared = multiply(u, u)
    polynomial_tail = multiply(u_squared, poly)
    rotation_m = t_hi_table * u_m
    rotation_error = symmetric(half_ulp(rotation_m))
    head_error = symmetric(half_ulp(t_hi_table + rotation_m))
    small = add(multiply(constant(t_hi_table), add(polynomial_tail, u_lo)),
                add(constant(t_lo_table), multiply(constant(t_lo_table), u)))
    tail = add(head_error, add(rotation_error, small))
    model = (u_m ** 7 / 5040 * 2 + sum(u_m ** k * abs(EXP_COEFFICIENTS[k] - Fraction(1, math.factorial(k)))
                                       for k in range(2, 7)))
    exact_ln2_error = (magnitude(t_low, t_high) + t_lo.magnitude()) * abs(ln2_hi + ln2_lo - LN2)
    u_lo_cross = u_lo.magnitude() * u_m * 2         # u_lo (e^u - 1) is dropped
    absolute = (tail.error + t_hi_table * (model + exact_ln2_error + u_lo.error + u_lo_cross) +
                abs(t_lo_table) * (model + u_squared.magnitude()) + EXP_TABLE_ERROR * 2)
    # The result is at least T e^-|u| >= T_hi (1 - 2^-52) (1 - |u|).
    return absolute / (t_hi_table * (1 - 2 * U) * (Fraction(1) - u_m))


def region_exp_error(j, t_low, t_high):
    """Relative error bound of the region kernel's 2^(t + t_lo) ~ head + tail
    (region_exp2_wide)."""
    t_hi_table, t_lo_table = EXP_TABLE[j]
    l1, l2, l3 = RC['region_ln2_1'], RC['region_ln2_2'], RC['region_ln2_3']
    require(significant_bits(l1) <= 26 and significant_bits(l2) <= 26, 'ln2 cuts exceed 26 bits')
    tm = magnitude(t_low, t_high)
    t = Value(t_low, t_high)
    t_lo_bound = half_ulp(tm)
    # t1 = cut_26(t) (26 bits), t2 = t - t1 exact (<= 27 bits, |t2| < 2^-25 |t|);
    # u_head = t1 * l1 exact (52 bits).
    t2 = symmetric(tm * Fraction(1, 2 ** 25))
    t1 = Value(-tm, tm)
    u_cross = add(add(exact_multiply(t1, constant(l2)), exact_multiply(t2, constant(l1))),
                  add(multiply(t2, constant(l2)), add(multiply(t, constant(l3)),
                                                      multiply(symmetric(t_lo_bound), constant(l1)))))
    u_m = tm * l1 + u_cross.magnitude()
    u = Value(-u_m, u_m)
    u_lo_bound = half_ulp(u_m)
    # u = u_head + u_cross + model; against (t + t_lo) ln 2:
    u_model = tm * abs(l1 + l2 + l3 - LN2) + t_lo_bound * abs(LN2 - l1) + u_cross.error
    # u1 = cut_26(u), u2 = u - u1 (<= 27 bits); half_square1 = u1^2/2 exact
    # (52 bits); w1 = cut_26(half_square1); w2 = ((half_square1 - w1) + u1 u2)
    # + 0.5 (u2 u2): the first difference and u1 u2 exact, two roundings.
    u2 = symmetric(u_m * Fraction(1, 2 ** 25))
    half_square_rest = symmetric(u_m * u_m / 2 * Fraction(1, 2 ** 25))
    w2 = add(add(half_square_rest, exact_multiply(Value(-u_m, u_m), u2)),
             scale(multiply(u2, u2), Fraction(1, 2)))
    w1_bound = u_m * u_m / 2
    u_squared = multiply(u, u)
    poly = constant(Fraction(1.0 / 5040.0))
    for k in (6, 5, 4, 3):
        poly = add(constant(EXP_COEFFICIENTS[k]), multiply(u, poly))
    cubic = multiply(multiply(u, u_squared), poly)
    # Table cut: c1 = cut_26(T_hi) (26 bits), c2 = (T_hi - c1) + T_lo rounded.
    c1 = cut(t_hi_table, 26)
    c2_exact = (t_hi_table - c1) + t_lo_table
    c2 = nearest_double(c2_exact)
    c2_value = Value(c2, c2, abs(c2 - c2_exact))
    # Head: fast_two_sum(T_hi, c1 u1), fast_two_sum(head, c1 w1), both exact.
    head_m = t_hi_table + c1 * u_m + c1 * w1_bound
    require(t_hi_table >= 1 and c1 * u_m < 1, 'fast_two_sum(T_hi, c1 u1) precondition')
    f = symmetric(2 * half_ulp(head_m))
    rotation_rest = add(exact_multiply(constant(c1), u2), multiply(c2_value, u))
    square_rest = add(multiply(constant(c1), w2), multiply(c2_value, scale(u_squared, Fraction(1, 2))))
    one_plus_u = add(constant(1), u)
    small = add(multiply(constant(t_hi_table), add(cubic, multiply(symmetric(u_lo_bound), one_plus_u))),
                constant(t_lo_table))
    tail = add(f, add(add(rotation_rest, square_rest), small))
    # Model: e^(u + u_lo) against 1 + u + u^2/2 + cubic(u) + u_lo (1 + u),
    # with T = T_hi + T_lo = c1 + c2 (c2's rounding counted in c2_value).
    exp_truncation = u_m ** 8 / 40320 * 2
    coefficient = sum(u_m ** k * abs(EXP_COEFFICIENTS[k] - Fraction(1, math.factorial(k))) for k in range(3, 7))
    coefficient += u_m ** 7 * abs(Fraction(1.0 / 5040.0) - Fraction(1, 5040))
    lo_model = u_lo_bound * u_m * u_m + u_lo_bound ** 2
    # T_lo enters through c2 in every term but the cubic and u_lo ones.
    t_lo_neglected = abs(t_lo_table) * (cubic.magnitude() + 2 * u_lo_bound)
    absolute = (tail.error + t_hi_table * (exp_truncation + coefficient + lo_model) * 2 +
                t_lo_neglected + 2 * EXP_TABLE_ERROR + 2 * u_model * 2)
    # The result is at least T e^-|u| >= T_hi (1 - 2^-52) (1 - |u|).
    return absolute / (t_hi_table * (1 - 2 * U) * (Fraction(1) - u_m))


def exp_relative(kind, pieces=8):
    worst = Fraction(0)
    worst_j = None
    for j in range(256):
        step = 2 * T_LIMIT / pieces
        for n in range(pieces):
            low = -T_LIMIT + step * n
            high = low + step
            if kind == 'current':
                error = current_exp_error(j, low, high)
            else:
                error = region_exp_error(j, low, high)
            if error > worst:
                worst = error
                worst_j = j
    return worst, worst_j


# ---- float logarithms ---------------------------------------------------------

LOG_BASES = ('natural', 'binary', 'decimal')
LN10 = reference.log_reference(10)


def float_log_error(base, i, r_low, r_high, k_low, k_high):
    """Absolute error bound of region.cpp's float log (d + tail, before the
    one rounding) for r in [r_low, r_high] (exact) and k in [k_low, k_high],
    against log_base x; and min |log_base x| there."""
    inv, logc_hi, logc_lo, log2c_hi, log2c_lo = REGION[i]
    r = Value(r_low, r_high)
    m = r.magnitude()
    k = Value(k_low, k_high)
    km = magnitude(k_low, k_high)
    square_low = Fraction(0) if r_low <= 0 <= r_high else min(r_low * r_low, r_high * r_high)
    square = Value(square_low, m * m)                  # exact: r has <= 26 bits
    half_square = scale(square, Fraction(1, 2))
    poly = constant(LOG_COEFFICIENTS[8])
    for n in range(7, 2, -1):
        poly = add(constant(LOG_COEFFICIENTS[n]), multiply(r, poly))
    cubic = multiply(multiply(r, square), poly)
    model = log1p_truncation(m, 9) + coefficient_error(m, 3, 8)
    logc_true = -reference.log_reference(inv)
    if base == 'natural':
        a_m = km * RC['region_ln2_hi']
        e1 = symmetric(half_ulp(a_m + abs(logc_hi)))
        b_m = a_m + abs(logc_hi)
        e2 = symmetric(half_ulp(b_m + m))
        c_m = b_m + m
        e3 = symmetric(half_ulp(c_m + m * m / 2))
        tail = add(add(add(e1, e2), e3), add(add(constant(logc_lo), multiply(k, constant(RC['region_ln2_lo']))),
                                            cubic))
        error = (tail.error + km * abs(RC['region_ln2_hi'] + RC['region_ln2_lo'] - LN2) +
                 abs(logc_hi + logc_lo - logc_true) + model)
        scale_factor = Fraction(1)
    elif base == 'binary':
        c1, c2 = RC['region_inverse_ln2_cut'], RC['region_inverse_ln2_rest']
        p1_rest = multiply(r, constant(c2))
        h_rest = symmetric(m * m / 2 * Fraction(1, 2 ** 25))   # half_square - cut_26(half_square), exact
        q_rest = add(multiply(h_rest, constant(c1)), multiply(half_square, constant(c2)))
        whole_m = km + abs(log2c_hi)
        e2 = symmetric(half_ulp(whole_m + m * c1))
        c_m = whole_m + m * c1
        e3 = symmetric(half_ulp(c_m + m * m / 2 * c1))
        tail = add(add(e2, e3), add(add(constant(log2c_lo), subtract(p1_rest, q_rest)),
                                    multiply(cubic, constant(RC['region_inverse_ln2_hi']))))
        log2c_true = logc_true * INVERSE_LN2
        error = (tail.error + abs(log2c_hi + log2c_lo - log2c_true) +
                 abs(c1 + c2 - INVERSE_LN2) * (m + m * m) +
                 abs(RC['region_inverse_ln2_hi'] - INVERSE_LN2) * cubic.magnitude() + model * INVERSE_LN2)
        scale_factor = INVERSE_LN2
    else:
        d1, d2, d_hi = RC['region_inverse_ln10_cut'], RC['region_inverse_ln10_rest'], RC['region_inverse_ln10_hi']
        inverse_ln10 = 1 / LN10
        e1 = symmetric(half_ulp(abs(logc_hi) + m))
        w1_m = abs(logc_hi) + m
        e2 = symmetric(half_ulp(w1_m + m * m / 2))
        w_m = w1_m + m * m / 2
        w_lo = add(add(e1, e2), add(constant(logc_lo), cubic))
        w = Value(-w_m, w_m)
        w_rest = symmetric(w_m * Fraction(1, 2 ** 25))       # w - cut_26(w), exact
        rest = add(add(multiply(w_rest, constant(d1)), multiply(w, constant(d2))), multiply(w_lo, constant(d_hi)))
        a_m = km * RC['region_log10_2_hi']
        e3 = symmetric(half_ulp(a_m + w_m * d1))
        tail = add(e3, add(multiply(k, constant(RC['region_log10_2_lo'])), rest))
        log10_2 = LN2 * inverse_ln10
        error = (tail.error + km * abs(RC['region_log10_2_hi'] + RC['region_log10_2_lo'] - log10_2) +
                 (w_lo.error + abs(logc_hi + logc_lo - logc_true) + model) * d_hi +
                 abs(d1 + d2 - inverse_ln10) * w_m + abs(d_hi - inverse_ln10) * w_lo.magnitude())
        scale_factor = inverse_ln10
    return error, scale_factor


def float_log_floor(i, r_low, r_high, k_low, k_high):
    """min |ln x| over r in [r_low, r_high], k in [k_low, k_high]."""
    inv = REGION[i][0]
    z_low = (1 + r_low) / inv
    z_high = (1 + r_high) / inv
    if k_low == k_high == 0:
        return true_log2_bounds(None, z_low, z_high) * LN2
    # |k ln2 + ln z| >= |k| ln2 - max |ln z|.
    ln_z = max(abs(reference.log_reference(z_low)), abs(reference.log_reference(z_high)))
    return min(abs(k_low), abs(k_high)) * LN2 - ln_z - Fraction(1, 2 ** 300)


def float_r_subintervals(i, inv, pieces=8):
    """As r_subintervals, down to r's granularity for a float z (2^-33; 2^-24
    in bucket 80)."""
    finest = 24 if inv == 1 else 33
    return [(low, high) for low, high in r_subintervals(i, inv, pieces)
            if max(abs(low), abs(high)) >= Fraction(1, 2 ** finest)]


def float_log_preconditions():
    """Every fast_two_sum of region.cpp's float logs has |a| >= |b| or a == 0
    (by magnitudes over each bucket; k = 0 makes the k-terms zero)."""
    c1 = RC['region_inverse_ln2_cut']
    d1 = RC['region_inverse_ln10_cut']
    for i in range(128):
        inv, logc_hi, logc_lo, log2c_hi, log2c_lo = REGION[i]
        first, last = bucket_range(i)
        m = max(abs(first * inv - 1), abs(last * inv - 1))
        half_square = m * m / 2
        if inv != 1:
            # k = 0: (logc_hi, r), (logc_hi + r, -r^2/2); the same in log10.
            require(abs(logc_hi) >= m, 'float log: |logc_hi| < |r| in bucket %d' % i)
            require(abs(logc_hi) - m >= half_square, 'float log: |c| < r^2/2 in bucket %d' % i)
            # log2, k = 0: (log2c_hi, p1), (c, -q1).
            require(abs(log2c_hi) >= m * c1, 'float log2: |log2c_hi| < |p1| in bucket %d' % i)
            require(abs(log2c_hi) - m * c1 >= half_square * c1, 'float log2: |c| < |q1| in bucket %d' % i)
        # inv == 1 (logc = 0): the first sums have a == 0 and c = r or p1,
        # |r| >= r^2/2 for |r| < 1.
        # k != 0: |k| >= 1.
        ln2_hi = RC['region_ln2_hi']
        require(ln2_hi >= abs(logc_hi), 'float log: |k ln2_hi| < |logc_hi| in bucket %d' % i)
        require(ln2_hi - abs(logc_hi) >= m, 'float log: |b| < |r| in bucket %d' % i)
        require(ln2_hi - abs(logc_hi) - m >= half_square, 'float log: |c| < r^2/2 in bucket %d' % i)
        require(1 - abs(log2c_hi) >= m * c1, 'float log2: |whole| < |p1| in bucket %d' % i)
        require(1 - abs(log2c_hi) - m * c1 >= half_square * c1, 'float log2: |c| < |q1| in bucket %d' % i)
        w_m = (abs(logc_hi) + m + half_square) * (1 + Fraction(1, 2 ** 50))
        require(RC['region_log10_2_hi'] >= w_m * d1, 'float log10: |k log10_2_hi| < |main| in bucket %d' % i)
    print('  fast_two_sum orders hold in every bucket (k = 0 and |k| >= 1)')


def logf_bounds():
    print('== float log, log2, log10 (region kernels): relative error before the one rounding')
    float_log_preconditions()
    worst = {}
    for base in LOG_BASES:
        worst[base] = (Fraction(0), None)
    for i in range(128):
        inv = REGION[i][0]
        first, last = bucket_range(i)
        full = (first * inv - 1, last * inv - 1)
        cases = [(r_low, r_high, 0, 0) for r_low, r_high in float_r_subintervals(i, inv)]
        cases += [(full[0], full[1], 1, 128), (full[0], full[1], -127, -1)]
        for r_low, r_high, k_low, k_high in cases:
            floor = float_log_floor(i, r_low, r_high, k_low, k_high)
            require(floor > 0, 'float log: |log x| not bounded below in bucket %d' % i)
            for base in LOG_BASES:
                error, factor = float_log_error(base, i, r_low, r_high, k_low, k_high)
                ratio = error / (floor * factor)
                if ratio > worst[base][0]:
                    worst[base] = (ratio, i)
    for base in LOG_BASES:
        ratio, i = worst[base]
        print('  %-8s 2^%.2f (worst bucket %s)' % (base, log2_of(ratio), i))
        require(ratio <= Fraction(1, 2 ** 62), 'float log (%s): bound above 2^-62' % base)
    return worst


# ---- composition ----------------------------------------------------------

P_LIMIT = Fraction(1025)          # |p| for every finite, normal result


def pow_budget(kind, p_limit=P_LIMIT, label='double'):
    print('== pow(%s), %s kernel, |p| <= %s' % (label, kind, p_limit))
    (log_ratio, worst_bucket, parts), largest_s_lo, per_bucket = log2_relative(kind)
    print('  log2 x relative error (k = 0, worst bucket %d): 2^%.2f' % (worst_bucket, log2_of(log_ratio)))
    for name, value in sorted(parts.items(), key=lambda item: -item[1]):
        print('    %-28s 2^%.2f' % (name, log2_of(value)))
    print('  largest |s_lo|: 2^%.2f' % log2_of(largest_s_lo))
    ranked = sorted(range(128), key=lambda i: -per_bucket[i])[:6]
    print('  worst buckets: ' + ', '.join('%d (2^%.2f)' % (i, log2_of(per_bucket[i])) for i in ranked))
    exp_error, exp_j = exp_relative(kind)
    print('  2^t relative error (worst index %d): 2^%.2f' % (exp_j, log2_of(exp_error)))
    # p + p_lo against y log2(x)computed: the roundings in p_lo. |y s_lo| <=
    # |p| |s_lo| / |log2 x|, and |log2 x| >= 2^-9 / ln 2 outside x = 1's
    # bucket (inside it s_lo is r's relative rounding).
    min_log2 = Fraction(1, 2 ** 9) * INVERSE_LN2 * Fraction(99, 100)
    y_s_lo = p_limit * largest_s_lo / min_log2
    big_small = 16 * p_limit * U / min_log2 * U      # (big_error + small_error) rounding
    p_lo_rounding = half_ulp(y_s_lo) + big_small + half_ulp(4 * p_limit * U)
    print('  p_lo roundings: 2^%.2f absolute' % log2_of(p_lo_rounding))
    delta_p = p_limit * log_ratio + p_lo_rounding
    # e^(delta_p ln 2) - 1 <= delta_p ln 2 (1 + delta_p).
    relative = delta_p * LN2 * (1 + delta_p) + exp_error * (1 + delta_p)
    # The final rounding of head + tail (in [1/2, 4)) is half an ulp; the
    # error before it is at most relative * 2^53 ulps of the exact result.
    ulps = Fraction(1, 2) + relative * 2 ** 53 * (1 + U)
    print('  composed: 2^%.2f relative before the final rounding, %.6f ulp' % (log2_of(relative), float(ulps)))
    return ulps, relative


def main(arguments):
    step = arguments[0] if arguments else 'all'
    if step in ('pow', 'all'):
        current, _ = pow_budget('current')
        region, _ = pow_budget('region')
        require(region < current, 'the region pow bound does not beat the current one')
        require(region <= Fraction(5002, 10000), 'the region pow bound exceeds 0.5002 ulp')
        # Float pow: |p| <= 128.5 past the early returns (region.cpp); the
        # snap threshold region_pow_float_snap must cover the bound.
        _, float_relative = pow_budget('region', Fraction(129), 'float')
        snap = Fraction(1, 2 ** 67)
        print('  float snap threshold 2^-67 against the bound 2^%.2f' % log2_of(float_relative))
        require(float_relative * (1 + Fraction(1, 2 ** 40)) <= snap, 'float pow bound above the snap threshold')
    if step in ('logf', 'all'):
        logf_bounds()
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    print('all checks hold')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
