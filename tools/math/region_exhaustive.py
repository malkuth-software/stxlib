#!/usr/bin/env python3
"""Every positive normal float through a bit-exact replica of region.cpp's
float log, log2 and log10, screened against the certified error bound.

The replica performs the kernel's IEEE double operations in the kernel's
order with numpy (float64 elementwise operations round to nearest, one
rounding each; numpy never contracts a multiply and an add). For each x it
forms the kernel's double-double v = d + tail and its float result f =
RN(v) (round_wide_to_float rounds the exact v once). The certificate
(region_certificate.py logf) proves |v - log_b x| <= 2^-62 |log_b x| for
every x, so f is the correctly rounded log_b x whenever v lies farther than
that bound from both ends of f's rounding interval: the exact value then
rounds to f too. The x that the screen cannot settle are decided against
region_reference.py's 400-bit logarithm and listed.

What this proves: for the replica's arithmetic, correct rounding of every
positive normal float (with the certificate's bound). That the C++ kernel
does the same operations is checked by tests/math_region_tests.cpp's
exhaustive mode on the M4 (it compares region::log* with the current
kernels, themselves exhaustively correctly rounded).

Usage:
  region_exhaustive.py [--chunk-bits N] [--first HEX --last HEX]
"""

import argparse
import math
import pathlib
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

LOG_OFFSET_BITS = np.uint64(0x3fe6000000000000 - (1 << 44))
EXPONENT_FIELD = np.uint64(0x7ff0000000000000)
CUT_26_MASK = np.uint64(~((1 << 27) - 1) & 0xffffffffffffffff)
BOUND = 2.0 ** -62                      # certified relative bound, all three
SCREEN = BOUND * (1 + 2.0 ** -40)       # with room for the screen's own rounding

LOG_3 = 1.0 / 3.0
LOG_4 = -1.0 / 4.0
LOG_5 = 1.0 / 5.0
LOG_6 = -1.0 / 6.0
LOG_7 = 1.0 / 7.0
LOG_8 = -1.0 / 8.0


def read_table():
    text = (MATH / 'region_log_table128.hpp').read_text()
    rows = re.findall(r'\{' + HEX + ', ' + HEX + ', ' + HEX + ', ' + HEX + ', ' + HEX + r'\}', text)
    if len(rows) != 128:
        raise SystemExit('region_log_table128.hpp: expected 128 entries')
    columns = [np.array([float.fromhex(row[c]) for row in rows]) for c in range(5)]
    constants = {}
    for name, value in re.findall(r'inline constexpr double (region_\w+) = ' + HEX + ';', text):
        constants[name] = float.fromhex(value)
    return columns, constants


(INV, LOGC_HI, LOGC_LO, LOG2C_HI, LOG2C_LO), C = read_table()


def two_sum(a, b):
    s = a + b
    b_part = s - a
    a_part = s - b_part
    e = (a - a_part) + (b - b_part)
    return s, e


def fast_two_sum(a, b):
    s = a + b
    e = b - (s - a)
    return s, e


def cut_26(value):
    return (value.view(np.uint64) & CUT_26_MASK).view(np.float64)


def parts(xd):
    bits = xd.view(np.uint64)
    offset = bits - LOG_OFFSET_BITS
    index = ((offset >> np.uint64(45)) & np.uint64(127)).astype(np.intp)
    k = (offset.view(np.int64) >> np.int64(52)).astype(np.float64)
    z = (bits - (offset & EXPONENT_FIELD)).view(np.float64)
    r = z * INV[index] - 1.0
    square = r * r
    cubic = (r * square) * (LOG_3 + r * (LOG_4 + r * (LOG_5 + r * (LOG_6 + r * (LOG_7 + r * LOG_8)))))
    return k, index, r, square * 0.5, cubic


def natural(k, index, r, half_square, cubic):
    b, e1 = fast_two_sum(k * C['region_ln2_hi'], LOGC_HI[index])
    c, e2 = fast_two_sum(b, r)
    d, e3 = fast_two_sum(c, -half_square)
    tail = ((e1 + e2) + e3) + ((LOGC_LO[index] + k * C['region_ln2_lo']) + cubic)
    return d, tail


def binary(k, index, r, half_square, cubic):
    p1 = r * C['region_inverse_ln2_cut']
    p1_rest = r * C['region_inverse_ln2_rest']
    h1 = cut_26(half_square)
    q1 = h1 * C['region_inverse_ln2_cut']
    q_rest = (half_square - h1) * C['region_inverse_ln2_cut'] + half_square * C['region_inverse_ln2_rest']
    whole = k + LOG2C_HI[index]
    c, e2 = fast_two_sum(whole, p1)
    d, e3 = fast_two_sum(c, -q1)
    tail = (e2 + e3) + ((LOG2C_LO[index] + (p1_rest - q_rest)) + cubic * C['region_inverse_ln2_hi'])
    return d, tail


