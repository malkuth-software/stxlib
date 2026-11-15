#!/usr/bin/env python3
"""Replays float pow's accurate stage (src/algorithms/math/region.cpp,
pow_float_accurate) and the test reference of tests/math_region_tests.cpp
(pow_float_accurate_stage) on the test's own input streams, and measures
each against the 400-bit reference (region_reference.py).

Why: the M4 run of round 2 failed "region pow(float): the accurate stage
above its bound" (error above 2^-93 against tests/reference/wide.hpp's
pow_reference). This tool separates the two candidates: the stage's own
error, operation for operation in IEEE double (Python floats are IEEE
binary64 with round to nearest; every two-product is exact as in the C++),
and the C++ reference's error, replayed operation for operation
(std::log by Python's math.log, the host's libm).

It is a replay over sampled inputs, not a proof: the proof of the stage is
pow_float_accurate_certificate.py. What it shows is whether the compiled
operations, as replayed, stay inside the certificate's 2^-94.10, and
whether the test's reference is accurate enough for the test's tolerance.

Usage: pow_float_accurate_replay.py [main-iterations]
"""

import math
import pathlib
import struct
import sys
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import pow_float_accurate_certificate as certificate  # noqa: E402
import region_certificate as rc  # noqa: E402
import region_reference as reference  # noqa: E402

MASK = (1 << 64) - 1
CHECKED_Y = 64        # the test checks the stage's error against its reference only for |y| <= 64
SHIFT = float.fromhex('0x1.8p52')
LOG_OFFSET_BITS = 0x3fe5f00000000000
EXPONENT_FIELD = 0xfff << 52


def bits_of(value):
    return struct.unpack('<Q', struct.pack('<d', value))[0]


def double_of(bits):
    return struct.unpack('<d', struct.pack('<Q', bits & MASK))[0]


def to_float32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def float32_of_bits(bits):
    return struct.unpack('<f', struct.pack('<I', bits))[0]


def exact_error(a, b, p):
    """fl(a b - p) where a b - p is a double (the fused form's error term)."""
    return float(Fraction(a) * Fraction(b) - Fraction(p))


# ---- the stage, as region.cpp computes it ----------------------------------------

def two_sum(a, b):
    s = a + b
    bb = s - a
    e = (a - (s - bb)) + (b - bb)
    return s, e


def two_product(a, b):
    p = a * b
    return p, exact_error(a, b, p)


def wide_add(a, b):
    s, e = two_sum(a[0], b[0])
    low = e + (a[1] + b[1])
    return two_sum(s, low)


def wide_multiply(a, b):
    p, pe = two_product(a[0], b[0])
    cross = a[0] * b[1] + a[1] * b[0]
    low = pe + cross
    return two_sum(p, low)


def wide_multiply_double(a, b):
    p, pe = two_product(a[0], b)
    low = pe + a[1] * b
    return two_sum(p, low)


def pair(value):
    return (float(value[0]), float(value[1]))


LN2 = pair(certificate.C['ln2'])
INVERSE_LN2 = pair(certificate.C['inverse_ln2'])
LOG_COEFFICIENTS = [pair(c) for c in certificate.C['log']]
EXP_COEFFICIENTS = [pair(c) for c in certificate.C['exp']]
TABLE = [tuple(float(v) for v in row) for row in rc.REGION]
EXP = [(float(hi), float(lo)) for hi, lo in rc.EXP_TABLE]


