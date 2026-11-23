#!/usr/bin/env python3
"""A proved bound on the error of STXlib's half-turn sine and cosine
(src/algorithms/math/half_turn.cpp: half_turn_wide, rotate_half_turn,
half_turn_rounded), for every double m with 2^-100 <= m < 2^53.

It closes what tools/math/half_turn_certificate.py leaves open: "the rounding
error of the low parts ... measured, not proved". proofs/half_turn/PROOF.md
(ported with this script from the PSFT prototype, ~/Developer/work/fft/ideal
proof/PROOF.md) is the argument; this script is its arithmetic, and nothing here samples inputs.

What is deduced, with exact rationals throughout:

  1. The table. The 256 cells are rebuilt here by the operations of
     make_half_turn_cells (binary64, round to nearest, one rounding per
     operation, as Python's float is). For each cell the distance of
     anchor + low from the true sin or cos of q pi/128, of slope + slope_low
     from pi cos or -pi sin, and of the whole_sine / whole_cosine sums, is
     bounded from certified intervals for pi and for the sines and cosines.
     The lattice facts of the exact head are re-checked on the rebuilt cells.
  2. The low part. For every cell and every range D_lo < |d| <= D_hi of the
     reduced argument (ranges covering 2^-100 <= |d| <= 2^-8), each operation of
     rotate_half_turn is followed with a bound on the magnitude of its
     result and on its distance from the real quantity it stands for:
     |fl(x) - x| <= 2^-53 |x| + 2^-1075 for every rounding (the second term
     covers a product that underflows), the stored coefficients' own
     rounding, and the two truncated series' remainders. The sum is E, a
     bound on |computed low - exact low|.
  3. The result. With V the true value and ulp(V) its unit in the last
     place, the lemma of PROOF.md gives |fl(head + low) - V| <= ulp(V)/2 + E
     when E < ulp(V). A lower bound on |V| over the cell and the range gives
     ulp(V), and the largest E / ulp(V) over all cells and ranges is the
     excess over half a unit.
  4. Lattice points (d = 0): the table's head is the correctly rounded value
     and head + low rounds to it.

A transform's twiddle arguments, x = 2a/N, are doubles in this range, so the
bound covers them.

Usage: half_turn_low_certificate.py [STXLIB_ROOT]   (default: this checkout)
Only the Python standard library.
"""
import math
import pathlib
import re
import sys
from fractions import Fraction as F

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2])
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sincos_certificate as sc  # certified pi, sin and cos intervals  # noqa: E402

failures = []


def require(condition, message):
    if not condition:
        failures.append(message)
        print('FAIL: ' + message)


U = F(1, 2 ** 53)
ETA = F(1, 2 ** 1075)
PI_LO, PI_HI = sc.PI_LOW, sc.PI_HIGH


def up(value, bits=80):
    """An upper bound on value >= 0 with a short numerator and denominator."""
    value = F(value)
    if value == 0:
        return value
    shift = bits - (value.numerator.bit_length() - value.denominator.bit_length())
    scaled = value * F(2) ** shift
    return F(math.ceil(scaled)) / F(2) ** shift


def down(value, bits=80):
    value = F(value)
    if value <= 0:
        return F(0)
    shift = bits - (value.numerator.bit_length() - value.denominator.bit_length())
    scaled = value * F(2) ** shift
    return F(math.floor(scaled)) / F(2) ** shift


def magnitude(interval):
    return max(abs(interval[0]), abs(interval[1]))


def distance(value, interval):
    """An upper bound on |value - t| for t in the interval."""
    return max(abs(value - interval[0]), abs(value - interval[1]))


def ulp_of(lower):
    """ulp of any real of magnitude at least `lower` > 0 (binary64, normal)."""
    exponent = lower.numerator.bit_length() - lower.denominator.bit_length()
    if F(2) ** exponent > lower:
        exponent -= 1
    return F(2) ** (exponent - 52)


# ---- 1. the table, rebuilt ----------------------------------------------------

