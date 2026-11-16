#!/usr/bin/env python3
"""The expected values of tests/glyph_turn_tests.cpp (tests/glyph_turn_expected.inc).

An oracle for src/algorithms/glyphs/turn_lattice.hpp that shares no code and no
arithmetic with it: the cell is stx_conv's compile_cell (surface_kernel.hpp) for
an interior cell without the bubble, written as that source has it, in exact
fractions of a level; the lattice, the box and the turn follow the rules at the
head of turn_lattice.hpp, in Python's whole numbers.

    python3 -I tools/turn_lattice_oracle.py > tests/glyph_turn_expected.inc
"""

from fractions import Fraction as F
import sys


def pos(x):
    return x if x > 0 else F(0)


def neg(x):
    return -x if x < 0 else F(0)


def cell(w):
    """w = y[i-3 .. i+4]; the cell lies between w[3] and w[4]. Its six controls."""
    left_m = (w[1] - 8 * w[2] + 8 * w[4] - w[5]) / F(12)
    left_q = (16 * w[4] - w[5] - 30 * w[3] + 16 * w[2] - w[1]) / F(12)
    right_m = (w[2] - 8 * w[3] + 8 * w[5] - w[6]) / F(12)
    right_q = (16 * w[5] - w[6] - 30 * w[4] + 16 * w[3] - w[2]) / F(12)
    delta = w[4] - w[3]
    raw = [left_m / 5, left_m / 5 + left_q / 20, None, right_m / 5 - right_q / 20, right_m / 5]
    raw[2] = delta - raw[0] - raw[1] - raw[3] - raw[4]
    previous_delta = w[3] - w[2]
    next_delta = w[5] - w[4]
    previous_last = left_m / 5
    next_first = right_m / 5
    room_positive = [F(0)] * 5
    room_negative = [F(0)] * 5
    head_positive = min(pos(previous_delta), pos(pos(raw[0]) - neg(previous_last)))
    head_negative = min(neg(previous_delta), pos(neg(raw[0]) - pos(previous_last)))
    for j in range(5):
        head_positive = min(head_positive, pos(raw[j]))
        head_negative = min(head_negative, neg(raw[j]))
        room_positive[j] = head_positive
        room_negative[j] = head_negative
    tail_positive = min(pos(next_delta), pos(pos(raw[4]) - neg(next_first)))
    tail_negative = min(neg(next_delta), pos(neg(raw[4]) - pos(next_first)))
    for j in (4, 3, 2, 1, 0):
        tail_positive = min(tail_positive, pos(raw[j]))
        tail_negative = min(tail_negative, neg(raw[j]))
        room_positive[j] += tail_positive
        room_negative[j] += tail_negative
    total_positive = sum(room_positive)
    total_negative = sum(room_negative)
    plus = pos(delta)
    minus = neg(delta)
    upper_base = plus + min(total_negative, 4 * plus)
    lower_base = minus + min(total_positive, 4 * minus)
    clipped = []
    down = []
    up = []
    for j in range(5):
        high = room_positive[j] + upper_base
        low = -(room_negative[j] + lower_base)
        value = min(high, max(low, raw[j]))
        clipped.append(value)
        down.append(value - low)
        up.append(high - value)
    residual = sum(clipped) - delta
    lower = residual > 0
    capacity = down if lower else up
    mass = residual if lower else -residual
    ordered = sorted(capacity)
    threshold = mass / 5
    partial = F(0)
    for taken, divisor in ((0, 4), (1, 3), (2, 2), (3, 1)):
        partial += ordered[taken]
        threshold = max(threshold, (mass - partial) / divisor)
    controls = [F(w[3])]
    for j in range(5):
        shift = min(threshold, capacity[j])
        controls.append(controls[-1] + (clipped[j] - shift if lower else clipped[j] + shift))
    assert controls[5] == w[4], "the currents do not sum to delta"
    return controls


def line_controls(values):
    """The controls along a line that stands in nothing: cells between value k-1 and k, k = 0 .. n."""
    n = len(values)
    padded = [F(0)] * 4 + [F(v) for v in values] + [F(0)] * 4
    out = []
    for k in range(n + 1):
        # w[3] = value k - 1 = padded[k + 3].
        made = cell(padded[k:k + 8])
        out.extend(made[:5])
    out.append(F(0))
    return out


def lattice(mask, width, height):
    """The kept controls of a mask (rows of bytes): (5 (h + 1) + 1) rows of 5 (w + 1) + 1."""
    stride = 5 * (width + 1) + 1
    rows = [line_controls(mask[r * width:(r + 1) * width]) for r in range(height)]
    columns = [line_controls([rows[r][x] for r in range(height)]) for x in range(stride)]
    kept = []
    for y in range(5 * (height + 1) + 1):
        for x in range(stride):
            value = columns[x][y] * 32
            size = abs(value)
            rounded = (size.numerator * 2 + size.denominator) // (2 * size.denominator)
            control = -rounded if value < 0 else rounded
            kept.append(max(-32768, min(32767, control)))
    return kept


