#!/usr/bin/env python3
"""Checks of the half-turn kernels (src/algorithms/math/half_turn.cpp).

  1. The series coefficients. Each constant half_turn_cos_k / half_turn_sin_k
     in half_turn.cpp is the correctly rounded double of
     (-1)^(k div 2) pi^k / k!: the value at both ends of
     tools/math/sincos_certificate.py's certified pi interval rounds to the
     stored double.
  2. The truncation. For |d| <= 1/256 the first omitted terms,
     (pi |d|)^9 / 9! of sin(pi d) - pi d and (pi |d|)^10 / 10! of
     cos(pi d) - 1, are below 2^-75 and 2^-83; the series alternate with
     decreasing terms there, so the first omitted term bounds the rest.
     sinc's series (half_turn.cpp sinc_series, a <= pi/256 or 2^-7) omits
     a^8 / 9! < 2^-68.
  3. The exact head, as a deduction over every cell rather than an
     enumeration: an anchor is a multiple of 2^-52 in [-1, 1] and a slope a
     multiple of 2^-22 below 4 in magnitude (both checked over the built
     table at compile time, half_turn.cpp half_turn_cells_valid). With d1 a
     multiple of 2^-30, |d1| <= 2^-8, slope d1 is a whole number of 2^-52
     units of magnitude below 2^24 * 2^22 = 2^46, and anchor + slope d1 a
     whole number of units below 2^52 + 2^46 < 2^53: exact in binary64. At
     the axis cells d1 = cut_26(d) has 26 significant bits and the slope at
     most 24, so the product has at most 50 and the anchor is zero.

Not covered (half_turn.cpp says so): the rounding error of the low parts,
the quotients and the squared half-angles; those are measured, not proved
here. tools/math/half_turn_low_certificate.py proves the low parts' bound
(0.5011 ulp for 2^-100 <= m < 2^53; proofs/half_turn/PROOF.md).

Usage:
  half_turn_certificate.py    print the checks; exit 1 on a failure

Only the Python standard library; exact rationals in every check.
"""

import math
import pathlib
import re
import sys
from fractions import Fraction

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sincos_certificate as sincos  # noqa: E402

SOURCE = HERE.parent.parent / 'src' / 'algorithms' / 'math' / 'half_turn.cpp'
RADIUS = Fraction(1, 256)

failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


def stored_coefficient(text, name):
    match = re.search(r'constexpr double ' + name + r' = (-?0x[0-9a-fp.+-]+);', text)
    require(match is not None, 'half_turn.cpp: %s not found' % name)
    if match is None:
        return Fraction(0)
    return Fraction(float.fromhex(match.group(1)))


def check_coefficients(text):
    for k in range(2, 9):
        kind = 'cos' if k % 2 == 0 else 'sin'
        name = 'half_turn_%s_%d' % (kind, k)
        sign = -1 if (k // 2) % 2 == 1 else 1
        stored = stored_coefficient(text, name)
        low = Fraction(float(sign * sincos.PI_LOW ** k / math.factorial(k)))
        high = Fraction(float(sign * sincos.PI_HIGH ** k / math.factorial(k)))
        require(low == stored and high == stored, '%s is not the correctly rounded value' % name)
    print('coefficients: (-1)^(k div 2) pi^k / k!, k = 2..8, correctly rounded')


def check_truncation():
    angle = sincos.PI_HIGH * RADIUS
    sine_remainder = angle ** 9 / math.factorial(9)
    cosine_remainder = angle ** 10 / math.factorial(10)
    require(sine_remainder < Fraction(1, 2 ** 75), 'sin(pi d) remainder reaches 2^-75')
    require(cosine_remainder < Fraction(1, 2 ** 83), 'cos(pi d) remainder reaches 2^-83')
    sinc_argument = max(angle, Fraction(1, 2 ** 7))
    sinc_remainder = sinc_argument ** 8 / math.factorial(9)
    require(sinc_remainder < Fraction(1, 2 ** 68), 'sinc series remainder reaches 2^-68')
    print('truncation: sin(pi d) below 2^-75 and cos(pi d) below 2^-83 (absolute), sinc below 2^-68 (relative)')


def check_head():
    slope_units = 2 ** 24        # |slope| < 4 in units of 2^-22
    d1_units = 2 ** 22           # |d1| <= 2^-8 in units of 2^-30
    anchor_units = 2 ** 52       # |anchor| <= 1 in units of 2^-52
    product_units = slope_units * d1_units
    require(product_units <= 2 ** 46, 'slope d1 needs more than 46 bits')
    require(anchor_units + product_units < 2 ** 53, 'anchor + slope d1 is not exact')
    require(24 + 26 <= 53, 'the axis product is not exact')
    require(sincos.PI_HIGH * 2 ** 22 < slope_units, 'pi rounded to 2^-22 needs more than 24 bits')
    print('head: slope d1 below 2^46 units, anchor + slope d1 below 2^53 units, axis products of 50 bits')


def main(arguments):
    if arguments:
        print(__doc__, file=sys.stderr)
        return 2
    text = SOURCE.read_text()
    check_coefficients(text)
    check_truncation()
    check_head()
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