def read_table():
    text = (ROOT / 'src/algorithms/math/sincos_table256.hpp').read_text()
    rows = re.findall(r'^\s*\{(-?0x[^,]+), (-?0x[^,]+), (-?0x[^,]+), (-?0x[^}]+)\},?\s*$', text, re.M)
    require(len(rows) == 256, 'sincos_table256.hpp: expected 256 entries, found %d' % len(rows))
    return [tuple(float.fromhex(v) for v in row) for row in rows]


def constant(path, name):
    text = (ROOT / path).read_text()
    match = re.search(name + r' = (-?0x[0-9a-fp.+-]+);', text)
    require(match is not None, '%s: %s not found' % (path, name))
    return float.fromhex(match.group(1))


PI_HEAD = constant('src/algorithms/math/atan_table128.hpp', 'atan_pi_hi')
PI_TAIL = constant('src/algorithms/math/atan_table128.hpp', 'atan_pi_lo')
ROUNDING_SHIFT = constant('src/algorithms/math/kernel_support.hpp', 'rounding_shift')
SPLITTER = 134217729.0  # 2^27 + 1 (float_exact.hpp)


def veltkamp(a):
    c = SPLITTER * a
    hi = c - (c - a)
    return hi, a - hi


def lattice_52(value):
    return -((-value + 1.0) - 1.0) if value < 0.0 else (value + 1.0) - 1.0


def times_pi_constant(hi, lo):
    p_hi, p_lo = veltkamp(PI_HEAD)
    h_hi, h_lo = veltkamp(hi)
    product = hi * PI_HEAD
    error = ((h_hi * p_hi - product) + h_hi * p_lo + h_lo * p_hi) + h_lo * p_lo
    return product, error + (hi * PI_TAIL + lo * PI_HEAD)


def slope_parts(hi, lo):
    head = ((hi * 2.0 ** 22 + ROUNDING_SHIFT) - ROUNDING_SHIFT) * 2.0 ** -22
    return head, (hi - head) + lo


class Cell:
    pass


def build_cells(table):
    cells = []
    for index, (s, c, s_lo, c_lo) in enumerate(table):
        cell = Cell()
        cell.index = index
        cell.entry = (s, c, s_lo, c_lo)
        sine, cosine = lattice_52(s), lattice_52(c)
        sine_low, cosine_low = (s - sine) + s_lo, (c - cosine) + c_lo
        sine_slope = slope_parts(*times_pi_constant(c, c_lo))
        cosine_slope = slope_parts(*times_pi_constant(-s, -s_lo))
        true_s, true_c = sc.sin_cos_of_step_multiple(index)
        pi_c = (min(PI_LO * true_c[0], PI_HI * true_c[0], PI_LO * true_c[1], PI_HI * true_c[1]),
                max(PI_LO * true_c[0], PI_HI * true_c[0], PI_LO * true_c[1], PI_HI * true_c[1]))
        pi_s = (min(-PI_LO * true_s[0], -PI_HI * true_s[0], -PI_LO * true_s[1], -PI_HI * true_s[1]),
                max(-PI_LO * true_s[0], -PI_HI * true_s[0], -PI_LO * true_s[1], -PI_HI * true_s[1]))
        whole_sine, whole_cosine = sine + sine_low, cosine + cosine_low
        # Each coordinate: (anchor, low, slope, slope_low, true value interval,
        #                   true slope interval), and the errors of step 1.
        cell.coordinates = {}
        for name, anchor, low, slope, true, true_slope in (
                ('sin', sine, sine_low, sine_slope, true_s, pi_c),
                ('cos', cosine, cosine_low, cosine_slope, true_c, pi_s)):
            co = Cell()
            co.anchor, co.low, co.slope, co.slope_low = F(anchor), F(low), F(slope[0]), F(slope[1])
            co.true = true
            co.tau = up(distance(co.anchor + co.low, true))            # |anchor + low - true|
            co.sigma = up(distance(co.slope + co.slope_low, true_slope))  # |slope + slope_low - true slope|
            cell.coordinates[name] = co
        cell.whole_sine, cell.whole_cosine = F(whole_sine), F(whole_cosine)
        cell.omega_sine = up(distance(cell.whole_sine, true_s))
        cell.omega_cosine = up(distance(cell.whole_cosine, true_c))
        cell.true_s, cell.true_c = true_s, true_c
        cells.append(cell)
    return cells


