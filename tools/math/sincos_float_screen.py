#!/usr/bin/env python3
"""Proof that float sincos (src/algorithms/math/sincos.cpp) is correctly
rounded for every float 2^-27 <= |x| <= 2^32.

The float kernel computes sin x and cos x in double (the table value at
k pi/128, a rounded remainder r, short polynomials) and rounds each once to
float. This tool:

  1. bounds that double's absolute error against sin x and cos x for each
     table index k, every rounding and truncation counted, with the interval
     arithmetic of sincos_certificate.py (Fractions; the remainder's error
     from the reduction's exact parts, the table's from its certified
     values);
  2. replicates the kernel in numpy, operation for operation in IEEE double
     (numpy neither fuses nor reorders; the C++ is built with
     -ffp-contract=off), for every float of the range, both signs, and
     certifies each result whose distance to the nearest float midpoint
     exceeds the bound: the exact value then rounds to the same float;
  3. decides each float the screen leaves exactly: sin and cos of the float
     from sincos_certificate.py's certified intervals (pi from Machin and
     Gauss), rounded to float, against the kernel's float.

A float is proved when step 2 certifies it or step 3 confirms it. The
kernel's exception table (sincos.cpp, float_sincos_exceptions: the floats a
first run of this tool found misrounded) is applied as the kernel applies it,
and every exception is decided exactly. Exit 1 if any float is misrounded or
undecided.

Result (2026-11-15, numpy on Apple arm64, full run 383 s): 989,855,746
floats; all but 44 certified by the screen; 42 confirmed exactly; +-9830.3984375
misrounded by the kernel (sin), now the one exception. 2^20 < |x| <= 2^32
(201,326,592 floats) needed no exact decision. Rerun with the exception
table: 989,855,702 certified, 44 confirmed exactly, 0 misrounded.

Usage:
  sincos_float_screen.py                 every float 2^-27 <= |x| <= 2^32
  sincos_float_screen.py --medium        only 2^20 < |x| <= 2^32
  sincos_float_screen.py --stride N      every N-th float (a sampled trial)
"""

import argparse
import math
import pathlib
import sys
import time
from fractions import Fraction

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sincos_certificate as base  # noqa: E402

U = base.U
TABLE = base.TABLE
FAST_PARTS = base.FAST_PARTS
MEDIUM_PARTS = base.MEDIUM_PARTS
MEDIUM_REST = base.MEDIUM_REST
CO = base.COEFFICIENTS
SHIFT = float.fromhex('0x1.8p52')
INVERSE_STEP = float(base.INVERSE_STEP)
FAST_LIMIT = 2.0 ** 20


# ---- step 1: the error bound per table index ------------------------------

def remainder_error(path):
    """|r - (x - q pi/128)| over every x of the path, r the rounded remainder
    the float kernel uses, and the largest |r|."""
    binades = [b for b in base.binades() if b.path == path]
    worst_error = Fraction(0)
    r_cap = Fraction(0)
    for binade in binades:
        q_max, remainder_max = base.q_bound(binade)
        r_cap = max(r_cap, remainder_max)
        if q_max == 0:
            continue                               # q = 0: r = x exactly
        if path == 'fast':
            # r1 = x - q s1 exact (as in fast_reduction_error), r2 = fl(r1 -
            # q s2) with q s2 exact, r = fl(r2 - fl(q s3)).
            s1, s2, s3 = FAST_PARTS
            base.require(base.product_exact(q_max, s1) and base.product_exact(q_max, s2),
                         'float fast reduction: inexact product in ' + binade.name())
            granule = min(Fraction(2) ** binade.s, base.significant_bits(s1)[1])
            r1_max = remainder_max + q_max * base.tail_bound([s1])
            base.require(base.fits_exactly(r1_max, granule), 'x - q s1 inexact in ' + binade.name())
            r2_max = remainder_max + q_max * base.tail_bound([s1, s2])
            product_3 = q_max * abs(s3)
            error = (q_max * base.tail_bound([s1, s2, s3]) + U * r2_max + U * product_3 +
                     U * (r2_max + product_3) * (1 + 2 * U))
        else:
            # r = fl(r + r_lo) from sincos_remainder_medium's normalized pair.
            error = base.medium_reduction_error(binade) + U * (remainder_max + base.medium_reduction_error(binade))
        worst_error = max(worst_error, error)
    return worst_error, r_cap


