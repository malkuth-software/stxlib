// Tests of stxlib::conv (include/stxlib/conv.hpp).
//
// - The field against an unseparated restatement of the stx_conv Surface (paper closure,
//   bubble off): every row cell and every vertical cell compiled whole, no K1, no
//   corrections. Values agree to 1e-12 (the separated field returns the raw currents where
//   the Surface's projection returns them to rounding; line.hpp).
// - K1 on every tier this machine runs against the scalar tier, bit for bit.
// - Preview transfers equal the encoded field at each map's pulled-back centre; a transfer
//   writes only its region, and a refused one writes nothing.
// - Axis canon: a constant source maps to itself; small cases against a direct quadrature of
//   the field through Field::evaluate.
// - A digest of preview and canon outputs, frozen, so each forced SIMD level (ctest
//   stxlib_conv_tests_<level>) must give the same bytes.

#include "stxlib/conv.hpp"
#include "stxlib/simd.hpp"

#include "algorithms/conv/kernels.hpp"
#include "algorithms/conv/line.hpp"

#include "test_support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace conv = stxlib::conv;
namespace line = stxlib::conv::detail;
using stxlib::u8;
using stxlib::u32;
using stxlib::u64;
using stxlib::usize;

// The digest of digest_outputs() on the fixtures below; 0 prints it without checking.
constexpr u64 expected_digest = 0x273d96e8953415eeull;

struct Random final {
    u64 state = 0x9e3779b97f4a7c15ull;
    u32 next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const u32 result = static_cast<u32>(state >> 33);
        return result;
    }
};

enum class Kind { noise, smooth, icon };

struct Image final {
    usize width = 0;
    usize height = 0;
    std::vector<u8> rgba;
    [[nodiscard]] conv::Rgba8Image view() const {
        conv::Rgba8Image result;
        result.pixels = stxlib::View<u8>(rgba.data(), rgba.size());
        result.width = width;
        result.height = height;
        result.row_stride = width * 4;
        return result;
    }
};

Image fixture(const Kind kind, const usize width, const usize height, const u32 seed) {
    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(width * height * 4);
    Random random;
    random.state = random.state + seed;
    for (usize y = 0; y < height; ++y) {
        for (usize x = 0; x < width; ++x) {
            u8* const p = image.rgba.data() + (y * width + x) * 4;
            switch (kind) {
            case Kind::noise:
                for (usize c = 0; c < 4; ++c) p[c] = static_cast<u8>(random.next() >> 24);
                break;
            case Kind::smooth:
                p[0] = static_cast<u8>((x * 255) / (width > 1 ? width - 1 : 1));
                p[1] = static_cast<u8>((y * 255) / (height > 1 ? height - 1 : 1));
                p[2] = static_cast<u8>(((x + y) * 97) % 256);
                p[3] = 255;
                break;
            case Kind::icon: {
                const bool inside = (x * 3 > width && x * 3 < 2 * width && y * 3 > height) || (x + y) % 7 == 0;
                p[0] = inside ? 230 : 0;
                p[1] = inside ? 40 : 0;
                p[2] = inside ? 90 : 0;
                p[3] = inside ? 255 : (random.next() % 5 == 0 ? 128 : 0);
                break;
            }
            }
        }
    }
    return image;
}

// ---------------------------------------------------------------- the unseparated reference

struct Reference final {
    usize width = 0;
    usize height = 0;
    std::vector<double> decoded;  // (y width + x) 4 + c
    std::vector<double> cells;    // ((y cells + i) 4 + c) 6: anchor, five currents

    explicit Reference(const Image& image) : width(image.width), height(image.height) {
        decoded.resize(width * height * 4);
        for (usize k = 0; k < width * height; ++k) line::decode(image.rgba.data() + k * 4, decoded.data() + k * 4);
        const usize n = width - 1;
        cells.resize(height * n * 4 * 6);
        for (usize y = 0; y < height; ++y) {
            for (usize i = 0; i < n; ++i) {
                const line::CellShape shape = line::cell_shape(i, width);
                for (usize c = 0; c < 4; ++c) {
                    double w[8];
                    for (usize k = 0; k < 8; ++k) {
                        const long long x = static_cast<long long>(i) - 3 + static_cast<long long>(k);
                        const usize clamped = x < 0 ? 0 : static_cast<usize>(x) >= width ? width - 1 : static_cast<usize>(x);
                        w[k] = decoded[(y * width + clamped) * 4 + c];
                    }
                    double raw[5];
                    double* const cell = cells.data() + ((y * n + i) * 4 + c) * 6;
                    line::raw_for_shape(w, shape, raw);
                    (void)line::admit_cell(w, shape, raw, cell + 1, false);
                    cell[0] = w[3];
                }
            }
        }
    }

    void value(const double u, const double v, double* out) const {
        usize i = 0;
        usize j = 0;
        double tu = 0.0;
        double tv = 0.0;
        line::locate(u, width - 1, i, tu);
        line::locate(v, height - 1, j, tv);
        double tauu[5];
        double tauv[5];
        line::tail_weights(tu, tauu);
        line::tail_weights(tv, tauv);
        const line::CellShape shape = line::cell_shape(j, height);
        for (usize c = 0; c < 4; ++c) {
            double w[8];
            for (usize k = 0; k < 8; ++k) {
                const long long row = static_cast<long long>(j) - 3 + static_cast<long long>(k);
                const usize clamped = row < 0 ? 0 : static_cast<usize>(row) >= height ? height - 1 : static_cast<usize>(row);
                const double* const cell = cells.data() + ((clamped * (width - 1) + i) * 4 + c) * 6;
                w[k] = line::evaluate_cell(cell[0], cell + 1, tauu);
            }
            double raw[5];
            double admitted[5];
            line::raw_for_shape(w, shape, raw);
            (void)line::admit_cell(w, shape, raw, admitted, false);
            out[c] = line::evaluate_cell(w[3], admitted, tauv);
        }
    }
};

