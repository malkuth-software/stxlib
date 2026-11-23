"""Write stx_fft's straight-line walk kernels (src/algorithms/fft/walk_small_f64.inc, walk_small_f32.inc).

Each kernel is the same walk as walk.inc (the same passes and cells, the same operations in the same order), written
out as single-assignment vector code for one size: the state is N/L vectors of L lanes that the compiler keeps in
registers, with no plan records and no loops. Every twiddle is read from the plan's table C. The only shuffles are
the terminal's: one layer to bring the last time bit out of the lane, one to interleave real and imaginary parts.

Ported from the walk's prototype (~/Developer/work/fft/ideal/gen_small.py, 2026-11-23); the generated text differs
only in spelling (named casts, initialised arrays, noexcept), not in any operation.

Run from the repository root:  python3 -B tools/fft/gen_walk_small.py
"""
import math
from fractions import Fraction

NQ = 0   # N of the kernel being written; the cosine table C has N/4 + 1 entries
K = "C[HALFQ]"   # cos(pi/4); HALFQ is replaced per kernel


class Emit:
    def __init__(self):
        self.lines, self.n = [], 0

    def new(self, expr):
        self.n += 1
        self.lines.append(f"    const V v{self.n} = {expr};")
        return f"v{self.n}"

    def raw(self, text):
        self.lines.append("    " + text)

    @staticmethod
    def val(i, scale=1.0):
        return f"C[{i}]" if scale == 1.0 else f"static_cast<T>({scale!r}) * C[{i}]"

    def const2(self, a, b, scale=1.0):
        if scale == 1.0 and b == a + 1:
            return f"P::ld(C + {a})"
        return f"P::ld2s(C + {a}, C + {b})" if scale == 1.0 else f"P::set2({self.val(a, scale)}, {self.val(b, scale)})"

    def const4(self, a, b, c, d, scale=1.0):
        return "P::set4(" + ", ".join(self.val(i, scale) for i in (a, b, c, d)) + ")"


def cs(turns):
    """Table positions of the cosine and the sine of an angle given in turns.
    The table holds (cos, sin) pairs for angles up to an eighth of a turn."""
    i = turns * NQ
    assert i.denominator == 1 and 0 <= i <= NQ // 4, (turns, NQ)
    a = int(i)
    if a <= NQ // 8:
        return 2 * a, 2 * a + 1
    b = NQ // 4 - a
    return 2 * b + 1, 2 * b


def rot(e, turns, a, b, swap=False, scale=1.0):
    """(r, i) = (c a - s b, s a + c b); swap exchanges c and s."""
    c, s = cs(turns)
    if swap:
        c, s = s, c
    if turns == Fraction(1, 8):
        k = f"P::set1({Emit.val(2 * (NQ // 8), scale)})"
        return e.new(f"P::mul({k}, P::sub({a}, {b}))"), e.new(f"P::mul({k}, P::add({a}, {b}))")
    C, S = f"P::set1({Emit.val(c, scale)})", f"P::set1({Emit.val(s, scale)})"
    return (e.new(f"P::fms(P::mul({C}, {a}), {S}, {b})"), e.new(f"P::fma(P::mul({S}, {a}), {C}, {b})"))


def irot(e, turns, x, y, swap=False, scale=1.0):
    """(c x + s y, c y - s x)."""
    c, s = cs(turns)
    if swap:
        c, s = s, c
    if turns == Fraction(1, 8):
        k = f"P::set1({Emit.val(2 * (NQ // 8), scale)})"
        return e.new(f"P::mul({k}, P::add({x}, {y}))"), e.new(f"P::mul({k}, P::sub({y}, {x}))")
    C, S = f"P::set1({Emit.val(c, scale)})", f"P::set1({Emit.val(s, scale)})"
    return (e.new(f"P::fma(P::mul({C}, {x}), {S}, {y})"), e.new(f"P::fms(P::mul({C}, {y}), {S}, {x})"))