def weights():
    binomial = [1, 5, 10, 10, 5, 1]
    table = []
    for p in range(256):
        exact = [F(binomial[k] * p ** k * (256 - p) ** (5 - k), 2 ** 26) for k in range(6)]
        row = [int(value + F(1, 2)) for value in exact]
        largest = max(range(6), key=lambda k: (row[k], -k))
        row[largest] += 16384 - sum(row)
        table.append(row)
    return table


WEIGHTS = weights()


def sine_cosine(degrees):
    one = 1 << 30
    x = (degrees * 307041569098) >> 30
    z = (x * x) >> 30
    p = one - z // 110
    for divisor in (72, 42, 20, 6):
        p = one - ((z * p) >> 30) // divisor
    s = (x * p) >> 30
    p = one - z // 132
    for divisor in (90, 56, 30, 12):
        p = one - ((z * p) >> 30) // divisor
    c = one - ((z * p) >> 30) // 2
    return (s + 8192) >> 14, (c + 8192) >> 14


def turn(degrees):
    """The cosine and sine (16.16) of turn_matrix for an angle in 65536ths of a degree."""
    quarter_turn = 90 << 16
    within = degrees % (4 * quarter_turn)
    quarter, rest = divmod(within, quarter_turn)
    if rest <= quarter_turn // 2:
        s, c = sine_cosine(rest)
    else:
        c, s = sine_cosine(quarter_turn - rest)
    sine, cosine = ((s, c), (c, -s), (-s, -c), (-c, s))[quarter]
    return cosine, sine


def turned_box(left, top, width, height, c, s, shift):
    """Every pixel whose centre is inside the box of the lattice's rectangle through the turn."""
    xs = [F(left) - F(1, 2), F(left + width) + F(1, 2)]
    ys = [F(top) + F(1, 2), F(top - height) - F(1, 2)]
    tx = [(x * c - y * s) / 65536 + F(shift[0], 64) for x in xs for y in ys]
    ty = [(x * s + y * c) / 65536 + F(shift[1], 64) for x in xs for y in ys]

    def first_inside(edge):
        centre = (edge - F(1, 2)).__floor__() + F(1, 2)
        while centre <= edge:
            centre += 1
        return int(centre - F(1, 2))

    def last_inside(edge):
        centre = (edge + F(1, 2)).__ceil__() + F(1, 2)
        while centre >= edge:
            centre -= 1
        return int(centre - F(1, 2))

    box_left = first_inside(min(tx))
    box_right = last_inside(max(tx)) + 1
    box_bottom = first_inside(min(ty))
    box_top = last_inside(max(ty)) + 1
    return box_left, box_top, box_right - box_left, box_top - box_bottom


def turned(kept, left, top, width, height, c, s, shift, box):
    stride = 5 * (width + 1) + 1
    box_left, box_top, box_width, box_height = box
    out = []
    for row in range(box_height):
        for column in range(box_width):
            x6 = (box_left + column) * 64 + 32 - shift[0]
            y6 = (box_top - row) * 64 - 32 - shift[1]
            u = ((x6 * c + y6 * s) >> 6) - (65536 * left - 32768) + 128
            v = (65536 * top + 32768) - ((y6 * c - x6 * s) >> 6) + 128
            if not (0 <= u < 65536 * (width + 1) and 0 <= v < 65536 * (height + 1)):
                out.append(0)
                continue
            p = WEIGHTS[(u >> 8) & 255]
            q = WEIGHTS[(v >> 8) & 255]
            total = 0
            for a in range(6):
                base = (5 * (v >> 16) + a) * stride + 5 * (u >> 16)
                along = sum(p[b] * kept[base + b] for b in range(6))
                total += q[a] * ((along + 8192) >> 14)
            out.append(max(0, min(255, (total + 262144) >> 19)))
    return out


class Random:
    """xorshift, as the test has it."""

    def __init__(self, seed):
        self.state = seed

    def next(self):
        mask = (1 << 64) - 1
        self.state ^= (self.state << 13) & mask
        self.state ^= self.state >> 7
        self.state ^= (self.state << 17) & mask
        return self.state


def mixed(total, value):
    """A value's low sixteen bits into a running number, as the test has it."""
    mask = (1 << 64) - 1
    total ^= value & 0xFFFF
    total ^= (total << 13) & mask
    total ^= total >> 7
    total ^= (total << 17) & mask
    return total


