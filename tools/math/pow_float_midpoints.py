#!/usr/bin/env python3
"""Near-midpoint search for float pow (the owner's proof item for the region
float pow, 2026-11-15).

The region float pow (src/algorithms/math/region.cpp) is correctly rounded
for a pair (x, y) unless x^y lies within its error bound (2^-69.31
relative, region_certificate.py) of a float midpoint without being one, and
it snaps any value within 2^-67 of a midpoint to it. A proof of correct
rounding for every pair needs the closest approach of x^y to a midpoint over
every float pair with a normal result, about 2^60 pairs. This tool:

  count    estimates that number of pairs (positive normal x, normal y,
           2^-126 <= x^y < 2^128, |y ln x| >= 2^-26: closer to 1 every
           result rounds to 1 or its neighbour with no midpoint near);
  density  samples pairs uniformly over that set (x uniform over bit
           patterns, then y uniform over the bit patterns of its range),
           evaluates x^y to 2^-79 (log2 x from the 400-bit reference as a
           double-double, y log2 x by an exact product, 2^p by a numpy
           replica of region_core.hpp's region_exp2) and counts the pairs
           within 2^-w of a midpoint for w = 36..60. The Lefevre-Muller
           heuristic predicts a fraction 2^(25-w) to 2^(26-w); the counts
           test it, and the expected number of pairs within the snap
           distance 2^-67 follows.
  integer  an exact sweep of x^n for every odd significand X < 2^24 and n
           in [3, N] (x^n = X^n 2^(ne) exactly): the closest non-exact
           approach to a midpoint, a complete search of those slices.

The tool proves nothing about every pair: it measures how far such a proof
is from reach, and decides the integer slices exactly.

Results (2026-11-15, Neo, numpy 2.5.4): count about 2^60.0 pairs; density
over 2^32 sampled pairs follows 2^(24.6-w) from w = 36 to 54, extrapolating
to about 2^18.2 pairs within 2^-67 and 2^15.9 within 2^-69.31 (sampled);
integer slices n = 3..40: closest non-exact approach 2^-51.67 (n = 37,
X = 8047171), 43 exact midpoints, so those slices are correctly rounded by
the region kernel (proved, exhaustive).

Usage:
  pow_float_midpoints.py count
  pow_float_midpoints.py density [--x-samples N] [--y-per-x M]
  pow_float_midpoints.py integer [--max-n N]
"""

import argparse
import math
import pathlib
import random
import re
import sys
import time
from fractions import Fraction

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import region_reference as reference  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
MATH = ROOT / 'src' / 'algorithms' / 'math'
HEX = r'(-?0x[0-9a-fA-F.]+p[+-]?\d+)'


def read_constants():
    region = (MATH / 'region_log_table128.hpp').read_text()
    constants = {name: float.fromhex(value)
                 for name, value in re.findall(r'inline constexpr double (region_\w+) = ' + HEX + ';', region)}
    exp_text = (MATH / 'exp_table256.hpp').read_text()
    rows = re.findall(r'\{' + HEX + ', ' + HEX + r'\}', exp_text)
    table = [(1.0, 0.0)] + [(float.fromhex(a), float.fromhex(b)) for a, b in rows[1:]]
    return constants, table


C, EXP_TABLE = read_constants()
EXP_HI = np.array([hi for hi, _ in EXP_TABLE])
EXP_LO = np.array([lo for _, lo in EXP_TABLE])
CUT_26_MASK = np.uint64(~((1 << 27) - 1) & 0xffffffffffffffff)
SHIFT = float.fromhex('0x1.8p52')
SPLITTER = float(2 ** 27 + 1)
E3, E4, E5, E6, E7 = 1.0 / 6.0, 1.0 / 24.0, 1.0 / 120.0, 1.0 / 720.0, 1.0 / 5040.0


def cut_26(value):
    return (value.view(np.uint64) & CUT_26_MASK).view(np.float64)


EXP_CUT1 = cut_26(EXP_HI.copy())
EXP_CUT2 = (EXP_HI - EXP_CUT1) + EXP_LO


def two_sum(a, b):
    s = a + b
    b_part = s - a
    a_part = s - b_part
    return s, (a - a_part) + (b - b_part)


def fast_two_sum(a, b):
    s = a + b
    return s, b - (s - a)


def split(a):
    t = SPLITTER * a
    hi = t - (t - a)
    return hi, a - hi


def two_product(a, b):
    """Dekker's exact product (numpy has no fused multiply-add)."""
    p = a * b
    a_hi, a_lo = split(a)
    b_hi, b_lo = split(b)
    e = ((a_hi * b_hi - p) + a_hi * b_lo + a_lo * b_hi) + a_lo * b_lo
    return p, e