def kernel_error(k, r_cap, rho):
    """Absolute error bounds of the float kernel's doubles s and c at table
    index k, |r| <= r_cap, the remainder within rho of x - q pi/128."""
    s, c, s_lo, c_lo = TABLE[k]
    sigma_s, sigma_c = base.table_sigmas(k)
    m = r_cap + rho
    r = base.Value(-r_cap, r_cap, rho)
    r2 = base.multiply(r, r)
    # sin_r = r + r * (r2 * (sin_3 + r2 * sin_5))
    t = base.multiply(r2, base.constant(CO['sin_5']))
    t = base.add(base.constant(CO['sin_3']), t)
    t = base.multiply(r2, t)
    t = base.multiply(r, t)
    sin_r = base.add(r, t)
    # cos_r_minus_1 = (r2 * r2) * (cos_4 + r2 * cos_6) - r2 * cos_half
    a = base.multiply(r2, r2)
    b = base.multiply(r2, base.constant(CO['cos_6']))
    b = base.add(base.constant(CO['cos_4']), b)
    a = base.multiply(a, b)
    half = base.Value(r2.low / 2, r2.high / 2, r2.error / 2)
    cos_m1 = base.subtract(a, half)
    # Model: the polynomials with stored coefficients against sin and cos of
    # the true remainder (alternating series: the first omitted term bounds
    # the rest for |r| < 1).
    model_sin = (m ** 3 * abs(Fraction(-1, 6) - CO['sin_3']) + m ** 5 * abs(Fraction(1, 120) - CO['sin_5']) +
                 m ** 7 / 5040)
    model_cos = (m ** 4 * abs(Fraction(1, 24) - CO['cos_4']) + m ** 6 * abs(Fraction(-1, 720) - CO['cos_6']) +
                 m ** 8 / 40320)
    sv = base.constant(s)
    cv = base.constant(c)
    sin_corr = base.add(base.multiply(sv, cos_m1), base.multiply(cv, sin_r))
    cos_corr = base.subtract(base.multiply(cv, cos_m1), base.multiply(sv, sin_r))
    sin_value = base.add(sv, sin_corr)
    cos_value = base.add(cv, cos_corr)
    # Table: S and C stand for S + S_lo and C + C_lo, which are within sigma
    # of the true values; sin x = S_true cos r + C_true sin r.
    table_s = abs(s_lo) + sigma_s
    table_c = abs(c_lo) + sigma_c
    sin_error = sin_value.error + abs(c) * model_sin + abs(s) * model_cos + table_s + table_c * m
    cos_error = cos_value.error + abs(c) * model_cos + abs(s) * model_sin + table_c + table_s * m
    return sin_error, cos_error


def error_tables():
    bounds = {}
    for path in ('fast', 'medium'):
        rho, r_cap = remainder_error(path)
        r_cap = r_cap * (1 + Fraction(1, 2 ** 30))
        sin_bounds = np.zeros(256)
        cos_bounds = np.zeros(256)
        for k in range(256):
            sin_error, cos_error = kernel_error(k, r_cap, rho)
            # Rounded up to a double, with slack for the screen's own distance
            # computation (one rounding of a difference below 2^-24).
            sin_bounds[k] = float(sin_error * (1 + Fraction(1, 2 ** 40))) * (1 + 2.0 ** -50)
            cos_bounds[k] = float(cos_error * (1 + Fraction(1, 2 ** 40))) * (1 + 2.0 ** -50)
        bounds[path] = (sin_bounds, cos_bounds)
        print('step 1 (%s): remainder within 2^%.2f (|r| <= %.6f); kernel error <= 2^%.2f (sin), 2^%.2f (cos)'
              % (path, base.log2(rho), float(r_cap), math.log2(sin_bounds.max()), math.log2(cos_bounds.max())))
    return bounds


# ---- step 2: the replica and the screen -----------------------------------

def read_exceptions():
    """sincos.cpp's float_sincos_exceptions: (|x| bits, sin |x|, cos |x|)."""
    import re
    text = (base.ROOT / 'src' / 'algorithms' / 'math' / 'sincos.cpp').read_text()
    block = re.search(r'float_sincos_exceptions\[\] = \{(.*?)\n\};', text, re.S)
    base.require(block is not None, 'sincos.cpp: float_sincos_exceptions not found')
    entries = re.findall(r'\{(0x[0-9a-fA-F]+)u, (-?0x[0-9a-fp.+-]+)f, (-?0x[0-9a-fp.+-]+)f\}', block.group(1))
    return [(int(bits, 16), float.fromhex(sin_value), float.fromhex(cos_value)) for bits, sin_value, cos_value in entries]


EXCEPTIONS = read_exceptions()

S_TABLE = np.array([float(entry[0]) for entry in TABLE])
C_TABLE = np.array([float(entry[1]) for entry in TABLE])
FAST = [float(part) for part in FAST_PARTS]
MEDIUM = [float(part) for part in MEDIUM_PARTS]
REST = float(MEDIUM_REST)
SIN_3, SIN_5 = float(CO['sin_3']), float(CO['sin_5'])
COS_4, COS_6 = float(CO['cos_4']), float(CO['cos_6'])


def two_sum(a, b):
    s = a + b
    b_part = s - a
    a_part = s - b_part
    return s, (a - a_part) + (b - b_part)