def stage(x, y):
    """(hi + lo) 2^exponent as an exact Fraction."""
    bits = bits_of(x)
    offset = (bits - LOG_OFFSET_BITS) & MASK
    index = (offset >> 45) & 127
    k = (offset - (1 << 64) if offset >> 63 else offset) >> 52
    z = double_of(bits - (offset & EXPONENT_FIELD))
    inv, logc_hi, logc_lo = TABLE[index][0], TABLE[index][1], TABLE[index][2]
    r = z * inv - 1.0
    series = LOG_COEFFICIENTS[12]
    for n in range(11, -1, -1):
        series = wide_add(wide_multiply_double(series, r), LOG_COEFFICIENTS[n])
    log1p_r = wide_multiply_double(series, r)
    log_z = wide_add((logc_hi, logc_lo), log1p_r)
    log2_z = wide_multiply(log_z, INVERSE_LN2)
    log2_x = wide_add((float(k), 0.0), log2_z)
    p = wide_multiply_double(log2_x, y)
    shifted = p[0] * 256.0 + SHIFT
    q = bits_of(shifted) - bits_of(SHIFT)
    j = q & 255
    exponent = q >> 8
    t_head = p[0] - (shifted - SHIFT) * (1.0 / 256.0)
    t = two_sum(t_head, p[1])
    u = wide_multiply(t, LN2)
    expm1_series = EXP_COEFFICIENTS[8]
    for n in range(7, -1, -1):
        expm1_series = wide_add(wide_multiply(expm1_series, u), EXP_COEFFICIENTS[n])
    expm1_u = wide_multiply(expm1_series, u)
    table = EXP[j]
    value = wide_add(table, wide_multiply(table, expm1_u))
    return (Fraction(value[0]) + Fraction(value[1])) * Fraction(2) ** exponent


# ---- the C++ test reference (tests/reference/wide.hpp) ------------------------------

W_LN2 = (6.931471805599452862e-01, 2.319046813846299558e-17, 5.707708438416212066e-34)


def w_two_sum(a, b):
    return two_sum(a, b)


def w_quick_two_sum(a, b):
    s = a + b
    return s, b - (s - a)


def w_add(a, b):
    s = w_two_sum(a[0], b[0])
    t = w_two_sum(a[1], b[1])
    u = w_quick_two_sum(s[0], s[1] + t[0])
    return w_quick_two_sum(u[0], u[1] + t[1])


def w_multiply(a, b):
    p = a[0] * b[0]
    e = exact_error(a[0], b[0], p)
    cross = a[0] * b[1] + a[1] * b[0]
    return w_quick_two_sum(p, e + cross)


def w_divide_double(a, b):
    q1 = a[0] / b
    product = w_multiply((b, 0.0), (q1, 0.0))
    remainder = w_add(a, (-product[0], -product[1]))
    q2 = remainder[0] / b
    return w_quick_two_sum(q1, q2)


def w_divide(a, b):
    q1 = a[0] / b[0]
    m1 = w_multiply(b, (q1, 0.0))
    r1 = w_add(a, (-m1[0], -m1[1]))
    q2 = r1[0] / b[0]
    m2 = w_multiply(b, (q2, 0.0))
    r2 = w_add(r1, (-m2[0], -m2[1]))
    q3 = r2[0] / b[0]
    head = w_quick_two_sum(q1, q2)
    return w_add(head, (q3, 0.0))


def w_exp_series(r):
    term = (1.0, 0.0)
    total = (1.0, 0.0)
    for n in range(1, 33):
        term = w_divide_double(w_multiply(term, r), float(n))
        total = w_add(total, term)
    return total


def w_exp_reference(z):
    k = float(round(z[0] / W_LN2[0]))
    r = z
    for part in W_LN2:
        p = k * part
        e = exact_error(k, part, p)
        r = w_add(r, (-p, -e))
    return w_exp_series(r), int(k)


def w_log_reference(x):
    y0 = math.log(x)
    mantissa, exponent = w_exp_reference((y0, 0.0))
    scaled_x = math.ldexp(math.ldexp(x, -int(exponent / 2)), -(exponent - int(exponent / 2)))
    ratio = w_divide((scaled_x, 0.0), mantissa)
    t = w_add(ratio, (-1.0, 0.0))
    t_squared_half = w_multiply(w_multiply(t, t), (0.5, 0.0))
    total = w_add(w_add((y0, 0.0), t), (-t_squared_half[0], -t_squared_half[1]))
    return total


def w_pow_reference(x, y):
    mantissa, exponent = w_exp_reference(w_multiply(w_log_reference(x), (y, 0.0)))
    return (Fraction(mantissa[0]) + Fraction(mantissa[1])) * Fraction(2) ** exponent


