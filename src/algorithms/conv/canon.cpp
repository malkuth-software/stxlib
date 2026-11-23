// stxlib::conv's canon for affine, mesh and projective maps (canon.hpp).
//
// A destination pixel (x, y) covers [x - 1/2, x + 1/2] x [y - 1/2, y + 1/2]; its canon value is
// (1 / covered area) times the integral over the pixel of F(M(p)), F the canon field with
// coordinates clamped to the source, M the map. Design and evidence: work/conv FFT_MATH.md
// Part VII (term 3 adjudicated, canon by shared-edge flux), SIMD_DESIGN.md Stage 3 (K3, the
// canon on the SIMD representation), PERSPECTIVE_OPTIONS.md option (b). Restated from the
// bench's mesh_canon.inc (SimdSink, CanonJob, perspective_triangle) in the same operation
// order, so its outputs are the bench's with CONV_FLUX=1 and CONV_SPLIT=off.
//
// Regions. Along u, interior cells i = 0 .. n_u - 2 cover [i, i + 1); region -1 is (-inf, 0)
// and region n_u - 1 is [n_u - 1, +inf), where F does not depend on u. Likewise along v.
// On a region cell the field is piecewise polynomial in tu: one strip, or four equal strips
// where the cell's term-3 nodes are contested, each with the degree-5 interpolant of term 3
// through its six Gauss nodes (owner, 2026-11-23: no adaptive strip splitting). Each strip keeps the power coefficients of a
// primitive Q (dQ/dtu = F, 42 terms per channel, channel innermost), anchored so Q is
// continuous along u across the strips and cells of a tile row. Then the integral of F over a
// source region is the closed contour integral of Q dv:
// - a pixel wholly inside one non-singular affine piece takes edge flux (each pixel edge by
//   six-point Gauss per strip piece, exact for the polynomial integrand; an edge shared with
//   the previous pixel in the tile, in the same piece, is reused);
// - other pixels are cut into region polygons, integrated by moments per strip;
// - a near-singular piece is integrated in destination space by collapsed 6 x 6 Gauss through
//   the field's point evaluation (a stated quadrature fallback);
// - perspective (option (b)): each pixel is two destination triangles, subdivided while the
//   relative variation of w across a triangle gives 1.5 (dw / w)^2 > 2^-16 (depth at most 8);
//   each leaf's exact source triangle is integrated with the density 1 / (|det H| w^3)
//   replaced by its linear interpolant, by degree-6 moments.
//
// Cells are built per 16-pixel tile, inside a box of region cells around the tile's source
// footprint (at most 4,096 cells); a cell outside it is built alone, without the cache. Tiles
// lie on the destination's own 16-pixel grid and every shared edge a region leaves out is
// recomputed exactly as the whole-image pass computes it, so a region's pixels have the bits
// the whole image's would have.
//
// Every value is computed into the workspace first and encoded only when the whole region has
// succeeded: on failure no pixel of the region has been written.

#include "canon.hpp"

#include "stxlib/memory.hpp"
#include "stxlib/require.hpp"

#include <new>