def kernel(x, medium):
    """sincos(float)'s doubles before the final rounding, and the index."""
    shifted = x * INVERSE_STEP + SHIFT
    index = (shifted.view(np.uint64) & np.uint64(255)).astype(np.intp)
    q = shifted - SHIFT
    if not medium:
        r = ((x - q * FAST[0]) - q * FAST[1]) - q * FAST[2]
    else:
        head = x - q * MEDIUM[0]
        low = np.zeros_like(x)
        for part in MEDIUM[1:]:
            head, error = two_sum(head, -(q * part))
            low = low + error
        low = low - q * REST
        r = head + low                      # two_sum's sum
    r2 = r * r
    sin_r = r + r * (r2 * (SIN_3 + r2 * SIN_5))
    cos_m1 = (r2 * r2) * (COS_4 + r2 * COS_6) - r2 * 0.5
    s_table = S_TABLE[index]
    c_table = C_TABLE[index]
    s = s_table + (s_table * cos_m1 + c_table * sin_r)
    c = c_table + (c_table * cos_m1 - s_table * sin_r)
    return s, c, index


def midpoint_distance(value):
    """Distance from each double to the nearest midpoint between floats."""
    f = value.astype(np.float32)
    up = np.nextafter(f, np.float32(np.inf)).astype(np.float64)
    down = np.nextafter(f, np.float32(-np.inf)).astype(np.float64)
    f64 = f.astype(np.float64)
    m_up = (f64 + up) * 0.5                 # exact: 25-bit values
    m_down = (f64 + down) * 0.5
    return np.minimum(np.abs(value - m_up), np.abs(value - m_down))


def correctly_rounded(low, high):
    """The float nearest every value in [low, high], or None if they differ."""
    first = round_float(low)
    second = round_float(high)
    return first if first == second else None


def round_float(value):
    """The float nearest a rational (ties to even), as a Python float."""
    value = Fraction(value)
    if value == 0:
        return 0.0
    sign = -1 if value < 0 else 1
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - 23)
    scaled = magnitude / quantum
    integer = math.floor(scaled)
    rest = scaled - integer
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and integer % 2 == 1):
        integer += 1
    return float(sign * integer * quantum)


def decide(x, s_out, c_out):
    (s_low, s_high), (c_low, c_high) = base.sin_cos_of_double(Fraction(x))
    s_exact = correctly_rounded(s_low, s_high)
    c_exact = correctly_rounded(c_low, c_high)
    if s_exact is None or c_exact is None:
        return 'undecided'
    if s_exact == float(s_out) and c_exact == float(c_out):
        return 'correct'
    return 'misrounded'


def screen(bounds, first_bits, last_bits, medium, stride, totals):
    sin_bounds, cos_bounds = bounds['medium' if medium else 'fast']
    chunk = 1 << 23
    for start in range(first_bits, last_bits + 1, chunk * stride):
        stop = min(start + chunk * stride, last_bits + 1)
        bits = np.arange(start, stop, stride, dtype=np.uint32)
        magnitude = bits.view(np.float32).astype(np.float64)
        for sign in (1.0, -1.0):
            x = magnitude * sign
            s, c, index = kernel(x, medium)
            s_out = s.astype(np.float32)
            c_out = c.astype(np.float32)
            open_s = midpoint_distance(s) <= sin_bounds[index]
            open_c = midpoint_distance(c) <= cos_bounds[index]
            # The exceptions return stored values: each is decided exactly.
            for magnitude_bits, sin_value, cos_value in EXCEPTIONS:
                hits = np.nonzero(bits == magnitude_bits)[0]
                s_out[hits] = np.float32(sign * sin_value)
                c_out[hits] = np.float32(cos_value)
                open_s[hits] = True
            pending = np.nonzero(open_s | open_c)[0]
            totals['floats'] += x.size
            totals['screened'] += x.size - pending.size
            for position in pending:
                verdict = decide(float(x[position]), s_out[position], c_out[position])
                totals[verdict] += 1
                if verdict != 'correct':
                    print('  %s at x = %s' % (verdict, float(x[position]).hex()))


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--medium', action='store_true')
    parser.add_argument('--stride', type=int, default=1)
    options = parser.parse_args(arguments)
    start = time.time()
    bounds = error_tables()
    totals = {'floats': 0, 'screened': 0, 'correct': 0, 'misrounded': 0, 'undecided': 0}
    tiny = int(np.float32(2.0 ** -27).view(np.uint32))
    fast_last = int(np.float32(FAST_LIMIT).view(np.uint32))
    medium_last = int(np.float32(2.0 ** 32).view(np.uint32))
    if not options.medium:
        screen(bounds, tiny, fast_last, False, options.stride, totals)
        print('step 2 (fast, 2^-27 <= |x| <= 2^20): %d floats, %d certified by the screen, %d decided exactly'
              % (totals['floats'], totals['screened'], totals['correct'] + totals['misrounded'] + totals['undecided']))
    fast_floats = totals['floats']
    screen(bounds, fast_last + 1, medium_last, True, options.stride, totals)
    print('step 2 (medium, 2^20 < |x| <= 2^32): %d floats' % (totals['floats'] - fast_floats))
    print('totals: %d floats, %d certified by the screen, %d confirmed exactly, %d misrounded, %d undecided '
          '(stride %d, %.0f s)' % (totals['floats'], totals['screened'], totals['correct'], totals['misrounded'],
                                   totals['undecided'], options.stride, time.time() - start))
    if totals['misrounded'] or totals['undecided'] or base.failures:
        return 1
    print('float sincos correctly rounded at every float checked')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