def row(m, t, k):
    s, a = m - 1 - t, 0
    for bit in range(t):
        if (k >> bit) & 1:
            a |= 1 << ((m - 1 - bit) if bit <= s else bit)
    return a


def passes(m):
    h = m // 2
    plain, t, out = m - h, 0, []
    if plain & 1:
        out.append((2, t)); t += 1
    while t < plain:
        out.append((4, t)); t += 2
    if h & 1:
        out.append((2, t)); t += 1
    while t < m - 2:
        out.append((4, t)); t += 2
    return out, t


def forward(m, L=2, mag=False):
    global NQ
    n = 1 << m
    NQ = n
    e = Emit()
    st = {a: e.new(f"walk_get<P>(x + {a})") for a in range(0, n, L)}
    ps, t0 = passes(m)
    add = lambda a, b: e.new(f"P::add({a}, {b})")
    sub = lambda a, b: e.new(f"P::sub({a}, {b})")
    for radix, t in ps:
        nx = {}
        if radix == 4:
            q = n >> (t + 2)
            off = [row(m, t + 2, c << t) for c in range(4)]
            R = lambda k: row(m, t + 2, k)
            for i in range(0, q, L):
                a, b, c, d = (st[tau * q + i] for tau in range(4))
                p0, p1 = add(a, c), add(b, d)
                nx[R(0) + i], nx[R(2 << t) + i] = add(p0, p1), sub(p0, p1)
                nx[R(1 << t) + i], nx[R(3 << t) + i] = sub(a, c), sub(b, d)
            if t >= 1:
                k = 1 << (t - 1)
                base = row(m, t, k)
                for i in range(0, q, L):
                    a, b, c, d = (st[base + tau * q + i] for tau in range(4))
                    r = e.new(f"P::mul(P::set1({K}), P::sub({b}, {d}))")
                    im = e.new(f"P::mul(P::set1({K}), P::add({b}, {d}))")
                    nx[R(k) + i], nx[R(7 * k) + i] = add(a, r), add(c, im)
                    nx[R(3 * k) + i], nx[R(5 * k) + i] = sub(a, r), sub(im, c)
            top = 1 << t
            for k in range(1, top >> 1):
                ire, iim, oo, ob = row(m, t, k), row(m, t, top - k), R(k), R(top - k)
                t1, t2 = Fraction(k, 2 << t), Fraction(k, 4 << t)
                for i in range(0, q, L):
                    ar, br, cr, dr = (st[ire + tau * q + i] for tau in range(4))
                    ai, bi, ci, di = (st[iim + tau * q + i] for tau in range(4))
                    r1, i1 = rot(e, t1, cr, ci)
                    r2, i2 = rot(e, t1, dr, di)
                    laA, laB, haA, haB = add(ar, r1), add(ai, i1), sub(ar, r1), sub(i1, ai)
                    lbA, lbB, hbA, hbB = add(br, r2), add(bi, i2), sub(br, r2), sub(i2, bi)
                    r3, i3 = rot(e, t2, lbA, lbB)
                    r4, i4 = rot(e, t2, hbA, hbB, swap=True)
                    nx[oo + off[0] + i], nx[ob + off[3] + i] = add(laA, r3), add(laB, i3)
                    nx[ob + off[1] + i], nx[oo + off[2] + i] = sub(laA, r3), sub(i3, laB)
                    nx[ob + off[0] + i], nx[oo + off[3] + i] = add(haA, r4), add(haB, i4)
                    nx[oo + off[1] + i], nx[ob + off[2] + i] = sub(haA, r4), sub(i4, haB)
        else:
            q = n >> (t + 1)
            R = lambda k: row(m, t + 1, k)
            for i in range(0, q, L):
                nx[R(0) + i], nx[R(1 << t) + i] = add(st[i], st[q + i]), sub(st[i], st[q + i])
            if t >= 1:
                k = 1 << (t - 1)
                base = row(m, t, k)
                for i in range(0, q, L):
                    nx[R(k) + i], nx[R(3 * k) + i] = st[base + i], st[base + q + i]
            top = 1 << t
            for k in range(1, top >> 1):
                ire, iim = row(m, t, k), row(m, t, top - k)
                for i in range(0, q, L):
                    ea, oa, eb, ob = st[ire + i], st[ire + q + i], st[iim + i], st[iim + q + i]
                    r, im = rot(e, Fraction(k, 2 << t), oa, ob)
                    nx[R(k) + i], nx[R(2 * top - k) + i] = add(ea, r), add(eb, im)
                    nx[R(top - k) + i], nx[R(top + k) + i] = sub(ea, r), sub(im, eb)
        assert len(nx) == n // L
        st = nx
    # terminal
    quarter, half = n >> 2, n >> 1
    top = 1 << t0
    nb = row(m, t0, top >> 1)
    k8 = quarter >> 1
    if L == 2:
        dcv = f"T a[4]{{}}; P::st(a, {st[0]}); P::st(a + 2, {st[2]});"
        nyv = f"T c[4]{{}}; P::st(c, {st[nb]}); P::st(c + 2, {st[nb + 2]});"
    else:
        dcv = f"T a[4]{{}}; P::st(a, {st[0]});"
        nyv = f"T c[4]{{}}; P::st(c, {st[nb]});"
    # rows 0 and N/8: a = (a, b, c, e) of the row
    if mag:
        e.raw(f"{{ {dcv} const T s0 = a[0] + a[2], s1 = a[1] + a[3], d0 = a[0] - a[2], d1 = a[1] - a[3]; "
              f"X[0] = walk_flush<T>(std::fabs(s0 + s1)); X[{half}] = walk_flush<T>(std::fabs(s0 - s1)); X[{quarter}] = walk_flush<T>(std::sqrt(d0 * d0 + d1 * d1)); }}")
        e.raw(f"{{ {nyv} const T rr = {K} * (c[1] - c[3]), ii = {K} * (c[1] + c[3]), p = c[0] + rr, q = c[2] + ii, u = c[0] - rr, w = c[2] - ii; "
              f"X[{k8}] = walk_flush<T>(std::sqrt(p * p + q * q)); X[{quarter + k8}] = walk_flush<T>(std::sqrt(u * u + w * w)); }}")
    else:
        e.raw(f"{{ {dcv} const T s0 = a[0] + a[2], s1 = a[1] + a[3]; X[0] = walk_flush<T>(s0 + s1); X[1] = static_cast<T>(0); X[{2*half}] = walk_flush<T>(s0 - s1); X[{2*half+1}] = static_cast<T>(0); "
              f"X[{2*quarter}] = walk_flush<T>(a[0] - a[2]); X[{2*quarter+1}] = walk_flush<T>(a[3] - a[1]); }}")
        e.raw(f"{{ {nyv} const T rr = {K} * (c[1] - c[3]), ii = {K} * (c[1] + c[3]); "
              f"X[{2*k8}] = walk_flush<T>(c[0] + rr); X[{2*k8+1}] = walk_flush<T>(-(c[2] + ii)); X[{2*(quarter+k8)}] = walk_flush<T>(c[0] - rr); X[{2*(quarter+k8)+1}] = walk_flush<T>(c[2] - ii); }}")
    rows = list(range(1, top >> 1))
    if L == 2:
        for k in rows:
            ire, iim = row(m, t0, k), row(m, t0, top - k)
            R0, R1, I0, I1 = st[ire], st[ire + 2], st[iim], st[iim + 2]
            r, i = rot(e, Fraction(k, 2 << t0), R1, I1)
            LA, LB, HA, HB = add(R0, r), add(I0, i), sub(R0, r), sub(i, I0)
            U, Vv = e.new(f"P::zip2({LA}, {HA})"), e.new(f"P::zip2({LB}, {HB})")
            PA, PB = e.new(f"P::zip1({LA}, {HA})"), e.new(f"P::zip1({LB}, {HB})")
            c2, s2 = cs(Fraction(k, 4 << t0))
            ia, ib = e.const2(c2, s2), e.const2(s2, c2)
            Tc, Ts = e.new(ia), e.new(ib)
            r34 = e.new(f"P::fms(P::mul({Tc}, {U}), {Ts}, {Vv})")
            i34 = e.new(f"P::fma(P::mul({Ts}, {U}), {Tc}, {Vv})")
            S1, S2 = add(PA, r34), sub(PA, r34)
            if mag:
                T1, T2 = add(PB, i34), sub(PB, i34)
                M1 = e.new(f"P::sqrt(P::fma(P::mul({S1}, {S1}), {T1}, {T1}))")
                M2 = e.new(f"P::sqrt(P::fma(P::mul({S2}, {S2}), {T2}, {T2}))")
                e.raw(f"walk_putl<P>(X + {k}, {M1}, 0); walk_putl<P>(X + {quarter-k}, {M1}, 1); walk_putl<P>(X + {half-k}, {M2}, 0); walk_putl<P>(X + {quarter+k}, {M2}, 1);")
            else:
                N1, N2 = e.new(f"P::neg(P::add({PB}, {i34}))"), sub(PB, i34)
                e.raw(f"walk_put<P>(X + {2*k}, P::zip1({S1}, {N1})); walk_put<P>(X + {2*(quarter-k)}, P::zip2({S1}, {N1}));")
                e.raw(f"walk_put<P>(X + {2*(half-k)}, P::zip1({S2}, {N2})); walk_put<P>(X + {2*(quarter+k)}, P::zip2({S2}, {N2}));")
    else:
        pairs = [(rows[i], rows[min(i + 1, len(rows) - 1)]) for i in range(0, len(rows), 2)]
        for k1, k2 in pairs:
            ra, rb = st[row(m, t0, k1)], st[row(m, t0, k2)]
            ja, jb = st[row(m, t0, top - k1)], st[row(m, t0, top - k2)]
            R0, R1 = e.new(f"P::cat_lo({ra}, {rb})"), e.new(f"P::cat_hi({ra}, {rb})")
            I0, I1 = e.new(f"P::cat_lo({ja}, {jb})"), e.new(f"P::cat_hi({ja}, {jb})")
            (ca, sa), (cb, sb) = cs(Fraction(k1, 2 << t0)), cs(Fraction(k2, 2 << t0))
            ic, isn = e.const4(ca, ca, cb, cb), e.const4(sa, sa, sb, sb)
            C1, S1v = e.new(ic), e.new(isn)
            r = e.new(f"P::fms(P::mul({C1}, {R1}), {S1v}, {I1})")
            i = e.new(f"P::fma(P::mul({S1v}, {R1}), {C1}, {I1})")
            LA, LB, HA, HB = add(R0, r), add(I0, i), sub(R0, r), sub(i, I0)
            U, Vv = e.new(f"P::trn2({LA}, {HA})"), e.new(f"P::trn2({LB}, {HB})")
            PA, PB = e.new(f"P::trn1({LA}, {HA})"), e.new(f"P::trn1({LB}, {HB})")
            (c2a, s2a), (c2b, s2b) = cs(Fraction(k1, 4 << t0)), cs(Fraction(k2, 4 << t0))
            ia, ib = e.const4(c2a, s2a, c2b, s2b), e.const4(s2a, c2a, s2b, c2b)
            Tc, Ts = e.new(ia), e.new(ib)
            r34 = e.new(f"P::fms(P::mul({Tc}, {U}), {Ts}, {Vv})")
            i34 = e.new(f"P::fma(P::mul({Ts}, {U}), {Tc}, {Vv})")
            S1, S2 = add(PA, r34), sub(PA, r34)
            # lanes: S1 = bins (k1, quarter-k1, k2, quarter-k2), S2 = (half-k1, quarter+k1, half-k2, quarter+k2)
            if mag:
                T1, T2 = add(PB, i34), sub(PB, i34)
                M1 = e.new(f"P::sqrt(P::fma(P::mul({S1}, {S1}), {T1}, {T1}))")
                M2 = e.new(f"P::sqrt(P::fma(P::mul({S2}, {S2}), {T2}, {T2}))")
                e.raw(f"walk_putl<P>(X + {k1}, {M1}, 0); walk_putl<P>(X + {quarter-k1}, {M1}, 1); walk_putl<P>(X + {k2}, {M1}, 2); walk_putl<P>(X + {quarter-k2}, {M1}, 3);")
                e.raw(f"walk_putl<P>(X + {half-k1}, {M2}, 0); walk_putl<P>(X + {quarter+k1}, {M2}, 1); walk_putl<P>(X + {half-k2}, {M2}, 2); walk_putl<P>(X + {quarter+k2}, {M2}, 3);")
            else:
                N1, N2 = e.new(f"P::neg(P::add({PB}, {i34}))"), sub(PB, i34)
                z1, z2 = e.new(f"P::zip1({S1}, {N1})"), e.new(f"P::zip2({S1}, {N1})")
                z3, z4 = e.new(f"P::zip1({S2}, {N2})"), e.new(f"P::zip2({S2}, {N2})")
                e.raw(f"walk_put_lo<P>(X + {2*k1}, {z1}); walk_put_hi<P>(X + {2*(quarter-k1)}, {z1}); walk_put_lo<P>(X + {2*k2}, {z2}); walk_put_hi<P>(X + {2*(quarter-k2)}, {z2});")
                e.raw(f"walk_put_lo<P>(X + {2*(half-k1)}, {z3}); walk_put_hi<P>(X + {2*(quarter+k1)}, {z3}); walk_put_lo<P>(X + {2*(half-k2)}, {z4}); walk_put_hi<P>(X + {2*(quarter+k2)}, {z4});")
    return e


