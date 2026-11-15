#!/usr/bin/env python3
"""Error certificate for the trigonometric region kernels
(src/algorithms/math/trig_region.cpp): sincos and tan of a double within the
library's stated bound (0.51 ulp) for every double 2^-27 <= |x| <= 2^32.

The region kernels change three things against sincos_core.hpp and tan.cpp:
the polynomial chains for sin r - r and tan r - r (Estrin's scheme), the
table layout (one line per index: the same values, so no proof is touched),
and, for 2^-27 <= |x| < 2^6, the reduction (pi/128 in 41-, 38- and 53-bit
parts). This tool reuses tools/math/sincos_certificate.py and:

  1. replaces its model of the two polynomial chains with the region
     kernels' (every rounding counted as there), so its step 2 recomputes
     the core's error and the reduction contract for the region rotation;
  2. reruns that certificate's steps 3 and 4 against the new contract: the
     library's reductions, which serve |x| >= 2^6, still meet it;
  3. proves the region reduction within the new contract in every binade
     below 2^6, at each binade's certified minimum remainder.

The certified minimum remainders come from sincos_certificate.py step 1:
from its complete search when --search names the built
tools/math/sincos_minimum_remainder.cpp, otherwise from the worst-case table
that search wrote (tests/reference/sincos_worst_cases.hpp: per binade, the
double attaining the minimum and its exact remainder), lowered by 2^-50 of
itself to cover the search's fixed-point error (2^-128 absolute).

Usage:
  trig_region_certificate.py [--search PROGRAM]
"""

import pathlib
import re
import sys
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sincos_certificate as base  # noqa: E402

ROOT = base.ROOT
REGION_PATH = ROOT / 'src' / 'algorithms' / 'math' / 'trig_region.cpp'
WORST_PATH = ROOT / 'tests' / 'reference' / 'sincos_worst_cases.hpp'
U = base.U
require = base.require
Value = base.Value
add = base.add
subtract = base.subtract
multiply = base.multiply
constant = base.constant


def read_region():
    text = open(REGION_PATH).read()

    def named(name):
        match = re.search(r'inline constexpr double ' + name + r' = (-?0x[0-9a-fp.+-]+);', text)
        require(match is not None, 'trig_region.cpp: constant ' + name + ' not found')
        return base.exact(match.group(1))

    for spelling in ('const double sin_polynomial = (sin_3 + r_squared * sin_5) + r_fourth * sin_7;',
                     'const double sin_r_minus_r = r_lo + r_cubed * sin_polynomial;',
                     'const double cos_r_minus_1 = r_fourth * (cos_4 + r_squared * cos_6) - r_squared * cos_half;',
                     'const double tan_low = tan_3 + r_squared * tan_5;',
                     'const double tan_middle = tan_7 + r_squared * tan_9;',
                     'const double tan_polynomial = tan_low + r_fourth * (tan_middle + r_fourth * tan_11);',
                     'const double t_minus_r = r_cubed * tan_polynomial + r_lo * (1.0 + r_squared);',
                     'const double r1 = x - q * trig_region_step_1;',
                     'two_sum(r1, -(q * trig_region_step_2), r2, r2_error);',
                     'const double low = r2_error - q * trig_region_step_3;',
                     'fast_two_sum(r2, low, r, r_lo);'):
        require(spelling in text, 'trig_region.cpp no longer spells ' + spelling)
    parts = [named('trig_region_step_%d' % i) for i in (1, 2, 3)]
    limit = named('trig_region_limit')
    return parts, limit


REGION_PARTS, REGION_LIMIT = read_region()


# ---- 1. the region rotation's polynomial chains ---------------------------

def region_polynomials(r_low, r_high, r_lo_max):
    """As sincos_certificate.reduced_polynomials, for trig_region_rotate:
    r3 = fl(r r2), r4 = fl(r2 r2), sin r - r = r_lo + r3 ((s3 + r2 s5) + r4 s7);
    cos r - 1 is the library's expression."""
    r_magnitude = base.magnitude_interval(r_low, r_high)
    r_lo_value = Value(-r_lo_max, r_lo_max)
    r_value = Value(r_low, r_high)
    squares = (r_low * r_low, r_high * r_high)
    square_low = Fraction(0) if r_low <= 0 <= r_high else min(squares)
    r2 = base.rounded(square_low, max(squares), Fraction(0))
    co = base.COEFFICIENTS
    r4 = multiply(r2, r2)
    r3 = multiply(r_value, r2)
    p = add(add(constant(co['sin_3']), multiply(r2, constant(co['sin_5']))), multiply(r4, constant(co['sin_7'])))
    sp = add(r_lo_value, multiply(r3, p))
    b = multiply(r2, constant(co['cos_6']))
    b = add(constant(co['cos_4']), b)
    a = multiply(r4, b)
    half = Value(r2.low / 2, r2.high / 2, r2.error / 2)
    cp = subtract(a, half)
    m = r_magnitude
    rho = r_lo_max
    model_sp = (rho * m * m / 2 + m * rho * rho / 2 + rho ** 3 / 6 +
                m ** 3 * abs(Fraction(-1, 6) - co['sin_3']) + m ** 5 * abs(Fraction(1, 120) - co['sin_5']) +
                m ** 7 * abs(Fraction(-1, 5040) - co['sin_7']) + m ** 9 / 362880)
    model_cp = (rho * m + rho * rho / 2 + m ** 4 * abs(Fraction(1, 24) - co['cos_4']) +
                m ** 6 * abs(Fraction(-1, 720) - co['cos_6']) + m ** 8 / 40320)
    sp_true_max = rho + (m + rho) ** 3 / 6
    cp_true_max = (m + rho) ** 2 / 2
    return r_value, r2, sp, cp, model_sp, model_cp, sp_true_max, cp_true_max