# ---- the test's input streams --------------------------------------------------------

class Lcg:
    def __init__(self, seed):
        self.state = seed

    def step(self):
        self.state = (self.state * 6364136223846793005 + 1442695040888963407) & MASK

    def unit(self):
        self.step()
        return (self.state >> 11) * 2.0 ** -53

    def bits(self):
        self.step()
        return self.state


def in_range(value):
    return Fraction(2) ** -126 <= value < Fraction(2) ** 128


def measure(label, pairs, with_reference):
    worst_stage = Fraction(0)
    worst_reference = Fraction(0)
    worst_reference_checked = Fraction(0)
    worst_at = None
    above = 0
    count = 0
    for x, y in pairs:
        if not (x > 0.0) or y == 0.0 or abs(y) < 2.0 ** -126:
            continue
        exact = reference.pow_reference(Fraction(x), Fraction(y))
        if not in_range(exact):
            continue
        count += 1
        error = abs(stage(x, y) - exact) / exact
        if error > worst_stage:
            worst_stage = error
            worst_at = (x, y)
        if with_reference:
            reference_error = abs(w_pow_reference(x, y) - exact) / exact
            worst_reference = max(worst_reference, reference_error)
            if abs(y) <= CHECKED_Y:
                worst_reference_checked = max(worst_reference_checked, reference_error)
                if float(reference_error + error) > 2.0 ** -93:
                    above += 1
    line = '%s: %d pairs; stage worst 2^%.2f (x = %s, y = %s)' % (
        label, count, rc.log2_of(worst_stage), worst_at[0].hex(), worst_at[1].hex())
    if with_reference:
        line += ('; test reference worst 2^%.2f, 2^%.2f where |y| <= %d; %d pairs with |y| <= %d where the '
                 'test could see more than 2^-93') % (rc.log2_of(worst_reference), rc.log2_of(worst_reference_checked),
                                                     CHECKED_Y, above, CHECKED_Y)
    print(line)
    return worst_stage


def streams(main_iterations):
    """The pairs of pow_float_accurate_stage: the two main streams (the first
    main_iterations of 100,000), then the 4,096 pairs near x = 1.0039. The
    LCG is advanced through all 100,000 main iterations either way."""
    lcg = Lcg(2032)
    first = []
    second = []
    for i in range(100000):
        x = to_float32(lcg.unit() * 16.0)
        y = to_float32((lcg.unit() - 0.5) * 80.0)
        x_bits = 0x00800000 + lcg.bits() % 0x7f000000
        wide_x = float32_of_bits(x_bits)
        log2_x = math.log2(wide_x)
        if log2_x != 0.0:
            wide_y = to_float32((lcg.unit() - 0.5) * 250.0 / log2_x)
            if i < main_iterations:
                second.append((wide_x, wide_y))
        if i < main_iterations:
            first.append((x, y))
    near = []
    for i in range(4096):
        x = float32_of_bits(0x3f808000 + lcg.bits() % 0x10000)
        y = to_float32((lcg.unit() - 0.5) * 250.0 / math.log2(x))
        near.append((x, y))
    return first, second, near


def main(arguments):
    main_iterations = int(arguments[0]) if arguments else 2000
    first, second, near = streams(main_iterations)
    worst = Fraction(0)
    worst = max(worst, measure('near x = 1.0039 (4,096 pairs)', near, True))
    worst = max(worst, measure('x in [0, 16), |y| <= 40 (first %d)' % main_iterations, first, True))
    worst = max(worst, measure('any x, |y log2 x| <= 125 (first %d)' % main_iterations, second, True))
    hard = [(float.fromhex('0x1.46ee2p+67'), float.fromhex('-0x1.acbb3ap-7'))]
    worst = max(worst, measure('the CORE-MATH hard case', hard, True))
    print('stage worst over every replayed pair: 2^%.2f (certificate: 2^-94.10)' % rc.log2_of(worst))
    return 0 if worst <= Fraction(certificate.C['stated']) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