def inverse(m, L=2):
    global NQ
    n = 1 << m
    NQ = n
    e = Emit()
    ps, t0 = passes(m)
    quarter, half = n >> 2, n >> 1
    top = 1 << t0
    add = lambda a, b: e.new(f"P::add({a}, {b})")
    sub = lambda a, b: e.new(f"P::sub({a}, {b})")
    st = {}
    e.raw(f"T dc[4]{{}}; {{ const T o0 = walk_flush<T>(X[0]), o1 = walk_flush<T>(X[{2*half}]), o2 = walk_flush<T>(X[{2*quarter}]), o3 = -walk_flush<T>(X[{2*quarter+1}]), su = static_cast<T>(0.25) * (o0 + o1), di = static_cast<T>(0.25) * (o0 - o1); "
          f"dc[0] = su + static_cast<T>(0.5) * o2; dc[2] = su - static_cast<T>(0.5) * o2; dc[1] = di + static_cast<T>(0.5) * o3; dc[3] = di - static_cast<T>(0.5) * o3; }}")
    k8 = quarter >> 1
    kh = 'static_cast<T>(0.5) * C[HALFQ]'
    e.raw(f"T ny[4]{{}}; {{ const T o0 = walk_flush<T>(X[{2*k8}]), o1 = -walk_flush<T>(X[{2*k8+1}]), o2 = walk_flush<T>(X[{2*(quarter+k8)}]), o3 = -walk_flush<T>(X[{2*(quarter+k8)+1}]), r2 = o0 - o2, i2 = o1 + o3; "
          f"ny[0] = static_cast<T>(0.5) * (o0 + o2); ny[2] = static_cast<T>(0.5) * (o1 - o3); ny[1] = ({kh}) * (i2 + r2); ny[3] = ({kh}) * (i2 - r2); }}")
    nb = row(m, t0, top >> 1)
    rows = list(range(1, top >> 1))
    if L == 2:
        st[0], st[2] = e.new("P::ld(dc)"), e.new("P::ld(dc + 2)")
        st[nb], st[nb + 2] = e.new("P::ld(ny)"), e.new("P::ld(ny + 2)")
        for k in rows:
            ire, iim = row(m, t0, k), row(m, t0, top - k)
            a0, a1 = e.new(f"walk_get<P>(X + {2*k})"), e.new(f"walk_get<P>(X + {2*(quarter-k)})")
            a2, a3 = e.new(f"walk_get<P>(X + {2*(half-k)})"), e.new(f"walk_get<P>(X + {2*(quarter+k)})")
            S1, N1 = e.new(f"P::zip1({a0}, {a1})"), e.new(f"P::zip2({a0}, {a1})")
            S2, N2 = e.new(f"P::zip1({a2}, {a3})"), e.new(f"P::zip2({a2}, {a3})")
            PA, R2, PB, In = add(S1, S2), sub(S1, S2), sub(N2, N1), add(N1, N2)
            c2, s2 = cs(Fraction(k, 4 << t0))
            ia, ib = e.const2(c2, s2), e.const2(s2, c2)
            Tc, Ts = e.new(ia), e.new(ib)
            U = e.new(f"P::fms(P::mul({Tc}, {R2}), {Ts}, {In})")
            Vv = e.new(f"P::neg(P::fma(P::mul({Ts}, {R2}), {Tc}, {In}))")
            LA, HA = e.new(f"P::zip1({PA}, {U})"), e.new(f"P::zip2({PA}, {U})")
            LB, HB = e.new(f"P::zip1({PB}, {Vv})"), e.new(f"P::zip2({PB}, {Vv})")
            rr, ii = sub(LA, HA), add(LB, HB)
            st[ire] = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::add({LA}, {HA}))")
            st[iim] = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::sub({LB}, {HB}))")
            st[ire + 2], st[iim + 2] = irot(e, Fraction(k, 2 << t0), rr, ii, scale=0.25)
    else:
        st[0], st[nb] = e.new("P::ld(dc)"), e.new("P::ld(ny)")
        pairs = [(rows[i], rows[min(i + 1, len(rows) - 1)]) for i in range(0, len(rows), 2)]
        for k1, k2 in pairs:
            A = e.new(f"walk_get2<P>(X + {2*k1}, X + {2*(quarter-k1)})")
            B = e.new(f"walk_get2<P>(X + {2*k2}, X + {2*(quarter-k2)})")
            Cc = e.new(f"walk_get2<P>(X + {2*(half-k1)}, X + {2*(quarter+k1)})")
            D = e.new(f"walk_get2<P>(X + {2*(half-k2)}, X + {2*(quarter+k2)})")
            S1, N1 = e.new(f"P::uzp1({A}, {B})"), e.new(f"P::uzp2({A}, {B})")
            S2, N2 = e.new(f"P::uzp1({Cc}, {D})"), e.new(f"P::uzp2({Cc}, {D})")
            PA, R2, PB, In = add(S1, S2), sub(S1, S2), sub(N2, N1), add(N1, N2)
            (c2a, s2a), (c2b, s2b) = cs(Fraction(k1, 4 << t0)), cs(Fraction(k2, 4 << t0))
            ia, ib = e.const4(c2a, s2a, c2b, s2b), e.const4(s2a, c2a, s2b, c2b)
            Tc, Ts = e.new(ia), e.new(ib)
            U = e.new(f"P::fms(P::mul({Tc}, {R2}), {Ts}, {In})")
            Vv = e.new(f"P::neg(P::fma(P::mul({Ts}, {R2}), {Tc}, {In}))")
            LA, HA = e.new(f"P::trn1({PA}, {U})"), e.new(f"P::trn2({PA}, {U})")
            LB, HB = e.new(f"P::trn1({PB}, {Vv})"), e.new(f"P::trn2({PB}, {Vv})")
            rr, ii = sub(LA, HA), add(LB, HB)
            R0 = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::add({LA}, {HA}))")
            I0 = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::sub({LB}, {HB}))")
            (ca, sa), (cb, sb) = cs(Fraction(k1, 2 << t0)), cs(Fraction(k2, 2 << t0))
            ic, isn = e.const4(ca, ca, cb, cb, scale=0.25), e.const4(sa, sa, sb, sb, scale=0.25)
            C1, S1v = e.new(ic), e.new(isn)
            R1 = e.new(f"P::fma(P::mul({C1}, {rr}), {S1v}, {ii})")
            I1 = e.new(f"P::fms(P::mul({C1}, {ii}), {S1v}, {rr})")
            st[row(m, t0, k1)], st[row(m, t0, k2)] = e.new(f"P::cat_lo({R0}, {R1})"), e.new(f"P::cat_hi({R0}, {R1})")
            st[row(m, t0, top - k1)], st[row(m, t0, top - k2)] = e.new(f"P::cat_lo({I0}, {I1})"), e.new(f"P::cat_hi({I0}, {I1})")
    for radix, t in reversed(ps):
        pv = {}
        if radix == 4:
            q = n >> (t + 2)
            off = [row(m, t + 2, c << t) for c in range(4)]
            R = lambda k: row(m, t + 2, k)
            for i in range(0, q, L):
                x0, x1, x2, x3 = st[R(0) + i], st[R(2 << t) + i], st[R(1 << t) + i], st[R(3 << t) + i]
                su = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::add({x0}, {x1}))")
                di = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::sub({x0}, {x1}))")
                pv[i], pv[2 * q + i] = e.new(f"P::fma({su}, P::set1(static_cast<T>(0.5)), {x2})"), e.new(f"P::fms({su}, P::set1(static_cast<T>(0.5)), {x2})")
                pv[q + i], pv[3 * q + i] = e.new(f"P::fma({di}, P::set1(static_cast<T>(0.5)), {x3})"), e.new(f"P::fms({di}, P::set1(static_cast<T>(0.5)), {x3})")
            if t >= 1:
                k = 1 << (t - 1)
                base = row(m, t, k)
                for i in range(0, q, L):
                    x0, x1, x2, x3 = st[R(k) + i], st[R(7 * k) + i], st[R(3 * k) + i], st[R(5 * k) + i]
                    r2, i2 = sub(x0, x2), add(x1, x3)
                    pv[base + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::add({x0}, {x2}))")
                    pv[base + 2 * q + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::sub({x1}, {x3}))")
                    pv[base + q + i] = e.new(f"P::mul(P::set1({kh}), P::add({i2}, {r2}))")
                    pv[base + 3 * q + i] = e.new(f"P::mul(P::set1({kh}), P::sub({i2}, {r2}))")
            top_t = 1 << t
            for k in range(1, top_t >> 1):
                ire, iim, oo, ob = row(m, t, k), row(m, t, top_t - k), R(k), R(top_t - k)
                t1, t2 = Fraction(k, 2 << t), Fraction(k, 4 << t)
                for i in range(0, q, L):
                    o0, o1, o2, o3 = (st[oo + off[c] + i] for c in range(4))
                    b0, b1, b2, b3 = st[ob + off[3] + i], st[ob + off[0] + i], st[ob + off[1] + i], st[ob + off[2] + i]
                    LA, R3, LB, I3 = add(o0, b2), sub(o0, b2), sub(b0, o2), add(b0, o2)
                    HA, R4, HB, I4 = add(b1, o1), sub(b1, o1), sub(o3, b3), add(o3, b3)
                    lbA, lbB = irot(e, t2, R3, I3, scale=0.25)
                    hbA, hbB = irot(e, t2, R4, I4, swap=True, scale=0.25)
                    rr, ii = sub(LA, HA), add(LB, HB)
                    r2, i2 = sub(lbA, hbA), add(lbB, hbB)
                    pv[ire + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::add({LA}, {HA}))")
                    pv[iim + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.25)), P::sub({LB}, {HB}))")
                    pv[ire + 2 * q + i], pv[iim + 2 * q + i] = irot(e, t1, rr, ii, scale=0.25)
                    pv[ire + q + i], pv[iim + q + i] = add(lbA, hbA), sub(lbB, hbB)
                    pv[ire + 3 * q + i], pv[iim + 3 * q + i] = irot(e, t1, r2, i2)
        else:
            q = n >> (t + 1)
            R = lambda k: row(m, t + 1, k)
            for i in range(0, q, L):
                x0, x1 = st[R(0) + i], st[R(1 << t) + i]
                pv[i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::add({x0}, {x1}))")
                pv[q + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::sub({x0}, {x1}))")
            if t >= 1:
                k = 1 << (t - 1)
                base = row(m, t, k)
                for i in range(0, q, L):
                    pv[base + i], pv[base + q + i] = st[R(k) + i], st[R(3 * k) + i]
            top_t = 1 << t
            for k in range(1, top_t >> 1):
                ire, iim = row(m, t, k), row(m, t, top_t - k)
                for i in range(0, q, L):
                    x0, x1, x2, x3 = st[R(k) + i], st[R(2 * top_t - k) + i], st[R(top_t - k) + i], st[R(top_t + k) + i]
                    rr, ii = sub(x0, x2), add(x1, x3)
                    pv[ire + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::add({x0}, {x2}))")
                    pv[iim + i] = e.new(f"P::mul(P::set1(static_cast<T>(0.5)), P::sub({x1}, {x3}))")
                    pv[ire + q + i], pv[iim + q + i] = irot(e, Fraction(k, 2 << t), rr, ii, scale=0.5)
        assert len(pv) == n // L, (m, radix, t, len(pv))
        st = pv
    for a in range(0, n, L):
        e.raw(f"walk_put<P>(y + {a}, {st[a]});")
    return e


