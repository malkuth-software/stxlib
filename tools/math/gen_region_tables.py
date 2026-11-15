#!/usr/bin/env python3
"""Generates src/algorithms/math/region_log_table128.hpp, the table and
constants of the region kernels (src/algorithms/math/region_core.hpp).

The buckets are those of log_table128.hpp (128 buckets of 2^45 bit patterns
of z in [0x1.5fp-1, 0x1.5fp0), offset by half a bucket so that 1 is the
centre of bucket 80). What changes is inv: here it has at most 10
significant bits, chosen per bucket to minimise max |z inv - 1| over the
bucket, so that

  - a float z (24 bits) times inv is exact (34 bits), and r = z inv - 1 is a
    multiple of 2^-33 with at most 26 significant bits, so r^2 is exact too;
  - a double z cut to 43 bits (z_hi) times inv is exact, and z_lo * inv is
    exact, so r + r_lo = z inv - 1 needs no exact product (no fused
    multiply-add and no Dekker split: the same operations on every target).

Bucket 80 has inv = 1 and logc = 0, which keeps log(1) = +0, log2(2^k) = k
and pow(1, y) = 1 exact.

Each entry: inv; -ln(inv) = logc_hi + logc_lo; -log2(inv) = log2c_hi +
log2c_lo with log2c_hi a multiple of 2^-41 (so k + log2c_hi is exact for
|k| < 2^11) and |log2c_lo| <= 2^-42. The references are
tools/math/region_reference.py's (error below 2^-380); each low part is the
nearest double to the rest, so logc_hi + logc_lo is within 2^-107 |logc| and
log2c_hi + log2c_lo within 2^-96 of the exact values.

Usage:
  gen_region_tables.py --write PATH   write the header
  gen_region_tables.py --check PATH   fail unless PATH is what --write makes
"""

import pathlib
import sys
from fractions import Fraction

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import region_reference as reference  # noqa: E402

LOG_OFFSET_BITS = 0x3fe6000000000000 - (1 << 44)
BUCKET_BITS = 1 << 45
INV_BITS = 10


def double_of_bits(bits):
    import struct
    return struct.unpack('<d', struct.pack('<Q', bits))[0]


def nearest(value):
    """The nearest double to a Fraction, as a Fraction (Python's int / int
    division rounds correctly)."""
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    return Fraction(value.numerator / value.denominator)


def round_to_bits(value, bits):
    """value rounded to `bits` significant bits (nearest), as a Fraction."""
    value = Fraction(value)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if Fraction(2) ** exponent > value:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - bits + 1)
    return round(value / quantum) * quantum


def significant_bits(value):
    value = Fraction(value)
    if value == 0:
        return 0
    numerator = abs(value.numerator)
    while numerator % 2 == 0:
        numerator //= 2
    denominator = value.denominator
    # value = numerator / denominator with denominator a power of two here.
    assert denominator & (denominator - 1) == 0
    return numerator.bit_length()


def bucket_range(i):
    """[z_first, z_last] (the smallest and largest double of bucket i)."""
    first = Fraction(double_of_bits(LOG_OFFSET_BITS + i * BUCKET_BITS))
    last = Fraction(double_of_bits(LOG_OFFSET_BITS + (i + 1) * BUCKET_BITS - 1))
    return first, last


def choose_inverse(i):
    """The inv of at most INV_BITS bits minimising max |z inv - 1| over the
    bucket (inv = 1 for the bucket centred on 1)."""
    first, last = bucket_range(i)
    if first <= 1 <= last:
        return Fraction(1)
    ideal = 2 / (first + last)
    centre = round_to_bits(ideal, INV_BITS)
    exponent = centre.numerator.bit_length() - centre.denominator.bit_length()
    if Fraction(2) ** exponent > centre:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - INV_BITS + 1)
    best = None
    for step in range(-2, 3):
        candidate = centre + step * quantum
        if significant_bits(candidate) > INV_BITS:
            continue
        spread = max(abs(first * candidate - 1), abs(last * candidate - 1))
        if best is None or spread < best[0]:
            best = (spread, candidate)
    return best[1]


def log2_head(value):
    """value rounded to a multiple of 2^-41."""
    quantum = Fraction(1, 2 ** 41)
    return round(value / quantum) * quantum


