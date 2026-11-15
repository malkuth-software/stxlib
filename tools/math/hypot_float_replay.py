#!/usr/bin/env python3
"""Replays region float hypot (src/algorithms/math/region.cpp,
float_hypot_ordered with float_bits_value) in IEEE double and checks each
result against the exact correct rounding (rational comparison with the
midpoints), for the fixed float_bits_value and for round 2's masked one.

Round 2 (M4, main d5f0995) failed "hypot(float): special input differs from
its recorded result": float_bits_value masked the pattern with 0x7f7fffff,
which clears the exponent's low bit and halves every float with an odd
biased exponent (1.0 among them), so hypot(1, 0) gave 1 + 2^-23. This replay
is sampled (100,000 random pairs and a few exact cases), not a proof; the
proof is the construction argued in region.cpp.

Usage: hypot_float_replay.py
"""
import struct, math, random
from fractions import Fraction
def f32(v): return struct.unpack('<f', struct.pack('<f', v))[0]
def fbits(f): return struct.unpack('<I', struct.pack('<f', f))[0]
def ffrom(b): return struct.unpack('<f', struct.pack('<I', b))[0]
def fbv(bits, buggy):
    if buggy: finite = ffrom(bits & 0x7f7fffff)
    else: finite = ffrom(0 if bits >= 0x7f800000 else bits)
    return 2.0**128 if bits >= 0x7f800000 else finite
def two_sum(a,b):
    s=a+b; bb=s-a; return s,(a-(s-bb))+(b-bb)
def to_f32_rn(r):
    try: return f32(r)
    except OverflowError: return float('inf')
def hyp(a,b,buggy=False):
    s,se=two_sum(a*a,b*b); r=math.sqrt(s); f=to_f32_rn(r); fb=fbits(f) if f!=float('inf') else 0x7f800000
    fv=fbv(fb,buggy); up = fv < r and fb < 0x7f800000
    nb = fb+1 if up else fb-1
    m=(fv+fbv(nb,buggy))*0.5; d=(s-m*m)+se
    beyond = d>0 if up else d<0
    even = fb if fb&1==0 else nb
    chosen = even if d==0 else (nb if beyond else fb)
    return chosen
def exact_rn(a,b):
    # correctly rounded float of sqrt(a^2+b^2), ties to even, by exact comparison
    s=Fraction(a)**2+Fraction(b)**2
    r=math.sqrt(float(s)); f=to_f32_rn(r); fb=fbits(f) if f!=float('inf') else 0x7f800000
    best=None
    for c in range(max(fb-2,0), min(fb+2,0x7f800000)+1):
        lo = Fraction(fbv(c-1,False)+fbv(c,False))/2 if c>0 else Fraction(0)
        hi = (Fraction(fbv(c,False))+Fraction(fbv(c+1,False)))/2 if c<0x7f800000 else None
        if lo*lo < s and (hi is None or s < hi*hi): return c
        if lo*lo == s: return c if c%2==0 else c-1
    return None
random.seed(1); bad=0; badbug=0; n=0
pairs=[(1.0,0.0),(3.0,0.0),(3.0,4.0),(1.0,1.0)]
for i in range(100000):
    a=f32(random.uniform(0,1)*2.0**random.randint(-120,120)); b=f32(a*random.uniform(0,1)*2.0**-random.randint(0,30))
    pairs.append((max(a,b),min(a,b)))
for a,b in pairs:
    if a < 2.0**-126: continue
    n+=1; e=exact_rn(a,b)
    if hyp(a,b)!=e: bad+=1
    if hyp(a,b,True)!=e: badbug+=1
print(n,'pairs; fixed misrounds',bad,'; old mask misrounds',badbug)
