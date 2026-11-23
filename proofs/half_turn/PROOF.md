# The half-turn sine and cosine are within 0.5011 ulp

2026-11-23. A proof of the accuracy of `stxlib::math::sincospi` (`src/algorithms/math/half_turn.cpp`), which supplies every twiddle of the PSFT. It replaces the measured figure with a proved one. `tools/math/half_turn_low_certificate.py` does the arithmetic of steps 4 to 6 in exact rationals; its output is `certificate_output.txt` in this folder. Ported from the PSFT prototype (`~/Developer/work/fft/ideal/proof/`), 2026-11-23.

## Theorem

Let m be a double with 2^-100 ≤ m < 2^53, and let V be sin(πm) or cos(πm). The routine returns a double R with

|R − V| ≤ (1/2 + 0.00109) ulp(V),

where ulp(V) = 2^(e−52) for 2^e ≤ |V| < 2^(e+1). If m is a multiple of 1/128, R is the correctly rounded value. Negative arguments follow by symmetry, which the routine applies exactly.

Every twiddle argument x = 2a/N is such a double, so the bound holds for every twiddle. For N ≤ 256 every twiddle is correctly rounded.

## Assumptions

1. Arithmetic is IEEE 754 binary64, round to nearest, ties to even.
2. Each written operation is rounded once: no contraction into fused operations (STXlib builds with `-ffp-contract=off`), and compile-time evaluation of the table rounds as run time does.
3. The certificate's intervals for π (Machin's and Gauss's formulas, checked against each other) and for sin and cos of qπ/128 (Taylor series with the tail bounded), taken from `tools/math/sincos_certificate.py`, contain the true values.

## Proof

**1. The reduction is exact.** For m < 2^44, 128m is exact and below 2^51. Adding 1.5·2^52 lands in [2^52, 2^53), where doubles are the integers, so the sum is 128m rounded to an integer q, and subtracting the constant is exact. q/128 is exact. If q = 0 then d = m. Otherwise m ≥ 1/256 and q/128 lies between m/2 and 2m, so d = m − q/128 is exact (Sterbenz). In both cases |d| ≤ 1/256. For m ≥ 2^44, m is a multiple of 2^-8 and is first reduced modulo 2 in integers, which is exact and leaves a multiple of 1/128. So the routine works with the true q and d, and V = S cos(πd) + C sin(πd) for the sine, with S and C the true sine and cosine of qπ/128; the cosine is alike.

**2. The head is exact.** With A the stored anchor, P the stored slope and d1 the cut of d, head = A + P·d1 involves no rounding. The anchors are multiples of 2^-52 in [−1, 1] and the slopes multiples of 2^-22 below 4; d1 is a multiple of 2^-30 with |d1| ≤ 2^-8; the product is then a multiple of 2^-52 below 2^-6 and the sum a multiple of 2^-52 below 2. At the four axis cells the anchor is 0 and the product of a 24-bit slope and a 26-bit d1 has at most 50 bits, or the slope is 0 and the head is ±1. The certificate re-checks these facts on all 256 cells. d2 = d − d1 is exact, being a multiple of ulp(d) no larger than |d|.

**3. What the low part must equal.** Write G(d) = sin(πd) − πd and K(d) = cos(πd) − 1. Then

L = V − head = (S − A) + (πC·d − P·d1) + C·G(d) + S·K(d).

The routine computes `low = a_low + ((P·d2 + p·d) + (wc·cubic + ws·loss))`, where a_low and p are the stored low parts of the anchor and the slope, wc and ws are the rounded sums anchor + low, and cubic and loss are the two truncated series.

**4. The table's own error.** For each cell the certificate rebuilds the stored values by the operations of the source and bounds, against the interval of the true value: |A + a_low − S| ≤ 2^-104.9, |P + p − πC| ≤ 2^-77.1, and |ws − S|, |wc − C|. These enter step 5 as known constants per cell.

**5. The low part's error, E.** Each double in the computation is carried as a pair: a bound on its magnitude and a bound on its distance from the real quantity it stands for. Two rules propagate them, with u = 2^-53:

- a product: err ≤ err_a·|b| + (|a| + err_a)·err_b + u·|a||b| + 2^-1075;
- a sum: err ≤ err_a + err_b + u·(|a| + |b|).

The term 2^-1075 covers a product that underflows; a sum never loses more than u times its exact value. Added to these are the stored series coefficients' distance from π^k/k!, the remainders of the series (below (π|d|)^9/9! and (π|d|)^10/10!, each series alternating with decreasing terms), and |P + p − πC|·|d|. The magnitudes are bounded using the upper end of a range D_lo < |d| ≤ D_hi, so the result E(cell, range) holds for every d in the range. Nothing is sampled.

**6. From E to the result.** Let z = head + low as a real number, so |z − V| ≤ E.

*Lemma.* If E < ulp(V), then |RN(z) − V| ≤ ulp(V)/2 + E.

*Proof.* Take V > 0 with 2^e ≤ V < 2^(e+1). If z lies in [2^e, 2^(e+1)], the doubles there are ulp(V) apart, so |RN(z) − z| ≤ ulp(V)/2. If z > 2^(e+1), then z − 2^(e+1) ≤ z − V ≤ E, which is less than half the spacing above 2^(e+1), so RN(z) = 2^(e+1) and |RN(z) − V| ≤ z − V ≤ E. If z < 2^e, the spacing below is ulp(V)/2, so |RN(z) − z| ≤ ulp(V)/4. In each case the claim follows. ∎

For each cell and each range the certificate bounds |V| from below: |S|(1 − (πD)²/2) − |C|πD when the anchor is not zero, and π|d|(1 − (π|d|)²/6) at the lower end of the range when it is. That gives ulp(V). The ranges are eight to a binade from 2^-8 down to 2^-100, so the lower bound stays within a factor 9/8 of the upper. The largest E/ulp(V) over the 256 cells, two functions and 744 ranges is 0.00108, at cell 1 for the sine with |d| near 1/256, where E ≤ 2^-68.85 and ulp(V) = 2^-59. E < ulp(V) holds everywhere.

**7. Lattice points.** When d = 0 the routine returns the table's value and its low part. The certificate checks that each of the 512 table values is the double nearest the true value, and that adding its low part returns it. ∎

## What the bound is and is not

- It is a worst-case forward bound: every rounding is assumed to err fully and in the same direction. The measured maximum, 0.5001 ulp, is ten times closer to one half, as expected when the roundings do not conspire.
- It does not say that every twiddle is correctly rounded. That would need a lower bound on how near cos(2πa/N) can come to the midpoint of two doubles. These are algebraic numbers of degree about N/4, and the general Diophantine bounds for such numbers (Liouville's and its successors) are many orders too weak to give it; only enumerating the twiddles of each N would settle it, and that is an evaluation.
- Outside the theorem: m below 2^-100, where the routine returns πm from an exact product and a subnormal result is rounded twice; and the derived functions (sincpi, cosm1pi, the degree forms).

## To reproduce

```sh
python3 tools/math/half_turn_low_certificate.py
```

About 45 seconds; standard library only.