def region_tan_errors(k, r_low, r_high, r_lo_max):
    """sincos_certificate.tan_errors for trig_region_tan_rotate: t - r =
    r3 ((t3 + t5 r2) + r4 ((t7 + t9 r2) + t11 r4)) + r_lo (1 + r2)."""
    s, c, s_lo, c_lo = base.TABLE[k]
    sigma_s, sigma_c = base.table_sigmas(k)
    r_value, r2, _, _, _, _, _, _ = region_polynomials(r_low, r_high, r_lo_max)
    rho = r_lo_max
    co = base.COEFFICIENTS
    r4 = multiply(r2, r2)
    r3 = multiply(r_value, r2)
    low_part = add(constant(co['tan_3']), multiply(r2, constant(co['tan_5'])))
    middle = add(constant(co['tan_7']), multiply(r2, constant(co['tan_9'])))
    inner = add(middle, multiply(r4, constant(co['tan_11'])))
    polynomial = add(low_part, multiply(r4, inner))
    one_plus = add(constant(Fraction(1)), r2)
    t_minus_r = add(multiply(r3, polynomial), multiply(Value(-rho, rho), one_plus))
    m, r2_cut = base.cut_parts(r_low, r_high)
    model_t = (sum(abs(base.TAN_TRUE[i] - co[base.TAN_NAMES[i]]) * m ** (2 * i + 3) for i in range(5)) +
               base.TAN_NEXT * m ** 13 * Fraction(11, 10) + rho * m ** 4 + 2 * rho * rho * (m + rho))
    t_true_max = rho * (1 + (m + rho) ** 2) + (m + rho) ** 3 / 3 * Fraction(11, 10)
    s1, s2 = base.table_cut(s, s_lo)
    c1, c2 = base.table_cut(c, c_lo)
    rotation = abs(c1) * m
    require(s == 0 or abs(s) >= rotation, 'fast_two_sum precondition (tan numerator) at k = %d' % k)
    head_error = base.half_ulp(abs(s) + rotation)
    cross = add(multiply(constant(c1), r2_cut), multiply(c2, r_value))
    n_tail = add(add(Value(-head_error, head_error), add(cross, multiply(constant(c), t_minus_r))),
                 constant(s_lo))
    n_error = (n_tail.error + abs(c) * model_t + sigma_s + sigma_c * m +
               (abs(c_lo) + sigma_c) * t_true_max)
    rotation = abs(s1) * m
    require(c == 0 or abs(c) >= rotation, 'fast_two_sum precondition (tan denominator) at k = %d' % k)
    head_error = base.half_ulp(abs(c) + rotation)
    cross = add(multiply(constant(s1), r2_cut), multiply(s2, r_value))
    d_tail = add(subtract(Value(-head_error, head_error), add(cross, multiply(constant(s), t_minus_r))),
                 constant(c_lo))
    d_error = (d_tail.error + abs(s) * model_t + sigma_c + sigma_s * m +
               (abs(s_lo) + sigma_s) * t_true_max)
    return n_error, d_error


# ---- certified minimum remainders without the search program -----------------

def table_worst_remainders(all_binades):
    """Per binade, the 'any q' class only: (lower bound, x, q) from the
    worst-case table, or None where the table has no case (q = 0 only)."""
    text = open(WORST_PATH).read()
    cases = re.findall(r'\{(-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+), (-?0x[0-9a-fp.+-]+),', text)
    require(len(cases) > 0, 'no cases in ' + str(WORST_PATH))
    parsed = [(base.exact(x), base.exact(q), base.exact(hi) + base.exact(lo)) for x, q, hi, lo in cases]
    any_class = base.CLASSES[0][2]
    worst = {}
    for binade in all_binades:
        best = None
        for x, q, remainder in parsed:
            if binade.x_low <= x <= binade.x_high:
                lower = abs(remainder) * (1 - Fraction(1, 2 ** 50))
                if best is None or lower < best[0]:
                    best = (lower, x, int(q))
        worst[binade] = {any_class: best}
    return worst


# ---- 3. the region reduction ---------------------------------------------------

def region_binades(all_binades):
    return [b for b in all_binades if b.path == 'fast' and b.x_high < REGION_LIMIT]


