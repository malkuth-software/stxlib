#!/usr/bin/env python3
"""Check exact linear heads and Taylor remainders for every half-turn cell."""
from fractions import Fraction as F
from math import factorial
from pathlib import Path
import re

from sincos_certificate import PI_HIGH

source = Path(__file__).resolve().parents[2] / "src/algorithms/math"
rows = re.findall(r"^    \{([^{}]+)\}", (source / "pi_slopes.inc").read_text(), re.M)
assert len(rows) == 256
radius = F(1, 256)
for index, row in enumerate(rows):
    sine, _, cosine, _, ds, _, dc, _ = map(lambda x: F(float.fromhex(x)), row.split(","))
    for anchor, slope in [(sine, ds), (cosine, dc)]:
        assert (anchor * 2**52).denominator == 1
        assert (slope * 2**22).denominator == 1
        # Grid d1 is a multiple of 2^-30. Both product and sum are on the
        # 2^-52 lattice inside (-2,2), hence exact in binary64.
        assert abs(slope) * radius < 2
        assert abs(anchor) + abs(slope) * radius < 2
        if index % 64 == 0:
            # The axis uses a 26-bit relative split instead, only one
            # summand survives. Its product uses at most 50 bits.
            assert not (anchor and slope)
            assert abs(int(slope * 2**22)).bit_length() + 26 <= 53

radians = PI_HIGH * radius
assert radians**9 / factorial(9) < F(1, 2**74)
assert radians**10 / factorial(10) < F(1, 2**83)
print("256 cell heads exact for |d| <= 1/256; series remainders < 2^-74 and 2^-83")