def check_lattice(cells):
    for cell in cells:
        for name, co in cell.coordinates.items():
            require((co.anchor * 2 ** 52).denominator == 1 and abs(co.anchor) <= 1,
                    'cell %d %s: anchor off the 2^-52 lattice' % (cell.index, name))
            require((co.slope * 2 ** 22).denominator == 1 and abs(co.slope) * 2 ** 22 < 2 ** 24,
                    'cell %d %s: slope off the 2^-22 lattice or 4 or more' % (cell.index, name))
            if cell.index % 64 == 0:
                require(co.anchor == 0 or co.slope == 0, 'cell %d %s: axis cell with anchor and slope' % (cell.index, name))
                require(abs(co.anchor) in (0, 1), 'cell %d %s: axis anchor not 0 or 1' % (cell.index, name))
                require(co.low == 0, 'cell %d %s: axis anchor has a low part' % (cell.index, name))
    require(len(cells) == 256, 'expected 256 cells')
    print('table: %d cells rebuilt;' % len(cells) + ' anchors on 2^-52, slopes on 2^-22 below 4, axis cells as the head needs')


def check_lattice_points(cells):
    """d = 0: half_turn_wide returns the table's (s, s_lo); the caller adds them."""
    for cell in cells:
        s, c, s_lo, c_lo = cell.entry
        for value, low, true in ((s, s_lo, cell.true_s), (c, c_lo, cell.true_c)):
            require(float(true[0]) == value and float(true[1]) == value, 'cell %d: table head not correctly rounded' % cell.index)
            require(value + low == value, 'cell %d: head + low does not round to the head' % cell.index)
    print('lattice points: all 512 table values correctly rounded, and head + low rounds to the head')


# ---- 2. the low part ----------------------------------------------------------

class Q:
    """A computed double: mag bounds its magnitude, err its distance from the
    real quantity it stands for."""
    __slots__ = ('mag', 'err')

    def __init__(self, mag, err=F(0)):
        self.mag, self.err = mag, err


def mul(a, b):
    exact = a.mag * b.mag
    err = a.err * b.mag + (a.mag + a.err) * b.err + U * exact + ETA
    return Q(up(exact * (1 + U) + ETA), up(err))


def add(a, b):
    exact = a.mag + b.mag
    return Q(up(exact * (1 + U)), up(a.err + b.err + U * exact))