def region_reduction_error(binade, rho=None):
    """trig_region_reduce's |(r + r_lo) - (x - q pi/128)| over the binade's
    inputs with |x - q pi/128| <= rho: r1 = x - q p1 exact, two_sum exact,
    low = fl(e2 - fl(q p3)), the fast two-sum exact (checked separately)."""
    q_max, remainder_max = base.q_bound(binade)
    if rho is not None:
        remainder_max = min(remainder_max, rho)
    if q_max == 0:
        return Fraction(0)
    p1, p2, p3 = REGION_PARTS
    require(base.product_exact(q_max, p1) and base.product_exact(q_max, p2),
            'region reduction: q * part_1 or q * part_2 not exact in ' + binade.name())
    granule = min(Fraction(2) ** binade.s, base.significant_bits(p1)[1])
    r1_max = remainder_max + q_max * base.tail_bound([p1])
    require(base.fits_exactly(r1_max, granule), 'region reduction: x - q part_1 not exact in ' + binade.name())
    r2_max = (remainder_max + q_max * base.tail_bound([p1, p2])) * (1 + U)
    product_3 = q_max * abs(p3)
    e2_max = U * r2_max
    low_max = (e2_max + product_3 * (1 + U)) * (1 + U)
    truncation = q_max * base.tail_bound([p1, p2, p3])
    return truncation + U * product_3 + U * low_max


def fast_two_sum_holds(binade, r_min):
    """|low| <= |r2| for every input of the binade with |remainder| >= r_min."""
    q_max, _ = base.q_bound(binade)
    if q_max == 0:
        return True
    p1, p2, p3 = REGION_PARTS
    r2_min = (r_min - q_max * base.tail_bound([p1, p2])) * (1 - U)
    # |low| <= (u |r2| + q|p3| (1 + u)) (1 + u): below |r2| when q|p3| (1 + u)^2
    # <= |r2| (1 - u (1 + u)).
    return q_max * abs(p3) * (1 + U) ** 2 <= r2_min * (1 - U * (1 + U))


def main(arguments):
    captured = {}
    original_core = base.core_certificate

    def core_capture(r_min, r_max):
        result = original_core(r_min, r_max)
        captured['core'] = result
        return result

    base.reduced_polynomials = region_polynomials
    base.tan_errors = region_tan_errors
    base.core_certificate = core_capture
    if '--search' in arguments:
        base_arguments = ['--search', arguments[arguments.index('--search') + 1]]
        searched = {}
        original_worst = base.worst_remainders

        def worst_capture(all_binades):
            result = original_worst(all_binades)
            searched['worst'] = result
            return result
        base.worst_remainders = worst_capture
    else:
        base_arguments = []
        searched = None
        base.worst_remainders = table_worst_remainders
    print('== the library certificate, steps 1-4, with the region rotation\'s chains')
    status = base.main(base_arguments)
    _, kappa, general = captured['core']
    if searched is not None:
        worst = searched['worst']
        all_binades = list(worst.keys())
    else:
        all_binades = base.binades()
        worst = table_worst_remainders(all_binades)
    any_class = base.CLASSES[0][2]

    def contract(rho):
        return min(kappa * rho, general)

    print('== the region reduction (2^-27 <= |x| < 2^%d): parts of %s bits, pi/128 held to %.1f bits'
          % (round(base.log2(REGION_LIMIT)), '/'.join(str(base.significant_bits(p)[0]) for p in REGION_PARTS),
             -base.log2(base.tail_bound(REGION_PARTS) / base.STEP_LOW)))
    worst_margin = None
    for binade in region_binades(all_binades):
        item = worst[binade][any_class]
        if item is None:
            if base.q_bound(binade)[0] == 0:
                continue                      # q = 0 only: r = x exactly
            # No table case: the binade lies below pi/128, so every q >= 1
            # leaves |x - q pi/128| >= pi/128 - x_high.
            require(binade.x_high < base.STEP_LOW, 'region binade without a certified minimum: ' + binade.name())
            r_min = base.STEP_LOW - binade.x_high
        else:
            r_min = item[0]
        r_cap = base.q_bound(binade)[1]
        require(fast_two_sum_holds(binade, r_min), 'region reduction: fast two-sum precondition in ' + binade.name())
        low = r_min
        while low < r_cap:
            high = min(2 * low, r_cap)
            error_high = region_reduction_error(binade, high)
            require(error_high <= contract(low) and error_high <= contract(high),
                    'region reduction exceeds the contract in %s near |r| = 2^%.1f' % (binade.name(),
                                                                                       base.log2(low)))
            low = high
        margin = base.log2(contract(r_min)) - base.log2(region_reduction_error(binade, 2 * r_min))
        worst_margin = margin if worst_margin is None else min(worst_margin, margin)
    print('region reduction within the contract in every binade below 2^6, from each certified minimum up, '
          'margin at least 2^%.1f' % worst_margin)
    if status or base.failures:
        print('%d failure(s)' % len(base.failures))
        return 1
    print('composed bound: region sincos and tan within %.2f ulp for every double 2^-27 <= |x| <= 2^32'
          % float(base.STATED_BOUND_ULPS))
    print('trig region certificate holds')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
