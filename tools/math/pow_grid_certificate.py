#!/usr/bin/env python3
"""Error budget of pow(double)'s reciprocal-grid kernel
(src/algorithms/math/pow.cpp, pow_positive; table
src/algorithms/math/pow_grid_table1024.hpp).

Exact rational upper bounds, in the manner of tools/math/region_certificate.py,
for normal arithmetic under round to nearest (unit roundoff U = 2^-53). The
kernel is followed operation by operation; each rounding contributes at most
U times a bound on the magnitude it rounds, and products of several
roundings are bounded by (1 + U)^n - 1.

  1. The logarithm, per cell. Every magnitude from r^2 through the low part
     is divided by |L|, L = log2 x, over the cell's doubles and every k, so
     the bound is relative to L, the quantity |y| multiplies. The smallest
     |L| of a cell is at k = 0 or k = -1 (ln b >= (b - 1)/b and
     -ln b >= 1 - b bound the nonzero distances; |log2| >= |ln| since
     ln 2 < 1). The two unity cells (n = 1024 and n = 512) reduce x near 1
     to r = x - 1 or x/2 - 1, where |r / L| <= (1 + |r|) ln 2. The stored
     head + tail is checked against tools/math/region_reference.py's
     logarithm (error below 2^-380), independent of the generator's.
  2. The product with y: p = y log_hi exactly, p_lo = p_error + y log_lo
     rounded; |y L| <= 1025 at every finite normal result.
  3. The exponential: t = t_head + p_lo rounded, u = t ln2_hi rounded,
     e^u - 1 - u to the u^5 term (Taylor remainder and the roundings of
     each monomial counted), the table's own error (region_certificate's
     EXP_TABLE_ERROR), the reconstruction head + tail.
  4. The composition, in ulps of a normal result: 1/2 for the final
     rounding plus 2^53 times the relative error before it.

What this does not cover, and pow.cpp says so: the correspondence of this
model with the compiled code is argued, not machine checked; subnormal
results (scale_wide rounds them once on the subnormal grid) and the
overflow and underflow decisions are argued in pow.cpp.

Usage:
  pow_grid_certificate.py    print the budget; exit 1 on a failed check

Only the Python standard library; no binary floating point in any bound
(floats appear only as exact stored constants and in printed summaries).
"""

import pathlib
import re
import sys
from fractions import Fraction

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import region_certificate as region  # noqa: E402

TABLE_PATH = HERE.parent.parent / 'src' / 'algorithms' / 'math' / 'pow_grid_table1024.hpp'
CELL_COUNT = 1024
U = Fraction(1, 2 ** 53)
ONE_PLUS_U = 1 + U
# The references' ln 2 errs by less than 2^-380; dividing by it is
# well conditioned, so 2^-370 covers every derived constant.
SLACK = Fraction(1, 2 ** 370)
# The stated bound in math.hpp, ulps of a normal result.
STATED_BOUND_ULPS = Fraction(51, 100)

failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


def rounding_growth(count):
    """(1 + U)^count - 1: the relative effect of count roundings."""
    return ONE_PLUS_U ** count - 1


def read_cells():
    pattern = r'\{(\d+), (-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+)\}'
    rows = re.findall(pattern, TABLE_PATH.read_text())
    require(len(rows) == CELL_COUNT, 'pow_grid_table1024.hpp: expected 1024 cells, found %d' % len(rows))
    cells = []
    for numerator, head, tail in rows:
        cells.append((int(numerator), Fraction(float.fromhex(head)), Fraction(float.fromhex(tail))))
    return cells


INVERSE_LN2_HI = region.LOG_CONSTANTS['log_inv_ln2_hi']
INVERSE_LN2_LO = region.LOG_CONSTANTS['log_inv_ln2_lo']
LN2 = region.LN2
INVERSE_LN2_ERROR = abs(INVERSE_LN2_HI + INVERSE_LN2_LO - 1 / LN2) + SLACK