conv::Field compiled(const Image& image) {
    stxlib::OperationResult<conv::Field> made = conv::Field::make(image.width, image.height, conv::FieldOptions{});
    CHECK(made.succeeded());
    conv::Field field = static_cast<stxlib::OperationResult<conv::Field>&&>(made).take_value();
    CHECK(field.compile(image.view()).succeeded());
    return field;
}

void field_matches_reference() {
    const usize sizes[][2] = {{2, 2}, {3, 5}, {4, 4}, {5, 7}, {6, 6}, {7, 3}, {9, 12}, {61, 37}};
    const Kind kinds[] = {Kind::noise, Kind::smooth, Kind::icon};
    double worst = 0.0;
    for (const usize(&size)[2] : sizes) {
        for (const Kind kind : kinds) {
            const Image image = fixture(kind, size[0], size[1], static_cast<u32>(size[0] * 31 + size[1]));
            const conv::Field field = compiled(image);
            CHECK(field.ready());
            const Reference reference(image);
            Random random;
            for (usize q = 0; q < 400; ++q) {
                // Points across and slightly outside the source, plus exact nodes.
                const double u = q % 5 == 0 ? static_cast<double>(q % size[0])
                                            : (static_cast<double>(random.next()) / 4294967296.0) * (static_cast<double>(size[0]) + 1.0) - 1.0;
                const double v = q % 7 == 0 ? static_cast<double>(q % size[1])
                                            : (static_cast<double>(random.next()) / 4294967296.0) * (static_cast<double>(size[1]) + 1.0) - 1.0;
                double got[4];
                double want[4];
                CHECK(field.evaluate(u, v, stxlib::MutableView<double>(got, 4)).succeeded());
                reference.value(u, v, want);
                for (usize c = 0; c < 4; ++c) {
                    const double difference = std::fabs(got[c] - want[c]);
                    worst = difference > worst ? difference : worst;
                }
            }
        }
    }
    std::printf("field against the unseparated Surface: largest difference %.3g\n", worst);
    CHECK(worst <= 1e-12);
}

// ---------------------------------------------------------------- K1 across tiers

struct K1Out final {
    std::vector<double> raw;
    std::vector<u32> list;
    usize count = 0;
    std::vector<double> admitted;
    std::vector<u8> idle;
};

K1Out run_k1(const line::Kernels& kernels, const std::vector<double>& samples, const usize n) {
    K1Out out;
    const double* window[8];
    for (usize k = 0; k < 8; ++k) window[k] = samples.data() + k;
    out.raw.assign(5 * n, 0.0);
    out.list.assign(n, 0);
    double* raw[5];
    for (usize r = 0; r < 5; ++r) raw[r] = out.raw.data() + r * n;
    out.count = kernels.pass_a(window, raw, out.list.data(), n);
    // Pass B over the first n lanes as given (every lane, contested or not).
    out.admitted.assign(5 * n, 0.0);
    out.idle.assign(n, 0);
    const double* raw_in[5];
    double* admitted[5];
    for (usize r = 0; r < 5; ++r) {
        raw_in[r] = raw[r];
        admitted[r] = out.admitted.data() + r * n;
    }
    kernels.pass_b(window, raw_in, admitted, out.idle.data(), n);
    return out;
}

void k1_tiers_identical() {
    const usize n = 1003;
    std::vector<double> samples(n + 8);
    Random random;
    for (usize k = 0; k < samples.size(); ++k) {
        const u32 r = random.next();
        // Steps, plateaus and noise, all byte-decoded values.
        const u32 pick = r % 4;
        samples[k] = pick == 0 ? 0.0 : pick == 1 ? 1.0 : static_cast<double>((r >> 8) % 256) / 255.0;
    }
    const K1Out scalar = run_k1(line::scalar::kernels(), samples, n);
    usize tiers = 1;
    const stxlib::simd::Level hardware = stxlib::simd::hardware_level();
    if (hardware >= stxlib::simd::Level::vector128) {
        const K1Out v128 = run_k1(line::kernels_for(stxlib::simd::Level::vector128), samples, n);
        CHECK(v128.count == scalar.count);
        CHECK(std::memcmp(v128.raw.data(), scalar.raw.data(), scalar.raw.size() * sizeof(double)) == 0);
        CHECK(std::memcmp(v128.list.data(), scalar.list.data(), scalar.count * sizeof(u32)) == 0);
        CHECK(std::memcmp(v128.admitted.data(), scalar.admitted.data(), scalar.admitted.size() * sizeof(double)) == 0);
        CHECK(v128.idle == scalar.idle);
        tiers = tiers + 1;
    }
    if (hardware >= stxlib::simd::Level::avx2_fma) {
        const K1Out avx2 = run_k1(line::kernels_for(stxlib::simd::Level::avx2_fma), samples, n);
        CHECK(avx2.count == scalar.count);
        CHECK(std::memcmp(avx2.raw.data(), scalar.raw.data(), scalar.raw.size() * sizeof(double)) == 0);
        CHECK(std::memcmp(avx2.list.data(), scalar.list.data(), scalar.count * sizeof(u32)) == 0);
        CHECK(std::memcmp(avx2.admitted.data(), scalar.admitted.data(), scalar.admitted.size() * sizeof(double)) == 0);
        CHECK(avx2.idle == scalar.idle);
        tiers = tiers + 1;
    }
    std::printf("K1: %zu tiers identical (%zu of %zu lanes contested)\n", tiers, scalar.count, n);
}

// ---------------------------------------------------------------- transfers

struct Destination final {
    usize width = 0;
    usize height = 0;
    std::vector<u8> rgba;
    Destination(const usize w, const usize h) : width(w), height(h), rgba(w * h * 4, 0xa5) {}
    [[nodiscard]] conv::MutableRgba8Image view() {
        conv::MutableRgba8Image result;
        result.pixels = stxlib::MutableView<u8>(rgba.data(), rgba.size());
        result.width = width;
        result.height = height;
        result.row_stride = width * 4;
        return result;
    }
};