import os
import re


def pruned(lines):
    """Drops `const V vN = ...;` definitions nothing reads (the inverse's
    unused halves): every one is a pure value, so the kernel computes the
    same bits without them."""
    definition = re.compile(r"^\s*const V (v\d+) = ")
    while True:
        defined = {}
        for index, line in enumerate(lines):
            m = definition.match(line)
            if m:
                defined[m.group(1)] = index
        text = "\n".join(lines)
        unused = [name for name in defined if len(re.findall(r"\b" + name + r"\b", text)) == 1]
        if not unused:
            return lines
        drop = {defined[name] for name in unused}
        lines = [line for index, line in enumerate(lines) if index not in drop]


root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "src", "algorithms", "fft")
for fname, L, sizes in (("walk_small_f64.inc", 2, [4, 5, 6, 7]), ("walk_small_f32.inc", 4, [4, 5, 6, 7, 8])):
    sfx = "f64" if L == 2 else "f32"
    out = ["// Generated by tools/fft/gen_walk_small.py; edit the generator, not this file.",
           "// stx_fft's straight-line walk kernels in " + ("float64 (two lanes), N = 16 .. 128." if L == 2 else "float32 (four lanes), N = 16 .. 256."),
           "// P is the tier's straight-line policy (walk_lanes.inc, WalkPolicies<T>::S). fwd writes interleaved",
           "// (re, im); mag writes the N/2 + 1 magnitudes; inv reads interleaved bins. C is the plan's twiddle table:",
           "// C[2a] = cos(2 pi a / N), C[2a + 1] = sin(2 pi a / N), a = 0 .. N/8."]
    for m in sizes:
        n = 1 << m
        for name, e in (("fwd", forward(m, L)), ("mag", forward(m, L, True)), ("inv", inverse(m, L))):
            src, dst = ("X", "y") if name == "inv" else ("x", "X")
            out.append(f"template <typename P>")
            out.append(f"inline void small_{name}_{sfx}_{n}(const typename P::t* const {src}, typename P::t* const {dst}, const typename P::t* const C) noexcept {{")
            out.append("    using V = typename P::v;")
            out.append("    using T = typename P::t;")
            out += pruned([l.replace("HALFQ", str(2 * (n // 8))) for l in e.lines])
            out.append("}")
    with open(os.path.join(root, fname), "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", fname, sum(len(l) for l in out) // 1024, "KiB")