def coefficient(k):
    name = 'half_turn_%s_%d' % ('cos' if k % 2 == 0 else 'sin', k)
    stored = F(constant('src/algorithms/math/half_turn.cpp', 'constexpr double ' + name))
    sign = -1 if (k // 2) % 2 == 1 else 1
    true = (sign * PI_LO ** k / math.factorial(k), sign * PI_HI ** k / math.factorial(k))
    return Q(abs(stored), up(distance(stored, true)))


COEFFICIENT = {k: coefficient(k) for k in range(2, 9)}
PI_UP = up(PI_HI, 40)
PI_DOWN = down(PI_LO, 40)


def series(d_high):
    """cubic and loss of rotate_half_turn for |d| <= d_high, each against the
    full sin(pi d) - pi d and cos(pi d) - 1 (truncation included in err)."""
    d = Q(d_high)
    square = mul(d, d)
    inner = add(COEFFICIENT[3], mul(square, add(COEFFICIENT[5], mul(square, COEFFICIENT[7]))))
    cubic = mul(mul(d, square), inner)
    loss = mul(square, add(COEFFICIENT[2], mul(square, add(COEFFICIENT[4], mul(
        square, add(COEFFICIENT[6], mul(square, COEFFICIENT[8])))))))
    angle = PI_UP * d_high
    cubic.err = up(cubic.err + angle ** 9 / math.factorial(9))
    loss.err = up(loss.err + angle ** 10 / math.factorial(10))
    return cubic, loss


def low_error(cell, name, d_high, cubic, loss):
    """E: a bound on |computed low - exact low| for one coordinate."""
    co = cell.coordinates[name]
    axis = cell.index % 64 == 0
    if axis:
        d2_high = d_high / 2 ** 25                      # d - cut_26(d)
    else:
        d2_high = min(d_high, F(1, 2 ** 31))            # d - (d to the nearest 2^-30)
    d = Q(d_high)
    cross = add(mul(Q(abs(co.slope)), Q(d2_high)), mul(Q(abs(co.slope_low)), d))
    cross.err = up(cross.err + co.sigma * d_high)       # (slope + slope_low) d against the true slope times d
    whole_sine = Q(abs(cell.whole_sine), cell.omega_sine)
    whole_cosine = Q(abs(cell.whole_cosine), cell.omega_cosine)
    if name == 'sin':
        second = add(mul(whole_cosine, cubic), mul(whole_sine, loss))
    else:
        second = add(mul(whole_cosine, loss), mul(whole_sine, cubic))   # a difference: same bounds
    low = add(Q(abs(co.low), co.tau), add(cross, second))
    return low.err


def value_floor(cell, name, d_low, d_high):
    """A lower bound on |V| for D_lo < |d| <= D_hi."""
    own = cell.true_s if name == 'sin' else cell.true_c
    other = cell.true_c if name == 'sin' else cell.true_s
    angle_high = PI_UP * d_high
    if magnitude(own) == 0:
        # V = +-sin(pi d): at least pi d (1 - (pi d)^2 / 6), increasing on the range.
        angle_low = PI_DOWN * d_low
        return down(angle_low * (1 - angle_low ** 2 / 6))
    own_low = min(abs(own[0]), abs(own[1]))
    # |own cos(pi d) -+ other sin(pi d)| >= |own| (1 - (pi d)^2/2) - |other| pi d.
    return down(own_low * (1 - angle_high ** 2 / 2) - magnitude(other) * angle_high)


def ranges():
    """D_lo < |d| <= D_hi covering 2^-100 <= |d| <= 2^-8, eight to a binade."""
    for k in range(8, 101):
        top = F(1, 2 ** k)
        for j in range(8, 16):
            yield top * j / 16, top * (j + 1) / 16


def certify(cells):
    worst = (F(0), None)
    for d_low, d_high in ranges():
        cubic, loss = series(d_high)
        for cell in cells:
            for name in ('sin', 'cos'):
                error = low_error(cell, name, d_high, cubic, loss)
                floor = value_floor(cell, name, d_low, d_high)
                require(floor > 0, 'cell %d %s: no positive lower bound on the value' % (cell.index, name))
                if floor <= 0:
                    continue
                unit = ulp_of(floor)
                require(error < unit, 'cell %d %s: E reaches one ulp' % (cell.index, name))
                excess = error / unit
                if excess > worst[0]:
                    worst = (excess, (cell.index, name, d_low, d_high, error, unit))
    return worst


def report(label, worst):
    excess, (index, name, d_low, d_high, error, unit) = worst
    print('%s: error at most (1/2 + %.3g) ulp = %.6f ulp' % (label, float(excess), 0.5 + float(excess)))
    print('    worst: cell %d, %s, %.4g < |d| <= %.4g; E <= 2^%.2f, ulp(V) >= 2^%d'
          % (index, name, float(d_low), float(d_high), sc.log2(error), round(sc.log2(unit))))


def main():
    cells = build_cells(read_table())
    check_lattice(cells)
    check_lattice_points(cells)
    print('table errors: anchor + low within 2^%.1f, slope + slope_low within 2^%.1f, of the true values'
          % (max(sc.log2(co.tau) for cell in cells for co in cell.coordinates.values() if co.tau),
             max(sc.log2(co.sigma) for cell in cells for co in cell.coordinates.values() if co.sigma)))
    report('every double, 2^-100 <= m < 2^53', certify(cells))
    if failures:
        print('%d check(s) failed' % len(failures))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