def decimal(k, index, r, half_square, cubic):
    w1, e1 = fast_two_sum(LOGC_HI[index], r)
    w, e2 = fast_two_sum(w1, -half_square)
    w_lo = (e1 + e2) + (LOGC_LO[index] + cubic)
    v1 = cut_26(w)
    main = v1 * C['region_inverse_ln10_cut']
    rest = ((w - v1) * C['region_inverse_ln10_cut'] + w * C['region_inverse_ln10_rest']) + \
        w_lo * C['region_inverse_ln10_hi']
    h, e3 = fast_two_sum(k * C['region_log10_2_hi'], main)
    tail = e3 + (k * C['region_log10_2_lo'] + rest)
    return h, tail


def round_wide_to_float(value_hi, value_lo):
    """region's round_wide_to_float: two-sum, round to odd, one cast."""
    hi, lo = two_sum(value_hi, value_lo)
    bits = hi.view(np.uint64)
    inexact = (lo != 0.0).astype(np.uint64)
    adjust = inexact & ((bits & np.uint64(1)) ^ np.uint64(1))
    outward = ((lo > 0.0) == (hi > 0.0)).astype(np.uint64)
    odd = (bits + (adjust & outward)) - (adjust & (outward ^ np.uint64(1)))
    return odd.view(np.float64).astype(np.float32), hi, lo


def unsettled(result, hi, lo):
    """True where v = hi + lo may lie within the bound of an end of result's
    rounding interval. The ends are result +- half its ulp (a quarter below a
    power of two); hi - end is exact (Sterbenz) and the added lo rounds once.
    Zero results (log 1) are exact and settled."""
    f = result.astype(np.float64)
    fbits = result.view(np.uint32)
    up = np.nextafter(result, np.float32(np.inf)).astype(np.float64)
    down = np.nextafter(result, np.float32(-np.inf)).astype(np.float64)
    high_end = (f + up) * 0.5          # exact: 25 bits
    low_end = (f + down) * 0.5
    distance = np.minimum(np.abs((hi - high_end) + lo), np.abs((hi - low_end) + lo))
    limit = np.abs(hi) * SCREEN
    return (distance <= limit) & (fbits & np.uint32(0x7fffffff) != 0)


def exact_float(x, base):
    """log_base x rounded to float (nearest, ties to even), from 400-bit
    references. x is a positive normal float as a Python float."""
    value = reference.log_reference(Fraction(x))
    if base == 'binary':
        value = value / reference.log_reference(2)
    elif base == 'decimal':
        value = value / reference.log_reference(10)
    if value == 0:
        return 0.0
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - 23)
    scaled = magnitude / quantum
    whole = scaled.numerator // scaled.denominator
    rest = scaled - whole
    if rest > Fraction(1, 2) or (rest == Fraction(1, 2) and whole % 2 == 1):
        whole += 1
    result = float(whole * quantum)
    return result if value > 0 else -result


KERNELS = (('natural', natural), ('binary', binary), ('decimal', decimal))
# An independent check of the replica itself (the screen trusts v): numpy's
# double logarithms rounded to float. They differ from the correct rounding
# only within about 2^-52 of a float midpoint, so every disagreement is
# decided exactly too; a replica error would show as many wrong results.
NUMPY_LOGS = {'natural': np.log, 'binary': np.log2, 'decimal': np.log10}


def hexadecimal(text):
    return int(text, 16)


def main(arguments):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--chunk-bits', type=int, default=22)
    parser.add_argument('--first', type=hexadecimal, default=0x00800000)
    parser.add_argument('--last', type=hexadecimal, default=0x7f7fffff)
    options = parser.parse_args(arguments)
    start = time.time()
    counts = {name: 0 for name, _ in KERNELS}
    unsettled_cases = {name: [] for name, _ in KERNELS}
    wrong = []
    chunk = 1 << options.chunk_bits
    total = 0
    for low in range(options.first, options.last + 1, chunk):
        high = min(low + chunk, options.last + 1)
        xbits = np.arange(low, high, dtype=np.uint32)
        xd = xbits.view(np.float32).astype(np.float64)
        values = parts(xd)
        for name, kernel in KERNELS:
            d, tail = kernel(*values)
            result, hi, lo = round_wide_to_float(d, tail)
            disagree = result != NUMPY_LOGS[name](xd).astype(np.float32)
            where = np.nonzero(unsettled(result, hi, lo) | disagree)[0]
            counts[name] += where.size
            for position in where:
                x = float(xd[position])
                got = float(result[position])
                want = exact_float(x, name)
                unsettled_cases[name].append((x.hex(), got == want))
                if got != want:
                    wrong.append((name, x.hex(), got, want))
        total += high - low
    elapsed = time.time() - start
    print('region float logs, exhaustive screen: %d floats [0x%08x, 0x%08x], %.0f s' %
          (total, options.first, options.last, elapsed))
    print('certified bound 2^-62 relative (region_certificate.py logf)')
    for name, _ in KERNELS:
        resolved = unsettled_cases[name]
        print('  %-8s unsettled by the screen or unlike numpy: %d (decided exactly: %d correct)' %
              (name, counts[name], sum(1 for _, ok in resolved if ok)))
        for text, ok in resolved[:20]:
            print('    %s %s' % (text, 'correct' if ok else 'WRONG'))
    if wrong:
        print('FAIL: %d results not correctly rounded' % len(wrong))
        for item in wrong[:50]:
            print('  %s(%s) = %r, correctly rounded %r' % item)
        return 1
    print('every result correctly rounded')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