def random_mask(random, width, height, kind):
    out = []
    for index in range(width * height):
        value = random.next()
        if kind == 0:
            out.append(value & 0xFF)
        elif kind == 1:
            out.append(255 if (value & 3) != 0 else 0)
        else:
            out.append(255 if ((index % width) + (index // width)) % 2 == 0 else 0)
    return out


def listed(name, kind, values, per_line=16):
    lines = ["inline constexpr %s %s[%d] = {" % (kind, name, len(values))]
    for start in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[start:start + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    out = ["// Made by tools/turn_lattice_oracle.py: not to be changed by hand.", ""]

    # Cells: six values, and the five controls in 14400ths.
    cells = [
        [0, 0, 0, 0, 0, 0],
        [7, 7, 7, 7, 7, 7],
        [0, 10, 20, 30, 40, 50],
        [0, 0, 0, 255, 255, 255],
        [255, 255, 255, 0, 0, 0],
        [0, 0, 255, 255, 0, 0],
        [255, 0, 0, 0, 0, 0],
        [0, 255, 0, 255, 0, 255],
        [0, 0, 0, 255, 0, 0],
        [12, 40, 200, 230, 90, 3],
        [3, 90, 230, 200, 40, 12],
        [-500, 1200, -30, 8000, 7, -9999],
        [100, 101, 99, 100, 250, 0],
        [0, 64, 128, 255, 255, 255],
        [255, 200, 30, 20, 10, 0],
        [0, 1, 4, 9, 16, 25],
    ]
    random = Random(0x9E3779B97F4A7C15)
    for _ in range(24):
        cells.append([int(random.next() % 512) - 128 for _ in range(6)])
    flat_in = []
    flat_out = []
    for values in cells:
        made = cell([F(0)] + [F(v) for v in values] + [F(0)])
        scaled = [control * 14400 for control in made[:5]]
        assert all(value.denominator == 1 for value in scaled)
        flat_in.extend(values)
        flat_out.extend(int(value) for value in scaled)
    out.append("inline constexpr unsigned expected_cell_count = %d;" % len(cells))
    out.append(listed("expected_cell_values", "long long", flat_in, 6))
    out.append(listed("expected_cell_controls", "long long", flat_out, 5))
    out.append("")

    # A mask of five by four, its lattice whole, and it turned.
    mask = [0, 40, 255, 255, 12,
            0, 0, 255, 200, 0,
            90, 255, 255, 0, 0,
            255, 255, 130, 0, 7]
    kept = lattice(mask, 5, 4)
    out.append(listed("expected_mask", "unsigned char", mask, 5))
    out.append(listed("expected_lattice", "short", kept, 31))
    turns = []
    numbers = []
    pixels = []
    for degrees, shift in (((17 << 16), (0, 0)), ((17 << 16), (21, -40)), ((-(33 << 16)) - 4321, (63, 1)), ((200 << 16) + 77, (-130, 300)),
                           ((45 << 16), (32, 32)), (3000, (5, 0))):
        c, s = turn(degrees)
        box = turned_box(-2, 9, 5, 4, c, s, shift)
        bytes_ = turned(kept, -2, 9, 5, 4, c, s, shift, box)
        numbers.extend([degrees, c, s, shift[0], shift[1], box[0], box[1], box[2], box[3], len(pixels)])
        pixels.extend(bytes_)
        turns.append(degrees)
    out.append("// Each: the angle (65536ths of a degree), its cosine and sine, the shift, the box (left, top, width, height) and where its bytes begin.")
    out.append("inline constexpr unsigned expected_turn_count = %d;" % len(turns))
    out.append(listed("expected_turns", "long long", numbers, 10))
    out.append(listed("expected_turned", "unsigned char", pixels, 24))
    out.append("")

    # Many masks: one number for all their lattices, one for all their turns.
    random = Random(0x0123456789ABCDEF)
    lattice_sum = 0xCBF29CE484222325
    turned_sum = 0xCBF29CE484222325
    count = 0
    for number in range(36):
        width = 1 + int(random.next() % 9)
        height = 1 + int(random.next() % 9)
        mask = random_mask(random, width, height, number % 3)
        kept = lattice(mask, width, height)
        for control in kept:
            lattice_sum = mixed(lattice_sum, control)
        degrees = int(random.next() % (360 << 16))
        shift = (int(random.next() % 64), -int(random.next() % 64))
        left = int(random.next() % 21) - 10
        top = int(random.next() % 21) - 10
        c, s = turn(degrees)
        box = turned_box(left, top, width, height, c, s, shift)
        for value in box:
            turned_sum = mixed(turned_sum, value)
        for byte in turned(kept, left, top, width, height, c, s, shift, box):
            turned_sum = mixed(turned_sum, byte)
        count += 1
    out.append("inline constexpr unsigned expected_many_count = %d;" % count)
    out.append("inline constexpr unsigned long long expected_many_lattices = 0x%016xULL;" % lattice_sum)
    out.append("inline constexpr unsigned long long expected_many_turned = 0x%016xULL;" % turned_sum)
    weight_sum = 0xCBF29CE484222325
    for row in WEIGHTS:
        for value in row:
            weight_sum = mixed(weight_sum, value)
    out.append("inline constexpr unsigned long long expected_weights = 0x%016xULL;" % weight_sum)
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