conv::Workspace workspace_made() {
    stxlib::OperationResult<conv::Workspace> made = conv::Workspace::make();
    CHECK(made.succeeded());
    return static_cast<stxlib::OperationResult<conv::Workspace>&&>(made).take_value();
}

void expect_pixel(const conv::Field& field, const double u, const double v, const u8* got, usize& mismatches) {
    double value[4];
    CHECK(field.evaluate(u, v, stxlib::MutableView<double>(value, 4)).succeeded());
    u8 want[4];
    line::encode(value, want);
    if (std::memcmp(got, want, 4) != 0) mismatches = mismatches + 1;
}

void previews_match_field() {
    const Image image = fixture(Kind::icon, 40, 30, 7);
    const conv::Field field = compiled(image);
    conv::Workspace workspace = workspace_made();
    usize mismatches = 0;
    // Axis.
    {
        Destination out(57, 23);
        conv::AxisMap map;
        map.target_width = 57;
        map.target_height = 23;
        const conv::Region all{0, 0, 57, 23};
        CHECK(conv::transfer(field, map, conv::Mode::preview, out.view(), all, workspace).succeeded());
        for (usize y = 0; y < 23; ++y) {
            for (usize x = 0; x < 57; ++x) {
                const double u = static_cast<double>(x) * 39.0 / 56.0;
                const double v = static_cast<double>(y) * 29.0 / 22.0;
                expect_pixel(field, u, v, out.rgba.data() + (y * 57 + x) * 4, mismatches);
            }
        }
    }
    // Affine (a rotation about the centre), on a region only.
    {
        Destination out(40, 30);
        conv::AffineMap map;
        const double c = std::cos(0.417);
        const double s = std::sin(0.417);
        map.a11 = c;
        map.a12 = s;
        map.a21 = -s;
        map.a22 = c;
        map.ou = 19.5 - c * 19.5 - s * 14.5;
        map.ov = 14.5 + s * 19.5 - c * 14.5;
        const conv::Region region{5, 4, 31, 22};
        CHECK(conv::transfer(field, map, conv::Mode::preview, out.view(), region, workspace).succeeded());
        for (usize y = 0; y < 30; ++y) {
            for (usize x = 0; x < 40; ++x) {
                const u8* const p = out.rgba.data() + (y * 40 + x) * 4;
                const bool inside = x >= 5 && x < 31 && y >= 4 && y < 22;
                if (!inside) {
                    CHECK(p[0] == 0xa5 && p[1] == 0xa5 && p[2] == 0xa5 && p[3] == 0xa5);
                    continue;
                }
                const double fx = static_cast<double>(x);
                const double fy = static_cast<double>(y);
                expect_pixel(field, map.ou + map.a11 * fx + map.a12 * fy, map.ov + map.a21 * fx + map.a22 * fy, p, mismatches);
            }
        }
    }
    // Mesh: 4 x 3 nodes, 16 px apart, perturbed, one fold.
    {
        Destination out(48, 32);
        std::vector<double> uv;
        for (usize gy = 0; gy < 3; ++gy) {
            for (usize gx = 0; gx < 4; ++gx) {
                uv.push_back(static_cast<double>(gx) * 13.0 + 2.0 * std::sin(1.7 * static_cast<double>(gx + gy)));
                uv.push_back(static_cast<double>(gy) * 14.0 + 2.0 * std::cos(1.1 * static_cast<double>(gy) - 0.5 * static_cast<double>(gx)));
            }
        }
        uv[2 * 5] = uv[2 * 6] + 3.0;  // a fold
        conv::MeshMap map;
        map.nodes_x = 4;
        map.nodes_y = 3;
        map.spacing = 16.0;
        map.source_uv = stxlib::View<double>(uv.data(), uv.size());
        const conv::Region all{0, 0, 48, 32};
        CHECK(conv::transfer(field, map, conv::Mode::preview, out.view(), all, workspace).succeeded());
        for (usize y = 0; y < 32; ++y) {
            for (usize x = 0; x < 48; ++x) {
                const double gxf = static_cast<double>(x) / 16.0;
                const double gyf = static_cast<double>(y) / 16.0;
                usize cx = static_cast<usize>(gxf);
                usize cy = static_cast<usize>(gyf);
                if (cx + 1 >= 4) cx = 2;
                if (cy + 1 >= 3) cy = 1;
                const double fx = gxf - static_cast<double>(cx);
                const double fy = gyf - static_cast<double>(cy);
                const usize n00 = cy * 4 + cx;
                const usize n10 = n00 + 1;
                const usize n01 = n00 + 4;
                const usize n11 = n01 + 1;
                double u = 0.0;
                double v = 0.0;
                if (fx + fy <= 1.0) {
                    u = uv[2 * n00] + fx * (uv[2 * n10] - uv[2 * n00]) + fy * (uv[2 * n01] - uv[2 * n00]);
                    v = uv[2 * n00 + 1] + fx * (uv[2 * n10 + 1] - uv[2 * n00 + 1]) + fy * (uv[2 * n01 + 1] - uv[2 * n00 + 1]);
                } else {
                    u = uv[2 * n11] + (1.0 - fx) * (uv[2 * n01] - uv[2 * n11]) + (1.0 - fy) * (uv[2 * n10] - uv[2 * n11]);
                    v = uv[2 * n11 + 1] + (1.0 - fx) * (uv[2 * n01 + 1] - uv[2 * n11 + 1]) + (1.0 - fy) * (uv[2 * n10 + 1] - uv[2 * n11 + 1]);
                }
                expect_pixel(field, u, v, out.rgba.data() + (y * 48 + x) * 4, mismatches);
            }
        }
    }
    // Projective; then a map whose w is not positive is refused and writes nothing.
    {
        Destination out(40, 30);
        conv::ProjectiveMap map;
        const double h[9] = {1.05, 0.08, -1.0, 0.03, 0.97, 0.5, 0.0012, 0.0005, 1.0};
        for (usize k = 0; k < 9; ++k) map.h[k] = h[k];
        const conv::Region all{0, 0, 40, 30};
        CHECK(conv::transfer(field, map, conv::Mode::preview, out.view(), all, workspace).succeeded());
        for (usize y = 0; y < 30; ++y) {
            for (usize x = 0; x < 40; ++x) {
                const double fx = static_cast<double>(x);
                const double fy = static_cast<double>(y);
                const double w = h[6] * fx + h[7] * fy + h[8];
                expect_pixel(field, (h[0] * fx + h[1] * fy + h[2]) / w, (h[3] * fx + h[4] * fy + h[5]) / w, out.rgba.data() + (y * 40 + x) * 4,
                             mismatches);
            }
        }
        Destination untouched(40, 30);
        map.h[8] = -1.0;
        CHECK(conv::transfer(field, map, conv::Mode::preview, untouched.view(), all, workspace).failed());
        for (const u8 b : untouched.rgba) CHECK(b == 0xa5);
    }
    std::printf("previews against the field: %zu mismatched pixels\n", mismatches);
    CHECK(mismatches == 0);
}