def split_cut(value, bits):
    """value cut (toward zero) to `bits` significant bits, as a Fraction."""
    value = Fraction(value)
    if value == 0:
        return Fraction(0)
    magnitude = abs(value)
    exponent = magnitude.numerator.bit_length() - magnitude.denominator.bit_length()
    if Fraction(2) ** exponent > magnitude:
        exponent -= 1
    quantum = Fraction(2) ** (exponent - bits + 1)
    kept = (magnitude // quantum) * quantum
    return kept if value > 0 else -kept


def hex_double(value):
    return float(value).hex() if value != 0 else '0x0p+0'


def entries():
    ln2 = reference.log_reference(2)
    result = []
    for i in range(128):
        inv = choose_inverse(i)
        logc = -reference.log_reference(inv)
        logc_hi = nearest(logc)
        logc_lo = nearest(logc - logc_hi)
        log2c = logc / ln2
        log2c_hi = log2_head(log2c)
        log2c_lo = nearest(log2c - log2c_hi)
        first, last = bucket_range(i)
        spread = max(abs(first * inv - 1), abs(last * inv - 1))
        result.append((inv, logc_hi, logc_lo, log2c_hi, log2c_lo, spread))
    return result


def constants():
    """Named constants of region_core.hpp, as (name, value, comment)."""
    ln2 = reference.log_reference(2)
    ln10 = reference.log_reference(10)
    third = Fraction(1, 3)
    third_hi = split_cut(third, 14)
    out = []
    out.append(('region_third_hi', third_hi, '1/3 cut to 14 bits: r1^3 (39 bits) * third_hi is exact'))
    out.append(('region_third_lo', nearest(third - third_hi), 'the nearest double to 1/3 - third_hi'))
    l1 = split_cut(ln2, 26)
    l2 = split_cut(ln2 - l1, 26)
    l3 = nearest(ln2 - l1 - l2)
    out.append(('region_ln2_1', l1, 'ln 2 = ln2_1 + ln2_2 + ln2_3: two 26-bit cuts and the rest'))
    out.append(('region_ln2_2', l2, ''))
    out.append(('region_ln2_3', l3, ''))
    inverse_ln2 = 1 / ln2
    i1 = nearest(inverse_ln2)
    out.append(('region_inverse_ln2_hi', i1, '1/ln 2 = hi + lo'))
    out.append(('region_inverse_ln2_lo', nearest(inverse_ln2 - i1), ''))
    c1 = split_cut(inverse_ln2, 26)
    c2 = nearest(inverse_ln2 - c1)
    out.append(('region_inverse_ln2_cut', c1, '1/ln 2 = cut + rest, the cut of 26 bits'))
    out.append(('region_inverse_ln2_rest', c2, ''))
    inverse_ln10 = 1 / ln10
    d1 = split_cut(inverse_ln10, 26)
    d2 = nearest(inverse_ln10 - d1)
    out.append(('region_inverse_ln10_cut', d1, '1/ln 10 = cut + rest, the cut of 26 bits'))
    out.append(('region_inverse_ln10_rest', d2, ''))
    out.append(('region_inverse_ln10_hi', nearest(inverse_ln10), '1/ln 10 rounded'))
    ln2_hi = split_cut(ln2, 42)
    out.append(('region_ln2_hi', ln2_hi, 'ln 2 with a 42-bit head (k * head exact for |k| < 2^11) and the rest'))
    out.append(('region_ln2_lo', nearest(ln2 - ln2_hi), ''))
    log10_2 = ln2 / ln10
    log10_2_hi = split_cut(log10_2, 42)
    out.append(('region_log10_2_hi', log10_2_hi, 'log10(2) the same way'))
    out.append(('region_log10_2_lo', nearest(log10_2 - log10_2_hi), ''))
    return out


def make_header():
    lines = []
    lines.append('#pragma once')
    lines.append('')
    lines.append('// Generated by tools/math/gen_region_tables.py. Do not edit by hand.')
    lines.append('// The region kernels\' log table (region_core.hpp): the buckets of')
    lines.append('// log_table128.hpp with an inverse of at most 10 significant bits.')
    lines.append('// -ln(inv) = logc_hi + logc_lo and -log2(inv) = log2c_hi + log2c_lo,')
    lines.append('// log2c_hi a multiple of 2^-41; references exact to below 2^-380.')
    lines.append('')
    lines.append('namespace stxlib::math::detail {')
    lines.append('')
    lines.append('struct RegionLogEntry final {')
    lines.append('    double inv;')
    lines.append('    double logc_hi;')
    lines.append('    double logc_lo;')
    lines.append('    double log2c_hi;')
    lines.append('    double log2c_lo;')
    lines.append('};')
    lines.append('')
    table = entries()
    worst = max(entry[5] for entry in table)
    lines.append('// max |z inv - 1| over every bucket: %s (below 2^-7.6).' % hex_double(nearest(worst)))
    lines.append('// 128 entries of 40 bytes: 5 KiB.')
    lines.append('alignas(64) inline constexpr RegionLogEntry region_log_table_128[128] = {')
    for i, (inv, logc_hi, logc_lo, log2c_hi, log2c_lo, _) in enumerate(table):
        separator = ',' if i < 127 else ''
        lines.append('    {%s, %s, %s, %s, %s}%s' % (hex_double(inv), hex_double(logc_hi), hex_double(logc_lo),
                                                  hex_double(log2c_hi), hex_double(log2c_lo), separator))
    lines.append('};')
    lines.append('')
    for name, value, comment in constants():
        if comment:
            lines.append('// ' + comment)
        lines.append('inline constexpr double %s = %s;' % (name, hex_double(value)))
    lines.append('')
    lines.append('} // namespace stxlib::math::detail')
    lines.append('')
    return '\n'.join(lines)


def main(arguments):
    if len(arguments) != 2 or arguments[0] not in ('--write', '--check'):
        print(__doc__)
        return 2
    text = make_header()
    path = pathlib.Path(arguments[1])
    if arguments[0] == '--write':
        path.write_text(text)
        return 0
    if not path.exists() or path.read_text() != text:
        print('FAIL: %s is not what gen_region_tables.py writes' % path)
        return 1
    print('%s matches' % path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