def region_exp2(p, p_lo):
    """region_core.hpp's region_exp2, element by element: (head + tail)
    2^exponent = 2^(p + p_lo) within 2^-79.9."""
    shifted = p * 256.0 + SHIFT
    q = shifted.view(np.int64) - np.float64(SHIFT).view(np.int64)
    j = (q & 255).astype(np.intp)
    exponent = q >> 8
    t_head = p - (shifted - SHIFT) * (1.0 / 256.0)
    t, t_lo = two_sum(t_head, p_lo)
    t1 = cut_26(t)
    t2 = t - t1
    u_head = t1 * C['region_ln2_1']
    u_cross = (t1 * C['region_ln2_2'] + t2 * C['region_ln2_1']) + \
        (t2 * C['region_ln2_2'] + (t * C['region_ln2_3'] + t_lo * C['region_ln2_1']))
    u, u_lo = fast_two_sum(u_head, u_cross)
    u1 = cut_26(u)
    u2 = u - u1
    half_square1 = (u1 * u1) * 0.5
    w1 = cut_26(half_square1)
    w2 = ((half_square1 - w1) + u1 * u2) + 0.5 * (u2 * u2)
    u_squared = u * u
    cubic = (u * u_squared) * (E3 + u * (E4 + u * (E5 + u * (E6 + u * E7))))
    hi = EXP_HI[j]
    lo = EXP_LO[j]
    c1 = EXP_CUT1[j]
    c2 = EXP_CUT2[j]
    head, f1 = fast_two_sum(hi, c1 * u1)
    head, f2 = fast_two_sum(head, c1 * w1)
    rotation_rest = c1 * u2 + c2 * u
    square_rest = c1 * w2 + c2 * (0.5 * u_squared)
    small = hi * (cubic + u_lo * (1.0 + u)) + lo
    tail = (f1 + f2) + ((rotation_rest + square_rest) + small)
    return head, tail, exponent


def midpoint_distance(head, tail):
    """|v - m| / v for v = head + tail (in [0.998, 2.003)) and m the float
    midpoint in v's float interval (the only rounding boundary there); the
    grid is the same in every normal binade, so the scaling 2^exponent does
    not change it."""
    v = head + tail
    mantissa, e = np.frexp(v)                  # v = mantissa 2^e, mantissa in [0.5, 1)
    ulp = np.ldexp(np.ones_like(v), e - 24)    # float ulp in v's binade
    m = (np.floor(v / ulp) + 0.5) * ulp
    return np.abs((head - m) + tail) / v


def exact_distance(value):
    """|value - m| / value for m the midpoint of value's float interval
    (value positive, a Fraction within 2^-390 of exact)."""
    e = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** e > value:
        e -= 1
    ulp = Fraction(2) ** (e - 23)
    m = (math.floor(value / ulp) + Fraction(1, 2)) * ulp
    return float(abs(value - m) / value)


def log2_double_double(x):
    """log2 x as hi + lo, from the 400-bit reference (error below 2^-105 |log2 x|)."""
    value = reference.log_reference(Fraction(x)) / reference.log_reference(2)
    hi = float(value)
    lo = float(value - Fraction(hi))
    return hi, lo


def float_bits_value(bits):
    return np.asarray(bits, dtype=np.uint32).view(np.float32).astype(np.float64)


def y_bit_range(x):
    """The bit patterns of positive floats y with 2^-26 <= y |ln x| and y
    |log2 x| <= 126."""
    ln_x = abs(math.log(x))
    low = 2.0 ** -26 / ln_x
    high = 126.0 / abs(math.log2(x))
    low_bits = int(np.float32(max(low, 2.0 ** -126)).view(np.uint32))
    high_bits = int(np.float32(min(high, 3.4e38)).view(np.uint32))
    return low_bits, high_bits


def random_positive_normal(rng):
    return float(np.uint32(rng.randrange(0x00800000, 0x7f800000)).view(np.float32))


def count_step(samples=200000, seed=1):
    rng = random.Random(seed)
    total_y = 0
    for _ in range(samples):
        x = random_positive_normal(rng)
        if x == 1.0:
            continue
        low_bits, high_bits = y_bit_range(x)
        if high_bits > low_bits:
            total_y += 2 * (high_bits - low_bits)       # both signs of y
    mean = total_y / samples
    x_count = 0x7f800000 - 0x00800000
    pairs = mean * x_count
    print('== count: about 2^%.2f pairs (mean 2^%.2f values of y per x, %d x sampled)' %
          (math.log2(pairs), math.log2(mean), samples))
    return pairs


WINDOWS = list(range(36, 61, 2))