namespace stxlib::conv::detail {

namespace {

constexpr usize cc = channel_count;

// ---------------------------------------------------------------- numbers

// floor and ceil of a finite double, exact (beyond 2^52 every double is whole).
double floor_of(const double x) noexcept {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    const double t = static_cast<double>(static_cast<i64>(x));
    return t > x ? t - 1.0 : t;
}

double ceil_of(const double x) noexcept {
    if (!(x > -4503599627370496.0 && x < 4503599627370496.0)) return x;
    const double t = static_cast<double>(static_cast<i64>(x));
    return t < x ? t + 1.0 : t;
}

// |x| for finite x (+0 for either zero).
double absolute(const double x) noexcept { return x < 0.0 ? -x : x + 0.0; }

bool finite(const double x) noexcept { return x == x && x - x == 0.0; }

usize least_size(const usize a, const usize b) noexcept { return b < a ? b : a; }

double clamp(const double v, const double lo, const double hi) noexcept { return v < lo ? lo : hi < v ? hi : v; }

void sort_small(double* values, const usize count) noexcept {
    for (usize k = 1; k < count; ++k) {
        const double value = values[k];
        usize j = k;
        while (j > 0 && value < values[j - 1]) {
            values[j] = values[j - 1];
            j = j - 1;
        }
        values[j] = value;
    }
}

// ---------------------------------------------------------------- tables

struct Tables final {
    double binom[13][13]{};
    // Power coefficients of B_m^5: beta[m][a].
    double beta[6][6]{};
    // Power coefficients of tau_k(t) = sum_{l > k} B_l^5(t): tauc[k][b].
    double tauc[5][6]{};
    double norm[6][6]{};
    // Six-point Gauss-Legendre on [0, 1].
    double gx[6]{};
    double gw[6]{};
};

constexpr Tables make_tables() noexcept {
    Tables t{};
    for (usize n = 0; n <= 12; ++n) {
        t.binom[n][0] = 1.0;
        for (usize k = 1; k <= n; ++k) t.binom[n][k] = t.binom[n - 1][k - 1] + (k <= n - 1 ? t.binom[n - 1][k] : 0.0);
    }
    for (usize m = 0; m < 6; ++m) {
        for (usize i = 0; m + i <= 5; ++i) {
            const double sign = (i % 2 == 0) ? 1.0 : -1.0;
            t.beta[m][m + i] = t.binom[5][m] * t.binom[5 - m][i] * sign;
        }
    }
    for (usize k = 0; k < 5; ++k) {
        for (usize l = k + 1; l <= 5; ++l) {
            for (usize b = 0; b < 6; ++b) t.tauc[k][b] = t.tauc[k][b] + t.beta[l][b];
        }
    }
    for (usize p = 0; p < 6; ++p) {
        for (usize q = 0; q < 6; ++q) {
            t.norm[p][q] = 1.0 / (static_cast<double>(p + q + 2) * static_cast<double>(p + q + 1) * t.binom[p + q][p]);
        }
    }
    const double x[6] = {-0.9324695142031521, -0.6612093864662645, -0.2386191860831969, 0.2386191860831969, 0.6612093864662645, 0.9324695142031521};
    const double w[6] = {0.1713244923791704, 0.3607615730481386, 0.4679139345726910, 0.4679139345726910, 0.3607615730481386, 0.1713244923791704};
    for (usize k = 0; k < 6; ++k) {
        t.gx[k] = 0.5 * (x[k] + 1.0);
        t.gw[k] = 0.5 * w[k];
    }
    return t;
}

constexpr Tables tables = make_tables();

// ---------------------------------------------------------------- geometry

struct P2 final {
    double x{0.0};
    double y{0.0};
};

// A convex polygon. The pixel square clipped by a triangle's three half-planes has at most 7
// vertices; four region half-planes add at most 4; two strip lines at most 2: 13 <= 16.
constexpr usize poly_capacity = 16;

struct Poly final {
    P2 p[poly_capacity];
    usize n{0};
};

// Keeps a x + b y + c >= 0 (convex input, convex output, at most one vertex more).
Poly clip(const Poly& in, const double a, const double b, const double c) noexcept {
    Poly out;
    if (in.n == 0) return out;
    STXLIB_REQUIRE(in.n + 1 <= poly_capacity, "stxlib.conv canon: polygon capacity");
    for (usize k = 0; k < in.n; ++k) {
        const P2 s = in.p[k];
        const P2 e = in.p[(k + 1) % in.n];
        const double ds = a * s.x + b * s.y + c;
        const double de = a * e.x + b * e.y + c;
        if (ds >= 0.0) {
            out.p[out.n] = s;
            out.n = out.n + 1;
        }
        if ((ds >= 0.0) != (de >= 0.0)) {
            const double t = ds / (ds - de);
            P2 cut;
            cut.x = s.x + t * (e.x - s.x);
            cut.y = s.y + t * (e.y - s.y);
            out.p[out.n] = cut;
            out.n = out.n + 1;
        }
    }
    return out;
}

double area(const Poly& q) noexcept {
    double twice = 0.0;
    for (usize k = 0; k < q.n; ++k) {
        const P2 a = q.p[k];
        const P2 b = q.p[(k + 1) % q.n];
        twice = twice + (a.x * b.y - b.x * a.y);
    }
    return 0.5 * twice;
}

// m[p][q] = integral of x^p y^q over a counterclockwise polygon, p, q <= 5.
void moments(const Poly& poly, double m[6][6]) noexcept {
    for (usize p = 0; p < 6; ++p) {
        for (usize q = 0; q < 6; ++q) m[p][q] = 0.0;
    }
    for (usize k = 0; k < poly.n; ++k) {
        const P2 a = poly.p[k];
        const P2 b = poly.p[(k + 1) % poly.n];
        const double cross = a.x * b.y - b.x * a.y;
        double ax[6];
        double bx[6];
        double ay[6];
        double by[6];
        ax[0] = 1.0;
        bx[0] = 1.0;
        ay[0] = 1.0;
        by[0] = 1.0;
        for (usize e = 1; e < 6; ++e) {
            ax[e] = ax[e - 1] * a.x;
            bx[e] = bx[e - 1] * b.x;
            ay[e] = ay[e - 1] * a.y;
            by[e] = by[e - 1] * b.y;
        }
        for (usize p = 0; p < 6; ++p) {
            for (usize q = 0; q < 6; ++q) {
                double sum = 0.0;
                for (usize i = 0; i <= p; ++i) {
                    for (usize j = 0; j <= q; ++j) {
                        sum = sum + tables.binom[i + j][i] * tables.binom[p + q - i - j][p - i] * ax[i] * bx[p - i] * ay[j] * by[q - j];
                    }
                }
                m[p][q] = m[p][q] + cross * sum;
            }
        }
    }
    for (usize p = 0; p < 6; ++p) {
        for (usize q = 0; q < 6; ++q) m[p][q] = m[p][q] * tables.norm[p][q];
    }
}

// m[p][q] = integral of x^p y^q, p, q <= 6, over a counterclockwise polygon.
void moments7(const Poly& poly, double m[7][7]) noexcept {
    for (usize p = 0; p < 7; ++p) {
        for (usize q = 0; q < 7; ++q) m[p][q] = 0.0;
    }
    for (usize k = 0; k < poly.n; ++k) {
        const P2 a = poly.p[k];
        const P2 b = poly.p[(k + 1) % poly.n];
        const double cross = a.x * b.y - b.x * a.y;
        double ax[7];
        double bx[7];
        double ay[7];
        double by[7];
        ax[0] = 1.0;
        bx[0] = 1.0;
        ay[0] = 1.0;
        by[0] = 1.0;
        for (usize e = 1; e < 7; ++e) {
            ax[e] = ax[e - 1] * a.x;
            bx[e] = bx[e - 1] * b.x;
            ay[e] = ay[e - 1] * a.y;
            by[e] = by[e - 1] * b.y;
        }
        for (usize p = 0; p < 7; ++p) {
            for (usize q = 0; q < 7; ++q) {
                double sum = 0.0;
                for (usize i = 0; i <= p; ++i) {
                    for (usize j = 0; j <= q; ++j) {
                        sum = sum + tables.binom[i + j][i] * tables.binom[p + q - i - j][p - i] * ax[i] * bx[p - i] * ay[j] * by[q - j];
                    }
                }
                m[p][q] = m[p][q] + cross * sum;
            }
        }
    }
    for (usize p = 0; p < 7; ++p) {
        for (usize q = 0; q < 7; ++q) {
            m[p][q] = m[p][q] / (static_cast<double>(p + q + 2) * static_cast<double>(p + q + 1) * tables.binom[p + q][p]);
        }
    }
}

void bernstein5(const double t, double* b) noexcept {
    const double s = 1.0 - t;
    for (usize q = 0; q <= 5; ++q) {
        double v = tables.binom[5][q];
        for (usize e = 0; e < q; ++e) v = v * t;
        for (usize e = 0; e < 5 - q; ++e) v = v * s;
        b[q] = v;
    }
}

// Clamped region index of a coordinate: -1, 0 .. cells - 1, or cells.
i64 region_of(const double x, const usize cells) noexcept {
    if (!(x >= 0.0)) return -1;
    if (x >= static_cast<double>(cells)) return static_cast<i64>(cells);
    i64 r = static_cast<i64>(floor_of(x));
    if (r >= static_cast<i64>(cells)) r = static_cast<i64>(cells) - 1;
    return r;
}

// A region's bounds as half-planes on one axis.
Poly clip_region(const Poly& in, const bool along_u, const i64 r, const usize cells) noexcept {
    Poly out = in;
    const double a = along_u ? 1.0 : 0.0;
    const double b = along_u ? 0.0 : 1.0;
    if (r >= 0) out = clip(out, a, b, -static_cast<double>(r));
    if (r < static_cast<i64>(cells)) out = clip(out, -a, -b, static_cast<double>(r + 1));
    return out;
}

// The origin of a region's local coordinates (interior: the cell corner; outside: the nearest
// boundary node, where F no longer depends on that coordinate).
double region_origin(const i64 r, const usize cells) noexcept {
    if (r < 0) return 0.0;
    if (r >= static_cast<i64>(cells)) return static_cast<double>(cells);
    return static_cast<double>(r);
}

// S(p) = (ox, oy) + A (p - (dx, dy)).
struct Affine final {
    double ox{0.0};
    double oy{0.0};
    double dx{0.0};
    double dy{0.0};
    double a11{0.0};
    double a12{0.0};
    double a21{0.0};
    double a22{0.0};
    void at(const double x, const double y, double& u, double& v) const noexcept {
        u = ox + a11 * (x - dx) + a12 * (y - dy);
        v = oy + a21 * (x - dx) + a22 * (y - dy);
    }
};

// ---------------------------------------------------------------- strips

constexpr usize q_terms = 42; // a <= 6, b <= 5
constexpr usize terms = q_terms * cc;
// Contested cells take four strips (dyadic level 2).
constexpr usize strip_count = 4;
constexpr usize strip_level = 2;
constexpr usize batch = strip_count * 6 * cc;

// The Lagrange basis of the six Gauss nodes on the dyadic strip (level 0: the whole cell; level 2:
// the four strips) [k w, (k + 1) w], w = 2^-level,
// as power polynomials in tu: lambda = tu / w - k.
void dyadic_basis(const usize level, const usize k, double coef[6][6]) noexcept {
    double lp[6][6]{};
    for (usize n = 0; n < 6; ++n) {
        double poly[6] = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        usize degree = 0;
        double denominator = 1.0;
        for (usize m = 0; m < 6; ++m) {
            if (m == n) continue;
            for (usize a = degree + 1; a > 0; --a) poly[a] = poly[a - 1] - tables.gx[m] * poly[a];
            poly[0] = -tables.gx[m] * poly[0];
            degree = degree + 1;
            denominator = denominator * (tables.gx[n] - tables.gx[m]);
        }
        for (usize a = 0; a < 6; ++a) lp[n][a] = poly[a] / denominator;
    }
    const double scale = static_cast<double>(usize{1} << level);
    for (usize n = 0; n < 6; ++n) {
        for (usize a = 0; a < 6; ++a) coef[n][a] = 0.0;
    }
    for (usize n = 0; n < 6; ++n) {
        for (usize a = 0; a < 6; ++a) {
            for (usize b = 0; b <= a; ++b) {
                double term = tables.binom[a][b];
                for (usize e = 0; e < b; ++e) term = term * scale;
                for (usize e = 0; e < a - b; ++e) term = term * -static_cast<double>(k);
                coef[n][b] = coef[n][b] + lp[n][a] * term;
            }
        }
    }
}

// A built region cell: its strip count (1 or 4) and the offset of its Q coefficients
// ([strip][42][channel]) in the pool.
struct CellEntry final {
    usize strips{1};
    usize offset{0};
};

// The boundary s of a cell's equal strips, in tu (exact: s / 4).
double strip_bound(const usize s, const usize strips) noexcept { return static_cast<double>(s) / static_cast<double>(strips); }

struct Strip final {
    usize k{0};
    double e[6 * 5 * cc]{}; // [node][current][channel]
    bool contested{false};
};

void node(const FieldState& field, const usize x, const usize r, double* out) noexcept {
    decode(field.source.view().data() + (r * field.width + x) * 4, out);
}

// ---------------------------------------------------------------- the sink

// The per-tile integrator: cell builds (the cache box and its pool), and the three ways of
// integrating a piece. Large (its batch buffers), so allocated once per call.
struct Sink final {
    const FieldState* field{nullptr};
    const Kernels* kernels{nullptr};
    usize cells_u{0};
    usize cells_v{0};
    bool boxed{false};
    i64 i0{0};
    i64 j0{0};
    usize ni{0};
    usize nj{0};
    Vector<CellEntry> entries;
    Vector<u32> stamps;
    Vector<usize> built_until;
    Vector<double> pool;
    u32 stamp{0};
    Vector<double> scratch_pool;
    Vector<double> scalar_pool;
    double sum[cc]{0.0, 0.0, 0.0, 0.0};
    bool failed{false};
    // K1 batch buffers: 4 strips x 6 nodes x 4 channels.
    double nodes_w[8 * batch]{};
    double nodes_raw[5 * batch]{};
    u32 nodes_list[batch]{};
    double compact[9 * batch]{};
    double compact_admitted[5 * batch]{};
    u8 compact_idle[batch]{};
    Strip strips_now[strip_count];