def cell_log_error(index, numerator, head, tail):
    """Bound on |computed log_hi + log_lo - L| / |L| over cell index."""
    reciprocal = Fraction(numerator, CELL_COUNT)
    require(head * 2 ** 41 == int(head * 2 ** 41) and 0 <= head <= 1,
            'cell %d: head is not a multiple of 2^-41 in [0, 1]' % index)
    require(1 <= numerator <= CELL_COUNT, 'cell %d: numerator out of range' % index)
    z_low = 1 + Fraction(index, CELL_COUNT)
    z_high = 1 + Fraction(index + 1, CELL_COUNT) - Fraction(1, 2 ** 52)
    m = max(abs(z_low * reciprocal - 1), abs(z_high * reciprocal - 1))
    require(m < Fraction(1, 800), 'cell %d: |r| reaches %s' % (index, float(m)))
    require(m * 2 ** 52 * CELL_COUNT <= 2 ** 53, 'cell %d: |M n - 2^62| exceeds 2^53' % index)
    require(z_high * 2 ** 52 * numerator < 2 ** 63, 'cell %d: M n reaches 2^63' % index)
    unity_cell = numerator in (CELL_COUNT, CELL_COUNT // 2)
    if unity_cell:
        require(tail == 0 and head == (0 if numerator == CELL_COUNT else 1),
                'cell %d: a unity cell must store head %d and tail 0' % (index, 0 if numerator == CELL_COUNT else 1))
        # k + head = 0: x = 1 + r and |r / L| <= (1 + m) ln 2. Otherwise
        # |L| >= 1 - |log2(1 + r)| > 0.99.
        r_over_l = max((1 + m) * (LN2 + SLACK), m / Fraction(99, 100))
        table_error = Fraction(0)
        tail_over_l = Fraction(0)
    else:
        floor = min((z_low - 1) / z_low, 1 - z_high / 2)
        require(floor > 0, 'cell %d: no positive lower bound on |L|' % index)
        r_over_l = m / floor
        tail_over_l = abs(tail) / floor
        exact = -region.reference.log_reference(reciprocal) / LN2
        table_error = (abs(exact - head - tail) + SLACK) / floor
    # |r^j| / |L| <= r_over_l m^(j - 1). The r^3 polynomial (terms 3 to 8):
    # at most 13 roundings on each monomial's path, coefficients and each
    # use of the rounded r^2 included; truncation below r^9/9.
    cubic = r_over_l * m * m / (3 * (1 - m))
    cubic_error = cubic * rounding_growth(13) + r_over_l * m ** 8 / (9 * (1 - m))
    cubic_computed = cubic * ONE_PLUS_U ** 13
    square = r_over_l * m
    square_error = square * U
    # a + a_error = r - r^2/2 and r^2 + its error = r r, both exact.
    a = (r_over_l + square * ONE_PLUS_U / 2) * ONE_PLUS_U
    a_error = (r_over_l + square * ONE_PLUS_U / 2) * U
    rest = cubic_computed + square_error / 2
    a_tail = (a_error + rest * ONE_PLUS_U) * ONE_PLUS_U
    a_tail_error = cubic_error + U * (rest + a_error + rest * ONE_PLUS_U)
    whole = 1 + (r_over_l / (1 - m)) * (1 / LN2 + SLACK) + tail_over_l + table_error
    s_error = U * a * INVERSE_LN2_HI
    h_error = U * (whole + a * INVERSE_LN2_HI * ONE_PLUS_U)
    # low = ((a_tail A + a B) + tail) + (s_error + h_error): four roundings
    # on the first group, two on the second.
    scaled = a_tail * INVERSE_LN2_HI + a * abs(INVERSE_LN2_LO)
    low_roundings = rounding_growth(4) * scaled + rounding_growth(2) * (tail_over_l + s_error + h_error)
    relative = (table_error + a_tail_error * INVERSE_LN2_HI +
                (a + a_tail + a_tail_error) * INVERSE_LN2_ERROR +
                (a_tail + a_tail_error) * abs(INVERSE_LN2_LO) + low_roundings)
    # two_sum(h, low) renormalizes without error.
    return relative


def log_budget(cells):
    worst = Fraction(0)
    worst_index = 0
    for index, (numerator, head, tail) in enumerate(cells):
        relative = cell_log_error(index, numerator, head, tail)
        if relative > worst:
            worst = relative
            worst_index = index
    return worst, worst_index


def exp_budget():
    """Relative error of 2^(j/256) e^u's head + tail against the exact
    2^(t_head + p_lo) of the rounded p_lo, |t| <= 1/512 + 2^-40."""
    # |p_lo| < 2^-40 for |p| <= 1076 (the underflow guard).
    require(4 * 1077 * U < Fraction(1, 2 ** 40), 'p_lo bound')
    t_max = Fraction(1, 512) + Fraction(1, 2 ** 40)
    ln2_hi = region.EXACT_LN2_CONSTANTS[0]
    u_max = t_max * ONE_PLUS_U ** 2 * ln2_hi
    u_error = t_max * (abs(ln2_hi - LN2) + SLACK + rounding_growth(2) * ln2_hi)
    # e^u - 1 - u to the u^5 term: at most nine roundings per monomial,
    # each use of u^2 counted; Taylor remainder below u^6/720 / (1 - u).
    higher = u_max ** 2 / (2 * (1 - u_max))
    higher_error = higher * rounding_growth(9) + u_max ** 6 / (720 * (1 - u_max))
    # Normalized to the table head: the rotation's product and its sum
    # with the head are error-free pairs.
    table_low = U
    rotation_error = U * u_max
    head_error = U * (1 + u_max * ONE_PLUS_U)
    small = table_low * (1 + u_max) + higher * ONE_PLUS_U ** 9
    roundings = rounding_growth(5) * small + rounding_growth(2) * rotation_error + U * head_error
    absolute = (roundings + higher_error + table_low * higher + 2 * region.EXP_TABLE_ERROR +
                (1 + 2 * U) * u_error / (1 - u_max - u_error))
    relative = absolute / ((1 - 2 * U) * (1 - u_max - u_error))
    return relative


def main(arguments):
    if arguments:
        print(__doc__, file=sys.stderr)
        return 2
    cells = read_cells()
    log_relative, worst_index = log_budget(cells)
    print('log2 x: relative error <= %.4e (2^%.2f), worst cell %d' %
          (float(log_relative), region.log2_of(log_relative), worst_index))
    # p = y log_hi exactly; |log_lo| <= U (1 + e) |L|, |log_hi| <= (1 + U)(1 + e) |L|.
    product_bound = 1025
    absolute_p = product_bound * (log_relative + (1 + log_relative) * 4 * U ** 2)
    exp_relative = exp_budget()
    print('exp reconstruction: relative error <= %.4e (2^%.2f)' %
          (float(exp_relative), region.log2_of(exp_relative)))
    relative = absolute_p * (LN2 + SLACK) * (1 + absolute_p) + exp_relative * (1 + absolute_p)
    ulps = Fraction(1, 2) + 2 ** 53 * relative * ONE_PLUS_U
    print('composed: within %.6f ulp for normal results (stated bound %s)' %
          (float(ulps), float(STATED_BOUND_ULPS)))
    require(ulps < STATED_BOUND_ULPS, 'the composed budget exceeds the stated bound')
    failures.extend(region.failures)
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