// The direct quadrature of the field over one axis-canon pixel through Field::evaluate.
void direct_average(const conv::Field& field, const usize x, const usize y, const usize target_width, const usize target_height, double* out) {
    const double gx[3] = {-0.7745966692414834, 0.0, 0.7745966692414834};
    const double gw[3] = {5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
    const double su = static_cast<double>(field.width()) / static_cast<double>(target_width);
    const double sv = static_cast<double>(field.height()) / static_cast<double>(target_height);
    const double u0 = static_cast<double>(x) * su - 0.5;
    const double u1 = static_cast<double>(x + 1) * su - 0.5;
    const double v0 = static_cast<double>(y) * sv - 0.5;
    const double v1 = static_cast<double>(y + 1) * sv - 0.5;
    for (usize c = 0; c < 4; ++c) out[c] = 0.0;
    for (double a = u0; a < u1;) {
        double b = std::floor(a) + 1.0;
        b = b > u1 ? u1 : b;
        for (double p = v0; p < v1;) {
            double q = std::floor(p) + 1.0;
            q = q > v1 ? v1 : q;
            for (usize i = 0; i < 3; ++i) {
                for (usize j = 0; j < 3; ++j) {
                    const double u = 0.5 * (a + b) + 0.5 * (b - a) * gx[i];
                    const double v = 0.5 * (p + q) + 0.5 * (q - p) * gx[j];
                    const double weight = gw[i] * 0.5 * (b - a) / (u1 - u0) * gw[j] * 0.5 * (q - p) / (v1 - v0);
                    double value[4];
                    CHECK(field.evaluate(u, v, stxlib::MutableView<double>(value, 4)).succeeded());
                    for (usize c = 0; c < 4; ++c) out[c] = out[c] + weight * value[c];
                }
            }
            p = q;
        }
        a = b;
    }
}

void axis_canon() {
    conv::Workspace workspace = workspace_made();
    // A constant source maps to itself, up and down.
    {
        Image image = fixture(Kind::smooth, 17, 13, 1);
        for (usize k = 0; k < image.rgba.size(); k += 4) {
            image.rgba[k] = 200;
            image.rgba[k + 1] = 31;
            image.rgba[k + 2] = 7;
            image.rgba[k + 3] = 180;
        }
        const conv::Field field = compiled(image);
        const usize targets[][2] = {{40, 29}, {5, 4}, {17, 13}};
        for (const usize(&target)[2] : targets) {
            Destination out(target[0], target[1]);
            conv::AxisMap map;
            map.target_width = target[0];
            map.target_height = target[1];
            const conv::Region all{0, 0, target[0], target[1]};
            CHECK(conv::transfer(field, map, conv::Mode::canon, out.view(), all, workspace).succeeded());
            usize off = 0;
            for (usize k = 0; k < out.rgba.size(); k += 4) {
                const bool same = out.rgba[k] == 200 && out.rgba[k + 1] == 31 && out.rgba[k + 2] == 7 && out.rgba[k + 3] == 180;
                off = off + (same ? 0 : 1);
            }
            CHECK(off == 0);
        }
    }
    // Against the direct quadrature: up, down, mixed.
    const Image image = fixture(Kind::noise, 23, 19, 3);
    const conv::Field field = compiled(image);
    const usize targets[][2] = {{51, 40}, {7, 6}, {31, 9}};
    usize exact = 0;
    usize total = 0;
    int worst = 0;
    for (const usize(&target)[2] : targets) {
        Destination out(target[0], target[1]);
        conv::AxisMap map;
        map.target_width = target[0];
        map.target_height = target[1];
        const conv::Region all{0, 0, target[0], target[1]};
        CHECK(conv::transfer(field, map, conv::Mode::canon, out.view(), all, workspace).succeeded());
        for (usize y = 0; y < target[1]; ++y) {
            for (usize x = 0; x < target[0]; ++x) {
                double average[4];
                direct_average(field, x, y, target[0], target[1], average);
                u8 want[4];
                line::encode(average, want);
                const u8* const got = out.rgba.data() + (y * target[0] + x) * 4;
                bool same = true;
                for (usize c = 0; c < 4; ++c) {
                    const int d = static_cast<int>(got[c]) - static_cast<int>(want[c]);
                    const int a = d < 0 ? -d : d;
                    worst = a > worst ? a : worst;
                    same = same && d == 0;
                }
                exact = exact + (same ? 1 : 0);
                total = total + 1;
            }
        }
    }
    std::printf("axis canon against direct quadrature: %zu of %zu pixels exact, largest difference %d codes\n", exact, total, worst);
    CHECK(worst <= 1);
    CHECK(exact * 100 >= total * 99);
}


// ---------------------------------------------------------------- affine and mesh canon

// The average of the field over destination pixel (x, y)'s square [x - 1/2, x + 1/2]^2,
// pulled back through `at`, by an m x m grid of 3 x 3 Gauss sub-squares in destination space
// (an independent quadrature: no source-space geometry, no flux, no cell cache).
struct PullBack {
    virtual ~PullBack() = default;
    virtual void at(double x, double y, double& u, double& v) const = 0;
};

struct AffinePullBack final : PullBack {
    conv::AffineMap map;
    void at(const double x, const double y, double& u, double& v) const override {
        u = map.ou + map.a11 * x + map.a12 * y;
        v = map.ov + map.a21 * x + map.a22 * y;
    }
};

struct MeshPullBack final : PullBack {
    const std::vector<double>* uv = nullptr;
    usize nodes_x = 0;
    usize nodes_y = 0;
    double spacing = 1.0;
    void at(const double x, const double y, double& u, double& v) const override {
        const std::vector<double>& p = *uv;
        const double gxf = x / spacing;
        const double gyf = y / spacing;
        usize cx = static_cast<usize>(gxf);
        usize cy = static_cast<usize>(gyf);
        if (cx + 1 >= nodes_x) cx = nodes_x - 2;
        if (cy + 1 >= nodes_y) cy = nodes_y - 2;
        const double fx = gxf - static_cast<double>(cx);
        const double fy = gyf - static_cast<double>(cy);
        const usize n00 = cy * nodes_x + cx;
        const usize n10 = n00 + 1;
        const usize n01 = n00 + nodes_x;
        const usize n11 = n01 + 1;
        if (fx + fy <= 1.0) {
            u = p[2 * n00] + fx * (p[2 * n10] - p[2 * n00]) + fy * (p[2 * n01] - p[2 * n00]);
            v = p[2 * n00 + 1] + fx * (p[2 * n10 + 1] - p[2 * n00 + 1]) + fy * (p[2 * n01 + 1] - p[2 * n00 + 1]);
        } else {
            u = p[2 * n11] + (1.0 - fx) * (p[2 * n01] - p[2 * n11]) + (1.0 - fy) * (p[2 * n10] - p[2 * n11]);
            v = p[2 * n11 + 1] + (1.0 - fx) * (p[2 * n01 + 1] - p[2 * n11 + 1]) + (1.0 - fy) * (p[2 * n10 + 1] - p[2 * n11 + 1]);
        }
    }
};

void destination_average(const conv::Field& field, const PullBack& map, const usize x, const usize y, const usize m, double* out) {
    const double gx[3] = {-0.7745966692414834, 0.0, 0.7745966692414834};
    const double gw[3] = {5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
    for (usize c = 0; c < 4; ++c) out[c] = 0.0;
    const double h = 1.0 / static_cast<double>(m);
    for (usize a = 0; a < m; ++a) {
        for (usize b = 0; b < m; ++b) {
            const double cx = static_cast<double>(x) - 0.5 + (static_cast<double>(a) + 0.5) * h;
            const double cy = static_cast<double>(y) - 0.5 + (static_cast<double>(b) + 0.5) * h;
            for (usize i = 0; i < 3; ++i) {
                for (usize j = 0; j < 3; ++j) {
                    double u = 0.0;
                    double v = 0.0;
                    map.at(cx + 0.5 * h * gx[i], cy + 0.5 * h * gx[j], u, v);
                    double value[4];
                    CHECK(field.evaluate(u, v, stxlib::MutableView<double>(value, 4)).succeeded());
                    const double weight = gw[i] * gw[j] * 0.25 * h * h;
                    for (usize c = 0; c < 4; ++c) out[c] = out[c] + weight * value[c];
                }
            }
        }
    }
}

struct Agreement final {
    usize exact = 0;
    usize total = 0;
    int worst = 0;
};

void compare_pixel(const conv::Field& field, const PullBack& map, const u8* got, const usize x, const usize y, const usize m, Agreement& agreement) {
    double average[4];
    destination_average(field, map, x, y, m, average);
    u8 want[4];
    line::encode(average, want);
    // Premultiplied codes (the bench's adjudication metric): straight colour divided by a small
    // alpha amplifies the canon's term-3 interpolation difference by 1 / alpha.
    bool same = std::memcmp(got, want, 4) == 0;
    for (usize c = 0; c < 4; ++c) {
        const double g = c == 3 ? static_cast<double>(got[3]) : static_cast<double>(got[c]) * static_cast<double>(got[3]) / 255.0;
        const double w = c == 3 ? static_cast<double>(want[3]) : static_cast<double>(want[c]) * static_cast<double>(want[3]) / 255.0;
        const int d = static_cast<int>(std::lround(g)) - static_cast<int>(std::lround(w));
        const int a = d < 0 ? -d : d;
        agreement.worst = a > agreement.worst ? a : agreement.worst;
    }
    agreement.exact = agreement.exact + (same ? 1 : 0);
    agreement.total = agreement.total + 1;
}

void affine_and_mesh_canon() {
    // 200 x 200, so a 16 px tile at five source cells a pixel (80 x 80 cells) passes the
    // 4,096-cell box limit and takes the uncached path.
    const Image image = fixture(Kind::icon, 200, 200, 5);
    const conv::Field field = compiled(image);
    conv::Workspace workspace = workspace_made();
    const conv::Region all{0, 0, 24, 20};
    // A rotation, a skew, and five source cells a destination pixel (uncached).
    const double maps[3][6] = {{0.9, 0.4, -0.4, 0.9, 60.0, 80.0}, {1.7, 0.6, 0.0, 1.3, 10.0, 20.0}, {5.0, 0.4, -0.3, 5.0, 20.0, 30.0}};
    const usize steps[3] = {8, 12, 24};
    for (usize k = 0; k < 3; ++k) {
        AffinePullBack pull;
        pull.map.a11 = maps[k][0];
        pull.map.a12 = maps[k][1];
        pull.map.a21 = maps[k][2];
        pull.map.a22 = maps[k][3];
        pull.map.ou = maps[k][4];
        pull.map.ov = maps[k][5];
        Destination out(24, 20);
        CHECK(conv::transfer(field, pull.map, conv::Mode::canon, out.view(), all, workspace).succeeded());
        Agreement agreement;
        for (usize y = 0; y < 20; ++y) {
            for (usize x = 0; x < 24; ++x) compare_pixel(field, pull, out.rgba.data() + (y * 24 + x) * 4, x, y, steps[k], agreement);
        }
        std::printf("affine canon %zu against destination quadrature: %zu of %zu bytes-exact, largest %d premultiplied codes\n", k, agreement.exact, agreement.total,
                    agreement.worst);
        CHECK(agreement.worst <= 2);
        CHECK(agreement.exact * 100 >= agreement.total * 90);
    }
    // A mesh at five source cells a pixel (the uncached path), and a mildly deformed one with a fold.
    for (usize k = 0; k < 2; ++k) {
        std::vector<double> uv;
        const usize nx = 4;
        const usize ny = 4;
        const double spacing = 8.0;
        for (usize gy = 0; gy < ny; ++gy) {
            for (usize gx = 0; gx < nx; ++gx) {
                const double scale = k == 0 ? 5.0 : 1.2;
                uv.push_back(3.0 + scale * static_cast<double>(gx) * spacing + (k == 0 ? 0.0 : 1.5 * std::sin(1.3 * static_cast<double>(gx + 2 * gy))));
                uv.push_back(2.0 + scale * static_cast<double>(gy) * spacing + (k == 0 ? 0.0 : 1.5 * std::cos(0.7 * static_cast<double>(gx) + static_cast<double>(gy))));
            }
        }
        if (k == 1) uv[2 * 5] = uv[2 * 6] + 2.0;
        MeshPullBack pull;
        pull.uv = &uv;
        pull.nodes_x = nx;
        pull.nodes_y = ny;
        pull.spacing = spacing;
        conv::MeshMap map;
        map.nodes_x = nx;
        map.nodes_y = ny;
        map.spacing = spacing;
        map.source_uv = stxlib::View<double>(uv.data(), uv.size());
        Destination out(24, 24);
        const conv::Region region{0, 0, 24, 24};
        CHECK(conv::transfer(field, map, conv::Mode::canon, out.view(), region, workspace).succeeded());
        Agreement agreement;
        // Pixels wholly inside the grid [0, 24]^2: x, y in [1, 23).
        for (usize y = 1; y < 23; ++y) {
            for (usize x = 1; x < 23; ++x) compare_pixel(field, pull, out.rgba.data() + (y * 24 + x) * 4, x, y, k == 0 ? 24 : 12, agreement);
        }
        std::printf("mesh canon %zu against destination quadrature: %zu of %zu bytes-exact, largest %d premultiplied codes\n", k, agreement.exact, agreement.total,
                    agreement.worst);
        CHECK(agreement.worst <= 2);
        CHECK(agreement.exact * 100 >= agreement.total * 90);
    }
    // A region gives the bytes the whole image gives there.
    AffinePullBack pull;
    pull.map.a11 = 0.9;
    pull.map.a12 = 0.4;
    pull.map.a21 = -0.4;
    pull.map.a22 = 0.9;
    pull.map.ou = 60.0;
    pull.map.ov = 80.0;
    Destination whole(40, 36);
    Destination part(40, 36);
    CHECK(conv::transfer(field, pull.map, conv::Mode::canon, whole.view(), conv::Region{0, 0, 40, 36}, workspace).succeeded());
    const conv::Region inner{5, 19, 37, 33};
    CHECK(conv::transfer(field, pull.map, conv::Mode::canon, part.view(), inner, workspace).succeeded());
    usize differing = 0;
    for (usize y = 0; y < 36; ++y) {
        for (usize x = 0; x < 40; ++x) {
            const u8* const a = whole.rgba.data() + (y * 40 + x) * 4;
            const u8* const b = part.rgba.data() + (y * 40 + x) * 4;
            const bool inside = x >= 5 && x < 37 && y >= 19 && y < 33;
            if (inside) {
                differing = differing + (std::memcmp(a, b, 4) == 0 ? 0 : 1);
            } else {
                CHECK(b[0] == 0xa5 && b[1] == 0xa5 && b[2] == 0xa5 && b[3] == 0xa5);
            }
        }
    }
    std::printf("canon region against the whole image: %zu differing pixels\n", differing);
    CHECK(differing == 0);
}


struct ProjectivePullBack final : PullBack {
    double h[9]{};
    void at(const double x, const double y, double& u, double& v) const override {
        const double w = h[6] * x + h[7] * y + h[8];
        u = (h[0] * x + h[1] * y + h[2]) / w;
        v = (h[3] * x + h[4] * y + h[5]) / w;
    }
};

void projective_canon() {
    const Image image = fixture(Kind::icon, 60, 50, 13);
    const conv::Field field = compiled(image);
    conv::Workspace workspace = workspace_made();
    // Horizon strength k over a 40 x 40 destination (as the bench's perspective_of): mild, and
    // strong (w from 1.75 to 0.25, the lower rows reaching far outside the source).
    const double strengths[2] = {0.2, 1.5};
    for (usize s = 0; s < 2; ++s) {
        const double k = strengths[s];
        const double n = 40.0;
        const double c = 0.5 * (n - 1.0);
        const double a = -k / (n - 1.0);
        const double b = 1.0 + 0.5 * k;
        ProjectivePullBack pull;
        const double h[9] = {1.0, c * a, c * b - c, 0.0, 1.0 + c * a, c * b - c, 0.0, a, b};
        for (usize m = 0; m < 9; ++m) pull.h[m] = h[m];
        conv::ProjectiveMap map;
        for (usize m = 0; m < 9; ++m) map.h[m] = h[m];
        Destination out(40, 40);
        CHECK(conv::transfer(field, map, conv::Mode::canon, out.view(), conv::Region{0, 0, 40, 40}, workspace).succeeded());
        Agreement agreement;
        for (usize y = 0; y < 40; ++y) {
            for (usize x = 0; x < 40; ++x) compare_pixel(field, pull, out.rgba.data() + (y * 40 + x) * 4, x, y, 12, agreement);
        }
        std::printf("projective canon k %.1f against destination quadrature: %zu of %zu bytes-exact, largest %d premultiplied codes\n", k,
                    agreement.exact, agreement.total, agreement.worst);
        CHECK(agreement.worst <= 2);
        CHECK(agreement.exact * 100 >= agreement.total * 90);
    }
}

// ---------------------------------------------------------------- lines

void line_field() {
    // A line made of one decoded image row equals the 2D field on that row, bit for bit.
    const Image image = fixture(Kind::noise, 37, 9, 21);
    const conv::Field field = compiled(image);
    const usize row = 4;
    std::vector<double> samples(37 * 4);
    for (usize x = 0; x < 37; ++x) line::decode(image.rgba.data() + (row * 37 + x) * 4, samples.data() + x * 4);
    stxlib::OperationResult<conv::LineField> made = conv::LineField::make(37, 4, conv::FieldOptions{});
    CHECK(made.succeeded());
    conv::LineField whole_line = static_cast<stxlib::OperationResult<conv::LineField>&&>(made).take_value();
    conv::LineSamples input;
    input.values = stxlib::View<double>(samples.data(), samples.size());
    input.nodes = 37;
    input.lanes = 4;
    input.node_stride = 4;
    CHECK(whole_line.compile(input).succeeded());
    usize differing = 0;
    for (usize q = 0; q <= 720; ++q) {
        const double u = static_cast<double>(q) * 0.05 - 0.5;
        double a[4];
        double b[4];
        CHECK(whole_line.evaluate(u, stxlib::MutableView<double>(a, 4)).succeeded());
        CHECK(field.evaluate(u, static_cast<double>(row), stxlib::MutableView<double>(b, 4)).succeeded());
        differing = differing + (std::memcmp(a, b, sizeof(a)) == 0 ? 0 : 1);
    }
    std::printf("line field against the 2D field's row: %zu of 721 points differ\n", differing);
    CHECK(differing == 0);
    // Canon against a dense quadrature through evaluate (ten Gauss pieces per footprint cell
    // piece); preview and points against evaluate; one lane, strided target.
    const usize targets[3] = {101, 13, 37};
    double worst = 0.0;
    for (const usize n : targets) {
        std::vector<double> out(n * 6, -7.0);
        conv::MutableLineSamples target;
        target.values = stxlib::MutableView<double>(out.data(), out.size());
        target.nodes = n;
        target.lanes = 4;
        target.node_stride = 6;
        CHECK(conv::transfer(whole_line, conv::Mode::canon, target).succeeded());
        const double scale = 37.0 / static_cast<double>(n);
        for (usize k = 0; k < n; ++k) {
            const double left = static_cast<double>(k) * scale - 0.5;
            const double right = static_cast<double>(k + 1) * scale - 0.5;
            double sum[4] = {0.0, 0.0, 0.0, 0.0};
            const double gx[3] = {-0.7745966692414834, 0.0, 0.7745966692414834};
            const double gw[3] = {5.0 / 9.0, 8.0 / 9.0, 5.0 / 9.0};
            for (double a = left; a < right;) {
                double b = std::floor(a) + 1.0;
                b = b > right ? right : b;
                for (usize p = 0; p < 10; ++p) {
                    const double a0 = a + (b - a) * static_cast<double>(p) / 10.0;
                    const double a1 = a + (b - a) * static_cast<double>(p + 1) / 10.0;
                    for (usize g = 0; g < 3; ++g) {
                        double value[4];
                        CHECK(whole_line.evaluate(0.5 * (a0 + a1) + 0.5 * (a1 - a0) * gx[g], stxlib::MutableView<double>(value, 4)).succeeded());
                        for (usize c = 0; c < 4; ++c) sum[c] = sum[c] + gw[g] * 0.5 * (a1 - a0) / (right - left) * value[c];
                    }
                }
                a = b;
            }
            for (usize c = 0; c < 4; ++c) {
                const double d = std::fabs(out[k * 6 + c] - sum[c]);
                worst = d > worst ? d : worst;
            }
            CHECK(out[k * 6 + 4] == -7.0 && out[k * 6 + 5] == -7.0);
        }
        CHECK(conv::transfer(whole_line, conv::Mode::preview, target).succeeded());
        for (usize k = 0; k < n; ++k) {
            double value[4];
            CHECK(whole_line.evaluate(static_cast<double>(k) * 36.0 / static_cast<double>(n - 1), stxlib::MutableView<double>(value, 4)).succeeded());
            CHECK(std::memcmp(value, out.data() + k * 6, sizeof(value)) == 0);
        }
    }
    std::printf("line canon against dense quadrature: largest difference %.3g\n", worst);
    CHECK(worst <= 1e-12);
    std::vector<double> positions = {-3.0, 0.0, 0.25, 17.5, 35.999, 36.0, 99.0};
    std::vector<double> out(positions.size() * 4);
    conv::MutableLineSamples target;
    target.values = stxlib::MutableView<double>(out.data(), out.size());
    target.nodes = positions.size();
    target.lanes = 4;
    target.node_stride = 4;
    CHECK(conv::transfer_points(whole_line, stxlib::View<double>(positions.data(), positions.size()), target).succeeded());
    for (usize k = 0; k < positions.size(); ++k) {
        double value[4];
        CHECK(whole_line.evaluate(positions[k], stxlib::MutableView<double>(value, 4)).succeeded());
        CHECK(std::memcmp(value, out.data() + k * 4, sizeof(value)) == 0);
    }
    // Short lines (2 to 5 nodes, the short-line jets) against the unseparated restatement.
    for (usize n = 2; n <= 7; ++n) {
        const Image tiny = fixture(Kind::noise, n, 2, static_cast<u32>(n));
        const Reference reference(tiny);
        std::vector<double> row0(n * 4);
        for (usize x = 0; x < n; ++x) line::decode(tiny.rgba.data() + x * 4, row0.data() + x * 4);
        stxlib::OperationResult<conv::LineField> small = conv::LineField::make(n, 4, conv::FieldOptions{});
        CHECK(small.succeeded());
        conv::LineField short_line = static_cast<stxlib::OperationResult<conv::LineField>&&>(small).take_value();
        conv::LineSamples in;
        in.values = stxlib::View<double>(row0.data(), row0.size());
        in.nodes = n;
        in.lanes = 4;
        in.node_stride = 4;
        CHECK(short_line.compile(in).succeeded());
        for (usize q = 0; q <= 40; ++q) {
            const double u = static_cast<double>(q) * static_cast<double>(n - 1) / 40.0;
            double a[4];
            double b[4];
            CHECK(short_line.evaluate(u, stxlib::MutableView<double>(a, 4)).succeeded());
            reference.value(u, 0.0, b);
            for (usize c = 0; c < 4; ++c) CHECK(std::fabs(a[c] - b[c]) <= 1e-12);
        }
    }
    CHECK(conv::LineField::make(1, 4, conv::FieldOptions{}).failed());
    CHECK(conv::LineField::make(8, conv::max_line_lanes + 1, conv::FieldOptions{}).failed());
}

void refusals() {
    CHECK(conv::Field::make(1, 5, conv::FieldOptions{}).failed());
    CHECK(conv::Field::make(5, conv::max_side + 1, conv::FieldOptions{}).failed());
    conv::FieldOptions bubble;
    bubble.bubble = true;
    stxlib::OperationResult<conv::Field> refused = conv::Field::make(8, 8, bubble);
    CHECK(refused.failed() && (*refused.error()).kind() == stxlib::ErrorKind::unsupported);
    stxlib::OperationResult<conv::Field> made = conv::Field::make(8, 8, conv::FieldOptions{});
    CHECK(made.succeeded());
    conv::Field field = static_cast<stxlib::OperationResult<conv::Field>&&>(made).take_value();
    double value[4];
    CHECK(field.evaluate(1.0, 1.0, stxlib::MutableView<double>(value, 4)).failed());
    const Image wrong = fixture(Kind::noise, 9, 8, 1);
    CHECK(field.compile(wrong.view()).failed());
    CHECK(!field.ready());
    const Image right = fixture(Kind::noise, 8, 8, 1);
    CHECK(field.compile(right.view()).succeeded());
    CHECK(field.storage_bytes() > 8 * 8 * 4);
    conv::Workspace workspace = workspace_made();
    Destination out(8, 8);
    const conv::Region outside{0, 0, 9, 8};
    conv::AffineMap identity;
    CHECK(conv::transfer(field, identity, conv::Mode::preview, out.view(), outside, workspace).failed());
    conv::Workspace empty;
    const conv::Region all{0, 0, 8, 8};
    CHECK(conv::transfer(field, identity, conv::Mode::preview, out.view(), all, empty).failed());
    for (const u8 b : out.rgba) CHECK(b == 0xa5);
}

// ---------------------------------------------------------------- the frozen digest

u64 fnv(u64 hash, const std::vector<u8>& bytes) {
    for (const u8 b : bytes) {
        hash = hash ^ b;
        hash = hash * 0x100000001b3ull;
    }
    return hash;
}

void digest_outputs() {
    u64 hash = 0xcbf29ce484222325ull;
    conv::Workspace workspace = workspace_made();
    const Kind kinds[] = {Kind::noise, Kind::smooth, Kind::icon};
    for (const Kind kind : kinds) {
        const Image image = fixture(kind, 97, 71, 11);
        const conv::Field field = compiled(image);
        const usize targets[][2] = {{203, 150}, {41, 29}};
        for (const usize(&target)[2] : targets) {
            conv::AxisMap map;
            map.target_width = target[0];
            map.target_height = target[1];
            const conv::Region all{0, 0, target[0], target[1]};
            Destination preview(target[0], target[1]);
            CHECK(conv::transfer(field, map, conv::Mode::preview, preview.view(), all, workspace).succeeded());
            hash = fnv(hash, preview.rgba);
            Destination canon(target[0], target[1]);
            CHECK(conv::transfer(field, map, conv::Mode::canon, canon.view(), all, workspace).succeeded());
            hash = fnv(hash, canon.rgba);
        }
        Destination rotated(97, 71);
        conv::AffineMap map;
        map.a11 = 0.9;
        map.a12 = 0.4;
        map.a21 = -0.4;
        map.a22 = 0.9;
        map.ou = 3.0;
        map.ov = 20.0;
        const conv::Region all{0, 0, 97, 71};
        CHECK(conv::transfer(field, map, conv::Mode::preview, rotated.view(), all, workspace).succeeded());
        hash = fnv(hash, rotated.rgba);
        Destination rotated_canon(97, 71);
        CHECK(conv::transfer(field, map, conv::Mode::canon, rotated_canon.view(), all, workspace).succeeded());
        hash = fnv(hash, rotated_canon.rgba);
    }
    std::printf("digest %016llx on level %d\n", static_cast<unsigned long long>(hash), static_cast<int>(stxlib::simd::selected_level()));
    if (expected_digest != 0) CHECK(hash == expected_digest);
}

} // namespace

int main() {
    field_matches_reference();
    k1_tiers_identical();
    previews_match_field();
    axis_canon();
    affine_and_mesh_canon();
    projective_canon();
    line_field();
    refusals();
    digest_outputs();
    return stxlib_tests::finish("stxlib conv");
}