    bool grow(Vector<double>& into, const usize extra) noexcept {
        if (failed) return false;
        if (into.resize(into.count() + extra).failed()) {
            failed = true;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool outer_u(const i64 iu) const noexcept { return iu < 0 || iu >= static_cast<i64>(cells_u); }

    // Row r's cell controls at region iu, per channel: anchor and prefix sums of the admitted
    // currents (outside along u: the end node, constant).
    void row_controls(const usize r, const i64 iu, double P[cc][6]) const noexcept {
        const FieldState& f = *field;
        const usize width = f.width;
        if (outer_u(iu)) {
            double end_node[cc];
            node(f, iu < 0 ? 0 : width - 1, r, end_node);
            for (usize c = 0; c < cc; ++c) {
                for (usize m = 0; m < 6; ++m) P[c][m] = end_node[c];
            }
            return;
        }
        // The row cell (row_segment: raw currents plus the contest's e, or an end cell's admitted
        // currents; compile's arithmetic).
        const usize i = static_cast<usize>(iu);
        double cell[24];
        SmallSegment small;
        row_segment(f, r, i, i, cell, small.buffers());
        for (usize c = 0; c < cc; ++c) {
            P[c][0] = cell[c];
            for (usize m = 0; m < 5; ++m) P[c][m + 1] = P[c][m] + cell[(m + 1) * cc + c];
        }
    }

    // e = admitted - raw of the vertical cell at phase tu; true if nonzero.
    static bool scalar_contest(const double P[8][cc][6], const usize c, const CellShape& shape, const bool interior_v, const double tu,
                               double* e) noexcept {
        double b5[6];
        bernstein5(tu, b5);
        double w[8];
        for (usize k = 0; k < 8; ++k) {
            double v = 0.0;
            for (usize m = 0; m < 6; ++m) v = v + b5[m] * P[k][c][m];
            w[k] = v;
        }
        double raw[5];
        raw_for_shape(w, shape, raw);
        double admitted[5]{0.0, 0.0, 0.0, 0.0, 0.0};
        const bool idle = admit_cell(w, shape, raw, admitted, interior_v);
        bool any = false;
        for (usize k = 0; k < 5; ++k) {
            e[k] = idle ? 0.0 : admitted[k] - raw[k];
            any = any || e[k] != 0.0;
        }
        return any;
    }

    // The scalar build (outer and end cells), into scalar_pool as [strip][channel][42]; anchor
    // per channel (six tv-coefficients of Q at the cell's left boundary; null: zero).
    void scalar_build(const i64 iu, const i64 jv, CellEntry& entry, Vector<double>& into, const double (*anchor)[6]) noexcept {
        const FieldState& f = *field;
        const usize height = f.height;
        const bool outer = outer_u(iu);
        double g[cc][36]{};
        double e_nodes[cc][6 * strip_count][5]{};
        bool contested = false;
        bool contested_channel[cc] = {false, false, false, false};
        if (jv < 0 || jv >= static_cast<i64>(cells_v)) {
            double P[cc][6];
            row_controls(jv < 0 ? 0 : height - 1, iu, P);
            for (usize c = 0; c < cc; ++c) {
                for (usize a = 0; a < 6; ++a) {
                    double v = 0.0;
                    for (usize m = 0; m < 6; ++m) v = v + tables.beta[m][a] * P[c][m];
                    g[c][a * 6] = v;
                }
            }
        } else {
            const usize j = static_cast<usize>(jv);
            const CellShape shape = cell_shape(j, height);
            const bool interior_v = j >= 2 && j + 4 <= height;
            double P[8][cc][6];
            for (usize k = 0; k < 8; ++k) {
                const i64 row = static_cast<i64>(j) - 3 + static_cast<i64>(k);
                const usize r = row < 0 ? 0 : static_cast<usize>(row) >= height ? height - 1 : static_cast<usize>(row);
                row_controls(r, iu, P[k]);
            }
            for (usize c = 0; c < cc; ++c) {
                double R[5][6];
                for (usize m = 0; m < 6; ++m) {
                    double w[8];
                    for (usize k = 0; k < 8; ++k) w[k] = P[k][c][m];
                    double raw[5];
                    raw_for_shape(w, shape, raw);
                    for (usize k = 0; k < 5; ++k) R[k][m] = raw[k];
                }
                for (usize m = 0; m < 6; ++m) {
                    double inner[6];
                    for (usize b = 0; b < 6; ++b) {
                        double v = b == 0 ? P[3][c][m] : 0.0;
                        for (usize k = 0; k < 5; ++k) v = v + R[k][m] * tables.tauc[k][b];
                        inner[b] = v;
                    }
                    for (usize a = 0; a < 6; ++a) {
                        if (tables.beta[m][a] == 0.0) continue;
                        for (usize b = 0; b < 6; ++b) g[c][a * 6 + b] = g[c][a * 6 + b] + tables.beta[m][a] * inner[b];
                    }
                }
                const usize nodes = outer ? 1 : 6 * strip_count;
                for (usize n = 0; n < nodes; ++n) {
                    const double tu = outer ? 0.0 : (static_cast<double>(n / 6) + tables.gx[n % 6]) / static_cast<double>(strip_count);
                    if (scalar_contest(P, c, shape, interior_v, tu, e_nodes[c][n])) {
                        contested_channel[c] = true;
                        contested = true;
                    }
                }
            }
        }
        const usize strips = (contested && !outer) ? strip_count : 1;
        entry.strips = strips;
        entry.offset = into.count();
        if (!grow(into, strips * cc * q_terms)) return;
        for (usize s = 0; s < strips; ++s) {
            double coef[6][6];
            dyadic_basis(strip_level, s, coef);
            const double t0 = outer ? 0.0 : static_cast<double>(s) / static_cast<double>(strips);
            for (usize c = 0; c < cc; ++c) {
                double fq[36];
                for (usize k = 0; k < 36; ++k) fq[k] = g[c][k];
                if (contested_channel[c]) {
                    if (outer) {
                        for (usize k = 0; k < 5; ++k) {
                            for (usize b = 0; b < 6; ++b) fq[b] = fq[b] + e_nodes[c][0][k] * tables.tauc[k][b];
                        }
                    } else {
                        for (usize k = 0; k < 5; ++k) {
                            double ec[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                            for (usize n = 0; n < 6; ++n) {
                                for (usize a = 0; a < 6; ++a) ec[a] = ec[a] + e_nodes[c][s * 6 + n][k] * coef[n][a];
                            }
                            for (usize a = 0; a < 6; ++a) {
                                for (usize b = 0; b < 6; ++b) fq[a * 6 + b] = fq[a * 6 + b] + ec[a] * tables.tauc[k][b];
                            }
                        }
                    }
                }
                double* const base = into.mutable_view().data();
                double* const q = base + entry.offset + (s * cc + c) * q_terms;
                for (usize a = 0; a < 6; ++a) {
                    for (usize b = 0; b < 6; ++b) q[(a + 1) * 6 + b] = fq[a * 6 + b] / static_cast<double>(a + 1);
                }
                // Q_s(t0, tv) = A_s(tv); A_0 from the left neighbour, A_s from strip s - 1.
                double a_s[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
                if (s == 0) {
                    if (anchor != nullptr) {
                        for (usize b = 0; b < 6; ++b) a_s[b] = anchor[c][b];
                    }
                } else {
                    const double* const prev = base + entry.offset + ((s - 1) * cc + c) * q_terms;
                    for (usize b = 0; b < 6; ++b) {
                        double v = 0.0;
                        for (usize a = 7; a > 0; --a) v = v * t0 + prev[(a - 1) * 6 + b];
                        a_s[b] = v;
                    }
                }
                for (usize b = 0; b < 6; ++b) {
                    double at_t0 = 0.0;
                    for (usize a = 7; a > 1; --a) at_t0 = at_t0 * t0 + q[(a - 1) * 6 + b];
                    at_t0 = at_t0 * t0;
                    q[b] = a_s[b] - at_t0;
                }
            }
        }
    }

    // Contests the six nodes of each of the four strips (all channels) in one K1 batch; fills e
    // and contested (any nonzero e).
    void contest_strips(const double* P) noexcept {
        const usize nodes = 6 * strip_count;
        const usize lanes = nodes * cc;
        const double width = 1.0 / static_cast<double>(usize{1} << strip_level);
        double b5[6 * 6 * strip_count];
        for (usize s = 0; s < strip_count; ++s) {
            for (usize g = 0; g < 6; ++g) {
                const double tu = (static_cast<double>(strips_now[s].k) + tables.gx[g]) * width;
                bernstein5(tu, b5 + 6 * (6 * s + g));
            }
        }
        double* w[8];
        for (usize k = 0; k < 8; ++k) w[k] = nodes_w + k * lanes;
        (*kernels).node_windows(P, nodes, b5, w);
        double* raw[5];
        for (usize k = 0; k < 5; ++k) raw[k] = nodes_raw + k * lanes;
        const double* wc[8] = {w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]};
        const usize listed = (*kernels).pass_a(wc, raw, nodes_list, lanes);
        for (usize s = 0; s < strip_count; ++s) {
            for (usize k = 0; k < 6 * 5 * cc; ++k) strips_now[s].e[k] = 0.0;
            strips_now[s].contested = false;
        }
        if (listed == 0) return;
        const usize stride = listed;
        double* cw[8];
        double* cr[5];
        double* ca[5];
        for (usize k = 0; k < 4; ++k) cw[k + 2] = compact + k * stride;
        cw[0] = cw[2];
        cw[1] = cw[2];
        cw[6] = cw[5];
        cw[7] = cw[5];
        for (usize k = 0; k < 5; ++k) {
            cr[k] = compact + (4 + k) * stride;
            ca[k] = compact_admitted + k * stride;
        }
        for (usize j = 0; j < listed; ++j) {
            const usize lane = nodes_list[j];
            for (usize k = 2; k <= 5; ++k) cw[k][j] = w[k][lane];
            for (usize k = 0; k < 5; ++k) cr[k][j] = raw[k][lane];
        }
        const double* cwc[8] = {cw[0], cw[1], cw[2], cw[3], cw[4], cw[5], cw[6], cw[7]};
        const double* crc[5] = {cr[0], cr[1], cr[2], cr[3], cr[4]};
        (*kernels).pass_b(cwc, crc, ca, compact_idle, listed);
        for (usize j = 0; j < listed; ++j) {
            if (compact_idle[j] != 0) continue;
            const usize lane = nodes_list[j];
            const usize at = lane / cc;
            const usize c = lane % cc;
            const usize s = at / 6;
            const usize g = at % 6;
            bool any = false;
            for (usize k = 0; k < 5; ++k) {
                const double value = ca[k][j] - cr[k][j];
                strips_now[s].e[(g * 5 + k) * cc + c] = value;
                any = any || value != 0.0;
            }
            if (any) strips_now[s].contested = true;
        }
    }

    // Builds region cell (iu, jv) into `into`, anchored to `anchor` (six tv-coefficients per
    // channel, channel innermost; null: zero).
    void build(const i64 iu, const i64 jv, CellEntry& entry, Vector<double>& into, const double* anchor) noexcept {
        if (failed) return;
        const FieldState& f = *field;
        const usize height = f.height;
        const bool in_range_v = jv >= 0 && jv < static_cast<i64>(cells_v);
        const bool interior_v = in_range_v && jv >= 2 && static_cast<usize>(jv) + 4 <= height;
        if (outer_u(iu) || !interior_v) {
            // Scalar build, transposed to channel innermost.
            double scalar_anchor[cc][6];
            if (anchor != nullptr) {
                for (usize c = 0; c < cc; ++c) {
                    for (usize b = 0; b < 6; ++b) scalar_anchor[c][b] = anchor[b * cc + c];
                }
            }
            scalar_pool.clear();
            CellEntry inner;
            scalar_build(iu, jv, inner, scalar_pool, anchor != nullptr ? scalar_anchor : nullptr);
            if (failed) return;
            entry.strips = inner.strips;
            entry.offset = into.count();
            if (!grow(into, inner.strips * terms)) return;
            double* const to = into.mutable_view().data();
            const double* const from = scalar_pool.view().data();
            for (usize s = 0; s < inner.strips; ++s) {
                for (usize c = 0; c < cc; ++c) {
                    const double* const q = from + inner.offset + (s * cc + c) * q_terms;
                    for (usize k = 0; k < q_terms; ++k) to[entry.offset + s * terms + k * cc + c] = q[k];
                }
            }
            return;
        }
        // Row controls of the window, channel innermost: P[k][m][c].
        alignas(32) double P[8 * 6 * cc];
        for (usize k = 0; k < 8; ++k) {
            const i64 row = jv - 3 + static_cast<i64>(k);
            const usize r = row < 0 ? 0 : static_cast<usize>(row) >= height ? height - 1 : static_cast<usize>(row);
            double Pr[cc][6];
            row_controls(r, iu, Pr);
            for (usize m = 0; m < 6; ++m) {
                for (usize c = 0; c < cc; ++c) P[(k * 6 + m) * cc + c] = Pr[c][m];
            }
        }
        // R: the raw map of every control column, as one 24-lane K1 pass A (lane = m * 4 + c).
        alignas(32) double R[5 * 6 * cc];
        {
            const double* w[8];
            for (usize k = 0; k < 8; ++k) w[k] = P + k * 6 * cc;
            double* raw[5];
            for (usize k = 0; k < 5; ++k) raw[k] = R + k * 6 * cc;
            (void)(*kernels).pass_a(w, raw, nodes_list, 6 * cc);
        }
        alignas(32) double g[36 * cc];
        (*kernels).g_coefficients(R, P + 3 * 6 * cc, g);
        // Term-3 node contests: four strips of six Gauss nodes each.
        for (usize k = 0; k < strip_count; ++k) strips_now[k].k = k;
        contest_strips(P);
        bool contested = false;
        for (usize s = 0; s < strip_count; ++s) contested = contested || strips_now[s].contested;
        const usize strips = contested ? strip_count : 1;
        entry.strips = strips;
        entry.offset = into.count();
        if (!grow(into, strips * terms)) return;
        double* const to = into.mutable_view().data();
        alignas(32) double next_anchor[6 * cc];
        for (usize s = 0; s < strips; ++s) {
            const usize level = contested ? strip_level : 0;
            const usize k = contested ? strips_now[s].k : 0;
            const double width = 1.0 / static_cast<double>(usize{1} << level);
            const double t0 = static_cast<double>(k) * width;
            const double* a_s = anchor;
            if (s > 0) {
                (*kernels).q_at(to + entry.offset + (s - 1) * terms, t0, next_anchor);
                a_s = next_anchor;
            }
            double coef[6][6];
            dyadic_basis(level, k, coef);
            const double* const es = contested ? strips_now[s].e : nullptr;
            (*kernels).strip_q(g, es, &coef[0][0], a_s, t0, to + entry.offset + s * terms);
        }
    }

    void right_anchor(const i64 iu, const CellEntry& entry, const double* from, double* out) const noexcept {
        const double t_end = iu < 0 ? 0.0 : 1.0;
        (*kernels).q_at(from + entry.offset + (entry.strips - 1) * terms, t_end, out);
    }

    [[nodiscard]] bool in_box(const i64 iu, const i64 jv) const noexcept {
        return boxed && iu >= i0 && jv >= j0 && iu < i0 + static_cast<i64>(ni) && jv < j0 + static_cast<i64>(nj);
    }

    // The cached cell, building its row's chain up to it; anchors are continuous along u.
    // Null after an allocation failure.
    const CellEntry* cached(const i64 iu, const i64 jv) noexcept {
        if (failed) return nullptr;
        const usize row = static_cast<usize>(jv - j0);
        if (stamps[row] != stamp) {
            stamps[row] = stamp;
            built_until[row] = 0;
        }
        const usize want = static_cast<usize>(iu - i0);
        for (usize k = built_until[row]; k <= want; ++k) {
            CellEntry& entry = entries[row * ni + k];
            if (k == 0) {
                build(i0, jv, entry, pool, nullptr);
            } else {
                alignas(32) double left[6 * cc];
                right_anchor(i0 + static_cast<i64>(k) - 1, entries[row * ni + k - 1], pool.view().data(), left);
                build(i0 + static_cast<i64>(k), jv, entry, pool, left);
            }
            if (failed) return nullptr;
        }
        if (built_until[row] <= want) built_until[row] = want + 1;
        return &entries[row * ni + want];
    }

    void integrate_poly(const i64 iu, const CellEntry& entry, const double* from, const Poly& poly, const double weight) noexcept {
        const bool outer = outer_u(iu);
        for (usize s = 0; s < entry.strips; ++s) {
            Poly part = poly;
            if (entry.strips > 1 && !outer) {
                part = clip(part, 1.0, 0.0, -strip_bound(s, entry.strips));
                part = clip(part, -1.0, 0.0, strip_bound(s + 1, entry.strips));
                if (part.n < 3 || !(area(part) > 0.0)) continue;
            }
            double m[6][6];
            moments(part, m);
            (*kernels).moments_contract(from + entry.offset + s * terms, &m[0][0], weight, sum);
        }
    }

    void piece(const i64 iu, const i64 jv, const Poly& poly, const double weight) noexcept {
        if (failed) return;
        if (in_box(iu, jv)) {
            const CellEntry* const entry = cached(iu, jv);
            if (entry == nullptr) return;
            integrate_poly(iu, *entry, pool.view().data(), poly, weight);
            return;
        }
        scratch_pool.clear();
        CellEntry entry;
        build(iu, jv, entry, scratch_pool, nullptr);
        if (failed) return;
        integrate_poly(iu, entry, scratch_pool.view().data(), poly, weight);
    }

    // Integral of F times the linear density r0 + ru tu + rv tv over a region-local polygon.
    void piece_density(const i64 iu, const i64 jv, const Poly& poly, const double r0, const double ru, const double rv) noexcept {
        if (failed) return;
        CellEntry scratch_entry;
        const CellEntry* entry = &scratch_entry;
        const double* from = nullptr;
        if (in_box(iu, jv)) {
            entry = cached(iu, jv);
            if (entry == nullptr) return;
            from = pool.view().data();
        } else {
            scratch_pool.clear();
            build(iu, jv, scratch_entry, scratch_pool, nullptr);
            if (failed) return;
            from = scratch_pool.view().data();
        }
        const bool outer = outer_u(iu);
        for (usize s = 0; s < (*entry).strips; ++s) {
            Poly part = poly;
            if ((*entry).strips > 1 && !outer) {
                part = clip(part, 1.0, 0.0, -strip_bound(s, (*entry).strips));
                part = clip(part, -1.0, 0.0, strip_bound(s + 1, (*entry).strips));
                if (part.n < 3 || !(area(part) > 0.0)) continue;
            }
            double m[7][7];
            moments7(part, m);
            const double* const q = from + (*entry).offset + s * terms;
            for (usize c = 0; c < cc; ++c) {
                double v = 0.0;
                for (usize aa = 0; aa < 6; ++aa) {
                    for (usize bb = 0; bb < 6; ++bb) {
                        const double fq = q[((aa + 1) * 6 + bb) * cc + c] * static_cast<double>(aa + 1);
                        v = v + fq * (r0 * m[aa][bb] + ru * m[aa + 1][bb] + rv * m[aa][bb + 1]);
                    }
                }
                sum[c] = sum[c] + v;
            }
        }
    }

    // Near-singular piece: collapsed 6 x 6 Gauss over a fan of the destination piece through the
    // field's point evaluation (a quadrature fallback).
    void dest_piece(const Poly& poly, const Affine& map) noexcept {
        double value[cc];
        for (usize k = 1; k + 1 < poly.n; ++k) {
            const P2 v0 = poly.p[0];
            const P2 v1 = poly.p[k];
            const P2 v2 = poly.p[k + 1];
            const double jac = absolute((v1.x - v0.x) * (v2.y - v1.y) - (v2.x - v1.x) * (v1.y - v0.y));
            for (usize a = 0; a < 6; ++a) {
                for (usize b = 0; b < 6; ++b) {
                    const double xi = tables.gx[a];
                    const double eta = tables.gx[b];
                    const double px = v0.x + xi * (v1.x - v0.x) + xi * eta * (v2.x - v1.x);
                    const double py = v0.y + xi * (v1.y - v0.y) + xi * eta * (v2.y - v1.y);
                    double u = 0.0;
                    double v = 0.0;
                    map.at(px, py, u, v);
                    field_value(*field, u, v, value);
                    const double w = tables.gw[a] * tables.gw[b] * xi * jac;
                    for (usize c = 0; c < cc; ++c) sum[c] = sum[c] + w * value[c];
                }
            }
        }
    }

    // The integral of Q dv along the source segment a -> b, all channels. Every region the
    // segment crosses lies in the cache box (the caller checked the corners).
    void edge_flux(const P2 a, const P2 b, double* out) noexcept {
        for (usize c = 0; c < cc; ++c) out[c] = 0.0;
        if (failed) return;
        const double dv = b.y - a.y;
        if (dv == 0.0) return;
        double ts[64];
        usize nt = 0;
        ts[nt] = 0.0;
        nt = nt + 1;
        ts[nt] = 1.0;
        nt = nt + 1;
        const double du = b.x - a.x;
        if (du != 0.0) {
            const double lo = greatest(0.0, ceil_of(least(a.x, b.x)));
            const double hi = least(static_cast<double>(cells_u), floor_of(greatest(a.x, b.x)));
            for (double k = lo; k <= hi && nt + 1 < 64; k = k + 1.0) {
                const double t = (k - a.x) / du;
                if (t > 0.0 && t < 1.0) {
                    ts[nt] = t;
                    nt = nt + 1;
                }
            }
        }
        {
            const double lo = greatest(0.0, ceil_of(least(a.y, b.y)));
            const double hi = least(static_cast<double>(cells_v), floor_of(greatest(a.y, b.y)));
            for (double k = lo; k <= hi && nt + 1 < 64; k = k + 1.0) {
                const double t = (k - a.y) / dv;
                if (t > 0.0 && t < 1.0) {
                    ts[nt] = t;
                    nt = nt + 1;
                }
            }
        }
        sort_small(ts, nt);
        for (usize k = 0; k + 1 < nt; ++k) {
            const double t0 = ts[k];
            const double t1 = ts[k + 1];
            if (!(t1 - t0 > 0.0)) continue;
            const double tm = 0.5 * (t0 + t1);
            const i64 iu = region_of(a.x + tm * du, cells_u);
            const i64 jv = region_of(a.y + tm * dv, cells_v);
            const CellEntry* const found = cached(iu, jv);
            if (found == nullptr) return;
            const CellEntry& entry = *found;
            const double ou = region_origin(iu, cells_u);
            const double ov = region_origin(jv, cells_v);
            const bool outer = outer_u(iu);
            const double* const data = pool.view().data();
            double ss[strip_count + 2];
            usize ns = 0;
            ss[ns] = t0;
            ns = ns + 1;
            if (entry.strips > 1 && !outer && du != 0.0) {
                for (usize s = 1; s < entry.strips; ++s) {
                    const double t = (ou + strip_bound(s, entry.strips) - a.x) / du;
                    if (t > t0 && t < t1) {
                        ss[ns] = t;
                        ns = ns + 1;
                    }
                }
            }
            ss[ns] = t1;
            ns = ns + 1;
            sort_small(ss, ns);
            for (usize r = 0; r + 1 < ns; ++r) {
                const double s0 = ss[r];
                const double s1 = ss[r + 1];
                if (!(s1 - s0 > 0.0)) continue;
                const double sm = 0.5 * (s0 + s1);
                usize strip = 0;
                if (entry.strips > 1 && !outer) {
                    const double tum = a.x + sm * du - ou;
                    while (strip + 1 < entry.strips && !(tum < strip_bound(strip + 1, entry.strips))) strip = strip + 1;
                }
                double tu[6];
                double tv[6];
                double wt[6];
                for (usize g = 0; g < 6; ++g) {
                    const double t = s0 + (s1 - s0) * tables.gx[g];
                    tu[g] = a.x + t * du - ou;
                    tv[g] = a.y + t * dv - ov;
                    wt[g] = (s1 - s0) * tables.gw[g] * dv;
                }
                (*kernels).flux_points(data + entry.offset + strip * terms, 6, tu, tv, wt, out);
            }
        }
    }

    // Starts a tile: clears the sum and sets the cache box from the tile's source bounds. No box
    // when it would exceed cache_limit cells (the tile then builds every cell alone); failed on
    // an allocation failure.
    void begin_tile(const double umin, const double umax, const double vmin, const double vmax) noexcept {
        for (usize c = 0; c < cc; ++c) sum[c] = 0.0;
        boxed = false;
        if (failed) return;
        const i64 ia = region_of(umin, cells_u) - 1;
        const i64 ib = region_of(umax, cells_u) + 1;
        const i64 ja = region_of(vmin, cells_v) - 1;
        const i64 jb = region_of(vmax, cells_v) + 1;
        const usize box_ni = static_cast<usize>(ib - ia + 1);
        const usize box_nj = static_cast<usize>(jb - ja + 1);
        if (box_ni * box_nj > cache_limit) return;
        if (entries.count() < box_ni * box_nj && entries.resize(box_ni * box_nj).failed()) {
            failed = true;
            return;
        }
        if (stamps.count() < box_nj && (stamps.resize(box_nj).failed() || built_until.resize(box_nj).failed())) {
            failed = true;
            return;
        }
        pool.clear();
        if (stamp == 0xffffffffu) {
            for (usize k = 0; k < stamps.count(); ++k) stamps[k] = 0;
            stamp = 0;
        }
        stamp = stamp + 1;
        i0 = ia;
        j0 = ja;
        ni = box_ni;
        nj = box_nj;
        boxed = true;
    }

    static constexpr usize cache_limit = 4096; // region cells per tile box
};

// A canon value from a sum and the covered area; a non-finite result or an uncovered pixel is
// transparent black (never a cast of a NaN).
void finish(const double* total, const double covered, double* value) noexcept {
    bool ok = covered > 0.0 && finite(covered);
    for (usize c = 0; c < cc; ++c) {
        value[c] = ok ? total[c] / covered : 0.0;
        ok = ok && finite(value[c]);
    }
    if (!ok) {
        for (usize c = 0; c < cc; ++c) value[c] = 0.0;
    }
}

// ---------------------------------------------------------------- affine pieces

// The source pieces of a destination polygon under one affine map, into the sink: region
// polygons by moments, or destination-space quadrature when the map is near singular.
void polygon_pieces(const Poly& dest, const Affine& map, Sink& sink) noexcept {
    const usize cells_u = sink.cells_u;
    const usize cells_v = sink.cells_v;
    const double det_a = map.a11 * map.a22 - map.a12 * map.a21;
    const double scale = absolute(map.a11) + absolute(map.a12) + absolute(map.a21) + absolute(map.a22);
    double umin = 1e300;
    double umax = -1e300;
    double vmin = 1e300;
    double vmax = -1e300;
    Poly src;
    src.n = dest.n;
    for (usize k = 0; k < dest.n; ++k) {
        map.at(dest.p[k].x, dest.p[k].y, src.p[k].x, src.p[k].y);
        umin = least(umin, src.p[k].x);
        umax = greatest(umax, src.p[k].x);
        vmin = least(vmin, src.p[k].y);
        vmax = greatest(vmax, src.p[k].y);
    }
    const i64 iu0 = region_of(umin, cells_u);
    const i64 iu1 = region_of(umax, cells_u);
    const i64 jv0 = region_of(vmin, cells_v);
    const i64 jv1 = region_of(vmax, cells_v);
    const bool singular = !(absolute(det_a) > 1e-9 * (scale * scale + 1e-300));
    if (singular) {
        // Destination space: the region lines pulled back are half-planes in p.
        for (i64 iu = iu0; iu <= iu1; ++iu) {
            for (i64 jv = jv0; jv <= jv1; ++jv) {
                Poly piece = dest;
                const double cu = map.ox - map.a11 * map.dx - map.a12 * map.dy;
                const double cv = map.oy - map.a21 * map.dx - map.a22 * map.dy;
                if (iu >= 0) piece = clip(piece, map.a11, map.a12, cu - static_cast<double>(iu));
                if (iu < static_cast<i64>(cells_u)) piece = clip(piece, -map.a11, -map.a12, static_cast<double>(iu + 1) - cu);
                if (jv >= 0) piece = clip(piece, map.a21, map.a22, cv - static_cast<double>(jv));
                if (jv < static_cast<i64>(cells_v)) piece = clip(piece, -map.a21, -map.a22, static_cast<double>(jv + 1) - cv);
                if (piece.n < 3 || !(area(piece) > 0.0)) continue;
                sink.dest_piece(piece, map);
            }
        }
        return;
    }
    if (det_a < 0.0) {
        for (usize k = 0; k < src.n / 2; ++k) {
            const P2 swap = src.p[k];
            src.p[k] = src.p[src.n - 1 - k];
            src.p[src.n - 1 - k] = swap;
        }
    }
    const double weight = 1.0 / absolute(det_a);
    for (i64 iu = iu0; iu <= iu1; ++iu) {
        const Poly column = clip_region(src, true, iu, cells_u);
        if (column.n < 3) continue;
        const double ou = region_origin(iu, cells_u);
        for (i64 jv = jv0; jv <= jv1; ++jv) {
            Poly piece = clip_region(column, false, jv, cells_v);
            if (piece.n < 3) continue;
            const double ov = region_origin(jv, cells_v);
            for (usize k = 0; k < piece.n; ++k) {
                piece.p[k].x = piece.p[k].x - ou;
                piece.p[k].y = piece.p[k].y - ov;
            }
            if (!(area(piece) > 0.0)) continue;
            sink.piece(iu, jv, piece, weight);
        }
    }
}

Poly pixel_square(const usize x, const usize y) noexcept {
    const double fx0 = static_cast<double>(x) - 0.5;
    const double fy0 = static_cast<double>(y) - 0.5;
    Poly dest;
    dest.n = 4;
    dest.p[0].x = fx0;
    dest.p[0].y = fy0;
    dest.p[1].x = fx0 + 1.0;
    dest.p[1].y = fy0;
    dest.p[2].x = fx0 + 1.0;
    dest.p[2].y = fy0 + 1.0;
    dest.p[3].x = fx0;
    dest.p[3].y = fy0 + 1.0;
    return dest;
}

// ---------------------------------------------------------------- the two piecewise-affine maps

struct Triangle final {
    P2 d[3];
    P2 s[3];
};

// One affine map (AffineMap) or a mesh of triangles (MeshMap).
struct Geometry final {
    const AffineMap* affine{nullptr};
    const MeshMap* mesh{nullptr};
};

void square_triangles(const MeshMap& mesh, const usize cx, const usize cy, Triangle* out) noexcept {
    const double* const uv = mesh.source_uv.data();
    const usize n00 = cy * mesh.nodes_x + cx;
    const usize n10 = n00 + 1;
    const usize n01 = n00 + mesh.nodes_x;
    const usize n11 = n01 + 1;
    const double g = mesh.spacing;
    P2 d00;
    d00.x = static_cast<double>(cx) * g;
    d00.y = static_cast<double>(cy) * g;
    P2 d10;
    d10.x = d00.x + g;
    d10.y = d00.y;
    P2 d01;
    d01.x = d00.x;
    d01.y = d00.y + g;
    P2 d11;
    d11.x = d00.x + g;
    d11.y = d00.y + g;
    P2 s00;
    s00.x = uv[2 * n00];
    s00.y = uv[2 * n00 + 1];
    P2 s10;
    s10.x = uv[2 * n10];
    s10.y = uv[2 * n10 + 1];
    P2 s01;
    s01.x = uv[2 * n01];
    s01.y = uv[2 * n01 + 1];
    P2 s11;
    s11.x = uv[2 * n11];
    s11.y = uv[2 * n11 + 1];
    out[0].d[0] = d00;
    out[0].d[1] = d10;
    out[0].d[2] = d01;
    out[0].s[0] = s00;
    out[0].s[1] = s10;
    out[0].s[2] = s01;
    out[1].d[0] = d11;
    out[1].d[1] = d01;
    out[1].d[2] = d10;
    out[1].s[0] = s11;
    out[1].s[1] = s01;
    out[1].s[2] = s10;
}

// The affine map of a triangle (destination d, source s; d counterclockwise or not).
Affine triangle_map(const P2* d, const P2* s) noexcept {
    const double e1x = d[1].x - d[0].x;
    const double e1y = d[1].y - d[0].y;
    const double e2x = d[2].x - d[0].x;
    const double e2y = d[2].y - d[0].y;
    const double det_d = e1x * e2y - e2x * e1y;
    Affine map;
    map.ox = s[0].x;
    map.oy = s[0].y;
    map.dx = d[0].x;
    map.dy = d[0].y;
    const double f1x = s[1].x - s[0].x;
    const double f1y = s[1].y - s[0].y;
    const double f2x = s[2].x - s[0].x;
    const double f2y = s[2].y - s[0].y;
    map.a11 = (f1x * e2y - f2x * e1y) / det_d;
    map.a12 = (f2x * e1x - f1x * e2x) / det_d;
    map.a21 = (f1y * e2y - f2y * e1y) / det_d;
    map.a22 = (f2y * e1x - f1y * e2x) / det_d;
    return map;
}

Affine affine_of(const AffineMap& a) noexcept {
    Affine map;
    map.ox = a.ou;
    map.oy = a.ov;
    map.a11 = a.a11;
    map.a12 = a.a12;
    map.a21 = a.a21;
    map.a22 = a.a22;
    return map;
}

// The mesh map at a destination point (as the preview's).
void mesh_at(const MeshMap& mesh, const double x, const double y, double& u, double& v) noexcept {
    const double* const uv = mesh.source_uv.data();
    const double gxf = x / mesh.spacing;
    const double gyf = y / mesh.spacing;
    usize cx = static_cast<usize>(gxf);
    usize cy = static_cast<usize>(gyf);
    if (cx + 1 >= mesh.nodes_x) cx = mesh.nodes_x - 2;
    if (cy + 1 >= mesh.nodes_y) cy = mesh.nodes_y - 2;
    const double fx = gxf - static_cast<double>(cx);
    const double fy = gyf - static_cast<double>(cy);
    const usize n00 = cy * mesh.nodes_x + cx;
    const usize n10 = n00 + 1;
    const usize n01 = n00 + mesh.nodes_x;
    const usize n11 = n01 + 1;
    if (fx + fy <= 1.0) {
        u = uv[2 * n00] + fx * (uv[2 * n10] - uv[2 * n00]) + fy * (uv[2 * n01] - uv[2 * n00]);
        v = uv[2 * n00 + 1] + fx * (uv[2 * n10 + 1] - uv[2 * n00 + 1]) + fy * (uv[2 * n01 + 1] - uv[2 * n00 + 1]);
    } else {
        u = uv[2 * n11] + (1.0 - fx) * (uv[2 * n01] - uv[2 * n11]) + (1.0 - fy) * (uv[2 * n10] - uv[2 * n11]);
        v = uv[2 * n11 + 1] + (1.0 - fx) * (uv[2 * n01 + 1] - uv[2 * n11 + 1]) + (1.0 - fy) * (uv[2 * n10 + 1] - uv[2 * n11 + 1]);
    }
}

// The source point of a tile-box sample at destination (x, y).
void geometry_at(const Geometry& geometry, const double x, const double y, double& u, double& v) noexcept {
    if (geometry.mesh != nullptr) {
        mesh_at(*geometry.mesh, greatest(0.0, x), greatest(0.0, y), u, v);
        return;
    }
    affine_of(*geometry.affine).at(x, y, u, v);
}

bool nonsingular(const Affine& map, double& det) noexcept {
    det = map.a11 * map.a22 - map.a12 * map.a21;
    const double scale = absolute(map.a11) + absolute(map.a12) + absolute(map.a21) + absolute(map.a22);
    return absolute(det) > 1e-9 * (scale * scale + 1e-300);
}

// Whether destination pixel (x, y) lies wholly inside one non-singular affine piece; if so its
// id, map and determinant.
bool pixel_in_one_piece(const Geometry& geometry, const usize x, const usize y, usize& id, Affine& map, double& det) noexcept {
    if (geometry.mesh == nullptr) {
        id = 0;
        map = affine_of(*geometry.affine);
        return nonsingular(map, det);
    }
    const MeshMap& mesh = *geometry.mesh;
    const double g = mesh.spacing;
    const double fx0 = static_cast<double>(x) - 0.5;
    const double fy0 = static_cast<double>(y) - 0.5;
    if (fx0 < 0.0 || fy0 < 0.0) return false;
    const double cxd = floor_of(fx0 / g);
    const double cyd = floor_of(fy0 / g);
    if (floor_of((fx0 + 1.0) / g) != cxd && (fx0 + 1.0) / g != cxd + 1.0) return false;
    if (floor_of((fy0 + 1.0) / g) != cyd && (fy0 + 1.0) / g != cyd + 1.0) return false;
    if (cxd + 2.0 > static_cast<double>(mesh.nodes_x) || cyd + 2.0 > static_cast<double>(mesh.nodes_y)) return false;
    const usize cx = static_cast<usize>(cxd);
    const usize cy = static_cast<usize>(cyd);
    const double lx0 = fx0 / g - cxd;
    const double ly0 = fy0 / g - cyd;
    const double lx1 = (fx0 + 1.0) / g - cxd;
    const double ly1 = (fy0 + 1.0) / g - cyd;
    const bool lower = lx1 + ly1 <= 1.0;
    const bool upper = lx0 + ly0 >= 1.0;
    if (!lower && !upper) return false;
    Triangle tris[2];
    square_triangles(mesh, cx, cy, tris);
    const Triangle& tri = lower ? tris[0] : tris[1];
    id = (cy * mesh.nodes_x + cx) * 2 + (lower ? 0 : 1);
    map = triangle_map(tri.d, tri.s);
    return nonsingular(map, det);
}

// The pieces of destination pixel (x, y) into the sink; returns the covered destination area.
double pixel_pieces(const Geometry& geometry, const usize x, const usize y, Sink& sink) noexcept {
    if (geometry.mesh == nullptr) {
        const Poly dest = pixel_square(x, y);
        const double covered = area(dest);
        polygon_pieces(dest, affine_of(*geometry.affine), sink);
        return covered;
    }
    const MeshMap& mesh = *geometry.mesh;
    const double g = mesh.spacing;
    const double fx0 = static_cast<double>(x) - 0.5;
    const double fy0 = static_cast<double>(y) - 0.5;
    const double last_x = static_cast<double>(mesh.nodes_x - 2);
    const double last_y = static_cast<double>(mesh.nodes_y - 2);
    const usize cx0 = static_cast<usize>(clamp(floor_of(fx0 / g), 0.0, last_x));
    const usize cy0 = static_cast<usize>(clamp(floor_of(fy0 / g), 0.0, last_y));
    const usize cx1 = static_cast<usize>(clamp(floor_of((fx0 + 1.0) / g), 0.0, last_x));
    const usize cy1 = static_cast<usize>(clamp(floor_of((fy0 + 1.0) / g), 0.0, last_y));
    double covered = 0.0;
    for (usize cy = cy0; cy <= cy1; ++cy) {
        for (usize cx = cx0; cx <= cx1; ++cx) {
            Triangle tris[2];
            square_triangles(mesh, cx, cy, tris);
            for (usize t = 0; t < 2; ++t) {
                Poly dest = pixel_square(x, y);
                P2 d[3] = {tris[t].d[0], tris[t].d[1], tris[t].d[2]};
                P2 s[3] = {tris[t].s[0], tris[t].s[1], tris[t].s[2]};
                const double orient = (d[1].x - d[0].x) * (d[2].y - d[0].y) - (d[2].x - d[0].x) * (d[1].y - d[0].y);
                if (orient < 0.0) {
                    const P2 swap_d = d[1];
                    d[1] = d[2];
                    d[2] = swap_d;
                    const P2 swap_s = s[1];
                    s[1] = s[2];
                    s[2] = swap_s;
                }
                for (usize e = 0; e < 3 && dest.n >= 3; ++e) {
                    const P2 a = d[e];
                    const P2 b = d[(e + 1) % 3];
                    dest = clip(dest, -(b.y - a.y), b.x - a.x, (b.y - a.y) * a.x - (b.x - a.x) * a.y);
                }
                if (dest.n < 3) continue;
                const double dest_area = area(dest);
                if (!(dest_area > 0.0)) continue;
                covered = covered + dest_area;
                polygon_pieces(dest, triangle_map(d, s), sink);
            }
        }
    }
    return covered;
}

// ---------------------------------------------------------------- drivers

constexpr usize tile = 16;

// Per-call storage: the sink (heap, for its batch buffers) and the region's values.
struct CallScope final {
    Sink* sink{nullptr};
    ~CallScope() {
        Sink* const held = sink;
        sink = nullptr;
        if (held == nullptr) return;
        (*held).~Sink();
        memory::release(held);
    }
};

OperationStatus exhausted() noexcept { return OperationStatus::failure(ErrorRecord::make(ErrorKind::out_of_memory, "stxlib.conv.transfer")); }

// Allocates the sink and the region's value buffer.
bool prepare(const FieldState& field, const Region& region, WorkspaceState& workspace, CallScope& scope) noexcept {
    const usize count = (region.x1 - region.x0) * (region.y1 - region.y0) * cc;
    if (workspace.values.resize(count).failed()) return false;
    void* const block = memory::allocate(sizeof(Sink), alignof(Sink));
    if (block == nullptr) return false;
    scope.sink = ::new (block) Sink();
    Sink& sink = *scope.sink;
    sink.field = &field;
    sink.kernels = field.kernels;
    sink.cells_u = field.width - 1;
    sink.cells_v = field.height - 1;
    return true;
}

void encode_region(const double* values, const MutableRgba8Image& destination, const Region& region) noexcept {
    const usize width = region.x1 - region.x0;
    for (usize y = region.y0; y < region.y1; ++y) {
        for (usize x = region.x0; x < region.x1; ++x) {
            u8* const pixel = destination.pixels.data() + y * destination.row_stride + x * 4;
            encode(values + ((y - region.y0) * width + (x - region.x0)) * cc, pixel);
        }
    }
}

// The flux state of pixel (x, y): whether it takes edge flux, its piece and the source corners.
bool flux_state(const Geometry& geometry, Sink& sink, const usize x, const usize y, usize& id, double& det, P2* corner) noexcept {
    Affine map;
    bool use = sink.boxed && pixel_in_one_piece(geometry, x, y, id, map, det);
    if (!use) return false;
    const double fx0 = static_cast<double>(x) - 0.5;
    const double fy0 = static_cast<double>(y) - 0.5;
    map.at(fx0, fy0, corner[0].x, corner[0].y);
    map.at(fx0 + 1.0, fy0, corner[1].x, corner[1].y);
    map.at(fx0 + 1.0, fy0 + 1.0, corner[2].x, corner[2].y);
    map.at(fx0, fy0 + 1.0, corner[3].x, corner[3].y);
    for (usize k = 0; k < 4 && use; ++k) use = sink.in_box(region_of(corner[k].x, sink.cells_u), region_of(corner[k].y, sink.cells_v));
    return use;
}

constexpr usize no_piece = ~usize{0};

OperationStatus piecewise_affine_canon(const FieldState& field, const Geometry& geometry, const MutableRgba8Image& destination,
                                       const Region& region, WorkspaceState& workspace) noexcept {
    if (region.x0 >= region.x1 || region.y0 >= region.y1) return OperationStatus::success();
    CallScope scope;
    if (!prepare(field, region, workspace, scope)) return exhausted();
    Sink& sink = *scope.sink;
    double* const values = workspace.values.mutable_view().data();
    const usize region_width = region.x1 - region.x0;
    double top_value[tile][cc]{};
    usize top_piece[tile]{};
    for (usize ty0 = (region.y0 / tile) * tile; ty0 < region.y1; ty0 += tile) {
        const usize ty1 = least_size(destination.height, ty0 + tile);
        for (usize tx0 = (region.x0 / tile) * tile; tx0 < region.x1; tx0 += tile) {
            const usize tx1 = least_size(destination.width, tx0 + tile);
            double umin = 1e300;
            double umax = -1e300;
            double vmin = 1e300;
            double vmax = -1e300;
            for (usize y = ty0; y <= ty1; ++y) {
                for (usize x = tx0; x <= tx1; ++x) {
                    double u = 0.0;
                    double v = 0.0;
                    geometry_at(geometry, static_cast<double>(x) - 0.5, static_cast<double>(y) - 0.5, u, v);
                    umin = least(umin, u);
                    umax = greatest(umax, u);
                    vmin = least(vmin, v);
                    vmax = greatest(vmax, v);
                }
            }
            sink.begin_tile(umin, umax, vmin, vmax);
            if (sink.failed) return exhausted();
            const usize x_first = tx0 > region.x0 ? tx0 : region.x0;
            const usize x_last = tx1 < region.x1 ? tx1 : region.x1;
            const usize y_first = ty0 > region.y0 ? ty0 : region.y0;
            const usize y_last = ty1 < region.y1 ? ty1 : region.y1;
            for (usize k = 0; k < tile; ++k) top_piece[k] = no_piece;
            // The row above the region inside this tile: its top edges, as the whole-image pass
            // has them.
            if (y_first > ty0) {
                for (usize x = x_first; x < x_last; ++x) {
                    usize id = 0;
                    double det = 0.0;
                    P2 corner[4];
                    if (flux_state(geometry, sink, x, y_first - 1, id, det, corner)) {
                        sink.edge_flux(corner[2], corner[3], top_value[x - tx0]);
                        top_piece[x - tx0] = id;
                    }
                }
            }
            for (usize y = y_first; y < y_last; ++y) {
                double right_value[cc]{0.0, 0.0, 0.0, 0.0};
                usize right_piece = no_piece;
                if (x_first > tx0) {
                    usize id = 0;
                    double det = 0.0;
                    P2 corner[4];
                    if (flux_state(geometry, sink, x_first - 1, y, id, det, corner)) {
                        sink.edge_flux(corner[1], corner[2], right_value);
                        right_piece = id;
                    }
                }
                for (usize x = x_first; x < x_last; ++x) {
                    for (usize c = 0; c < cc; ++c) sink.sum[c] = 0.0;
                    double* const out = values + ((y - region.y0) * region_width + (x - region.x0)) * cc;
                    const usize col = x - tx0;
                    usize id = 0;
                    double det = 0.0;
                    P2 corner[4];
                    if (flux_state(geometry, sink, x, y, id, det, corner)) {
                        double bottom[cc];
                        double right[cc];
                        double top[cc];
                        double left[cc];
                        if (y > ty0 && top_piece[col] == id) {
                            for (usize c = 0; c < cc; ++c) bottom[c] = -top_value[col][c];
                        } else {
                            sink.edge_flux(corner[0], corner[1], bottom);
                        }
                        if (x > tx0 && right_piece == id) {
                            for (usize c = 0; c < cc; ++c) left[c] = -right_value[c];
                        } else {
                            sink.edge_flux(corner[3], corner[0], left);
                        }
                        sink.edge_flux(corner[1], corner[2], right);
                        sink.edge_flux(corner[2], corner[3], top);
                        double value[cc];
                        for (usize c = 0; c < cc; ++c) {
                            value[c] = (bottom[c] + right[c] + top[c] + left[c]) / det;
                            top_value[col][c] = top[c];
                            right_value[c] = right[c];
                        }
                        top_piece[col] = id;
                        right_piece = id;
                        finish(value, 1.0, out);
                        continue;
                    }
                    top_piece[col] = no_piece;
                    right_piece = no_piece;
                    const double covered = pixel_pieces(geometry, x, y, sink);
                    finish(sink.sum, covered, out);
                }
            }
            if (sink.failed) return exhausted();
        }
    }
    encode_region(values, destination, region);
    return OperationStatus::success();
}

// ---------------------------------------------------------------- perspective, option (b)

struct Homography final {
    double h[9]{};
    [[nodiscard]] double w(const double x, const double y) const noexcept { return h[6] * x + h[7] * y + h[8]; }
    void at(const double x, const double y, double& u, double& v) const noexcept {
        const double d = w(x, y);
        u = (h[0] * x + h[1] * y + h[2]) / d;
        v = (h[3] * x + h[4] * y + h[5]) / d;
    }
    [[nodiscard]] double det() const noexcept {
        return h[0] * (h[4] * h[8] - h[5] * h[7]) - h[1] * (h[3] * h[8] - h[5] * h[6]) + h[2] * (h[3] * h[7] - h[4] * h[6]);
    }
};

P2 point(const double x, const double y) noexcept {
    P2 p;
    p.x = x;
    p.y = y;
    return p;
}

// The integral over one destination triangle through its exact source triangle, the density
// 1 / (|det H| w_s^3) replaced by its linear interpolant on the source triangle (values
// |w_d|^3 / |det H| at the vertices); subdivided while the density varies too much.
void perspective_triangle(Sink& sink, const Homography& H, const P2 d0, const P2 d1, const P2 d2, const usize depth) noexcept {
    const double w0 = H.w(d0.x, d0.y);
    const double w1 = H.w(d1.x, d1.y);
    const double w2 = H.w(d2.x, d2.y);
    const double wmin = least(w0, least(w1, w2));
    const double wmax = greatest(w0, greatest(w1, w2));
    const double variation = (wmax - wmin) / wmin;
    if (1.5 * variation * variation > 1.0 / 65536.0 && depth < 8) {
        const P2 a = point(0.5 * (d0.x + d1.x), 0.5 * (d0.y + d1.y));
        const P2 b = point(0.5 * (d1.x + d2.x), 0.5 * (d1.y + d2.y));
        const P2 c = point(0.5 * (d2.x + d0.x), 0.5 * (d2.y + d0.y));
        perspective_triangle(sink, H, d0, a, c, depth + 1);
        perspective_triangle(sink, H, a, d1, b, depth + 1);
        perspective_triangle(sink, H, c, b, d2, depth + 1);
        perspective_triangle(sink, H, a, b, c, depth + 1);
        return;
    }
    const double det = absolute(H.det());
    P2 s[3];
    H.at(d0.x, d0.y, s[0].x, s[0].y);
    H.at(d1.x, d1.y, s[1].x, s[1].y);
    H.at(d2.x, d2.y, s[2].x, s[2].y);
    const double rho[3] = {w0 * w0 * w0 / det, w1 * w1 * w1 / det, w2 * w2 * w2 / det};
    const double e1x = s[1].x - s[0].x;
    const double e1y = s[1].y - s[0].y;
    const double e2x = s[2].x - s[0].x;
    const double e2y = s[2].y - s[0].y;
    const double area2 = e1x * e2y - e2x * e1y;
    if (!(absolute(area2) > 0.0)) return;
    const double ru = ((rho[1] - rho[0]) * e2y - (rho[2] - rho[0]) * e1y) / area2;
    const double rv = ((rho[2] - rho[0]) * e1x - (rho[1] - rho[0]) * e2x) / area2;
    const double r0 = rho[0] - ru * s[0].x - rv * s[0].y;
    Poly src;
    src.n = 3;
    src.p[0] = s[0];
    src.p[1] = s[1];
    src.p[2] = s[2];
    if (area2 < 0.0) {
        src.p[1] = s[2];
        src.p[2] = s[1];
    }
    const double umin = least(s[0].x, least(s[1].x, s[2].x));
    const double umax = greatest(s[0].x, greatest(s[1].x, s[2].x));
    const double vmin = least(s[0].y, least(s[1].y, s[2].y));
    const double vmax = greatest(s[0].y, greatest(s[1].y, s[2].y));
    const i64 iu0 = region_of(umin, sink.cells_u);
    const i64 iu1 = region_of(umax, sink.cells_u);
    const i64 jv0 = region_of(vmin, sink.cells_v);
    const i64 jv1 = region_of(vmax, sink.cells_v);
    for (i64 iu = iu0; iu <= iu1; ++iu) {
        const Poly column = clip_region(src, true, iu, sink.cells_u);
        if (column.n < 3) continue;
        // Outside the source along an axis F does not depend on that coordinate, so the local
        // origin there is the triangle's own midpoint: the boundary can be thousands of cells
        // away, and r0 + ru u + rv v and the degree-7 moments would cancel catastrophically.
        const bool outer_u = iu < 0 || iu >= static_cast<i64>(sink.cells_u);
        const double ou = outer_u ? 0.5 * (umin + umax) : region_origin(iu, sink.cells_u);
        for (i64 jv = jv0; jv <= jv1; ++jv) {
            Poly piece = clip_region(column, false, jv, sink.cells_v);
            if (piece.n < 3) continue;
            const bool outer_v = jv < 0 || jv >= static_cast<i64>(sink.cells_v);
            const double ov = outer_v ? 0.5 * (vmin + vmax) : region_origin(jv, sink.cells_v);
            for (usize k = 0; k < piece.n; ++k) {
                piece.p[k].x = piece.p[k].x - ou;
                piece.p[k].y = piece.p[k].y - ov;
            }
            if (!(area(piece) > 0.0)) continue;
            sink.piece_density(iu, jv, piece, r0 + ru * ou + rv * ov, ru, rv);
        }
    }
}

} // namespace

OperationStatus affine_canon(const FieldState& field, const AffineMap& map, const MutableRgba8Image& destination, const Region& region,
                             WorkspaceState& workspace) noexcept {
    Geometry geometry;
    geometry.affine = &map;
    return piecewise_affine_canon(field, geometry, destination, region, workspace);
}

OperationStatus mesh_canon(const FieldState& field, const MeshMap& map, const MutableRgba8Image& destination, const Region& region,
                           WorkspaceState& workspace) noexcept {
    Geometry geometry;
    geometry.mesh = &map;
    return piecewise_affine_canon(field, geometry, destination, region, workspace);
}

OperationStatus projective_canon(const FieldState& field, const ProjectiveMap& map, const MutableRgba8Image& destination,
                                 const Region& region, WorkspaceState& workspace) noexcept {
    if (region.x0 >= region.x1 || region.y0 >= region.y1) return OperationStatus::success();
    CallScope scope;
    if (!prepare(field, region, workspace, scope)) return exhausted();
    Sink& sink = *scope.sink;
    Homography H;
    for (usize k = 0; k < 9; ++k) H.h[k] = map.h[k];
    double* const values = workspace.values.mutable_view().data();
    const usize region_width = region.x1 - region.x0;
    for (usize ty0 = (region.y0 / tile) * tile; ty0 < region.y1; ty0 += tile) {
        const usize ty1 = least_size(destination.height, ty0 + tile);
        for (usize tx0 = (region.x0 / tile) * tile; tx0 < region.x1; tx0 += tile) {
            const usize tx1 = least_size(destination.width, tx0 + tile);
            // A projective map keeps lines straight: the tile's footprint is the quadrilateral of
            // its corners' images.
            double umin = 1e300;
            double umax = -1e300;
            double vmin = 1e300;
            double vmax = -1e300;
            const double xs[2] = {static_cast<double>(tx0) - 0.5, static_cast<double>(tx1) - 0.5};
            const double ys[2] = {static_cast<double>(ty0) - 0.5, static_cast<double>(ty1) - 0.5};
            for (usize a = 0; a < 2; ++a) {
                for (usize b = 0; b < 2; ++b) {
                    double u = 0.0;
                    double v = 0.0;
                    H.at(xs[a], ys[b], u, v);
                    umin = least(umin, u);
                    umax = greatest(umax, u);
                    vmin = least(vmin, v);
                    vmax = greatest(vmax, v);
                }
            }
            sink.begin_tile(umin, umax, vmin, vmax);
            if (sink.failed) return exhausted();
            const usize x_first = tx0 > region.x0 ? tx0 : region.x0;
            const usize x_last = tx1 < region.x1 ? tx1 : region.x1;
            const usize y_first = ty0 > region.y0 ? ty0 : region.y0;
            const usize y_last = ty1 < region.y1 ? ty1 : region.y1;
            for (usize y = y_first; y < y_last; ++y) {
                for (usize x = x_first; x < x_last; ++x) {
                    for (usize c = 0; c < cc; ++c) sink.sum[c] = 0.0;
                    const double fx0 = static_cast<double>(x) - 0.5;
                    const double fy0 = static_cast<double>(y) - 0.5;
                    perspective_triangle(sink, H, point(fx0, fy0), point(fx0 + 1.0, fy0), point(fx0, fy0 + 1.0), 0);
                    perspective_triangle(sink, H, point(fx0 + 1.0, fy0 + 1.0), point(fx0, fy0 + 1.0), point(fx0 + 1.0, fy0), 0);
                    finish(sink.sum, 1.0, values + ((y - region.y0) * region_width + (x - region.x0)) * cc);
                }
            }
            if (sink.failed) return exhausted();
        }
    }
    encode_region(values, destination, region);
    return OperationStatus::success();
}

} // namespace stxlib::conv::detail