def density_step(x_samples, y_per_x, seed=2):
    rng = random.Random(seed)
    nprng = np.random.default_rng(seed)
    counts = {w: 0 for w in WINDOWS}
    closest = []
    evaluated = 0
    start = time.time()
    for _ in range(x_samples):
        x = random_positive_normal(rng)
        if x == 1.0:
            continue
        low_bits, high_bits = y_bit_range(x)
        if high_bits <= low_bits:
            continue
        bits = nprng.integers(low_bits, high_bits, size=y_per_x, dtype=np.uint64).astype(np.uint32)
        y = float_bits_value(bits)
        y = np.where(nprng.integers(0, 2, size=y_per_x) == 0, y, -y)
        l_hi, l_lo = log2_double_double(x)
        p, p_error = two_product(y, np.full_like(y, l_hi))
        p_lo = p_error + y * l_lo
        head, tail, _ = region_exp2(p, p_lo)
        distance = midpoint_distance(head, tail)
        evaluated += y_per_x
        for w in WINDOWS:
            counts[w] += int(np.count_nonzero(distance <= 2.0 ** -w))
        near = np.nonzero(distance <= 2.0 ** -52)[0]
        for position in near:
            closest.append((float(distance[position]), x, float(y[position])))
    elapsed = time.time() - start
    print('== density: 2^%.2f pairs evaluated in %.0f s' % (math.log2(evaluated), elapsed))
    print('  w   pairs within 2^-w   fraction     heuristic 2^(25.5-w)')
    for w in WINDOWS:
        fraction = counts[w] / evaluated
        print('  %2d  %10d          2^%-8s  2^%.1f' % (w, counts[w], ('%.2f' % math.log2(fraction)) if fraction else '-inf',
                                                      25.5 - w))
    closest.sort()
    for _, x, y in closest[:5]:
        # Confirm the closest exactly: x^y from the 400-bit references.
        exact = reference.exp_reference(Fraction(y) * reference.log_reference(Fraction(x)))
        print('  closest: x = %s, y = %s, exact distance 2^%.2f' % (x.hex(), y.hex(), math.log2(exact_distance(exact))))
    return counts, evaluated


def integer_step(max_n):
    """Every odd X < 2^24 (x = X 2^e, e free) and n in [3, max_n]: the
    closest non-exact approach of X^n to a midpoint (relative). X^n has L
    bits; its 25 leading bits top lie between top 2^s and (top + 1) 2^s
    (s = L - 25), and the odd one of the two is the midpoint of the float
    interval holding X^n (the even one is a float)."""
    print('== integer slices: y = n in [3, %d], every odd significand' % max_n)
    for n in range(3, max_n + 1):
        start = time.time()
        best = Fraction(1)
        best_x = 0
        exact = 0
        for X in range(3, 1 << 24, 2):
            value = X ** n
            shift = value.bit_length() - 25
            if shift <= 0:
                if shift == 0:
                    exact += 1                # 25 bits, odd: X^n is a midpoint exactly (the kernel snaps it)
                continue                      # fewer: X^n is a float
            top = value >> shift
            low = value & ((1 << shift) - 1)
            distance = low if top & 1 else (1 << shift) - low
            if distance == 0:
                exact += 1
                continue
            if distance * best.denominator < best.numerator * value:
                best = Fraction(distance, value)
                best_x = X
        elapsed = time.time() - start
        print('  n = %d: closest non-exact 2^%.2f at X = %d; %d exact midpoints; %.0f s' %
              (n, math.log2(best), best_x, exact, elapsed))


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('step', choices=('count', 'density', 'integer', 'all'))
    parser.add_argument('--x-samples', type=int, default=256)
    parser.add_argument('--y-per-x', type=int, default=1 << 22)
    parser.add_argument('--max-n', type=int, default=4)
    options = parser.parse_args(arguments)
    pairs = None
    if options.step in ('count', 'all'):
        pairs = count_step()
    if options.step in ('density', 'all'):
        counts, evaluated = density_step(options.x_samples, options.y_per_x)
        if pairs is None:
            pairs = count_step()
        # Fit the fraction at the finest windows with counts, then extrapolate.
        usable = [w for w in WINDOWS if counts[w] >= 20]
        w = max(usable)
        fraction = counts[w] / evaluated * 2.0 ** (w - 67)
        print('  extrapolated from w = %d: about 2^%.1f pairs within 2^-67 of a midpoint (the snap distance), '
              '2^%.1f within 2^-69.31 (the bound)' % (w, math.log2(fraction * pairs),
                                                      math.log2(fraction * pairs * 2.0 ** -2.31)))
    if options.step in ('integer', 'all'):
        integer_step(options.max_n)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
