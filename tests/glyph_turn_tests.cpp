// stxlib::glyphs, a mask's surface and its turning
// (src/algorithms/glyphs/turn_lattice.hpp). The expected numbers in
// glyph_turn_expected.inc are not this code's: tools/turn_lattice_oracle.py
// works them out from stx_conv's own formula in exact fractions and from
// the rules at the head of turn_lattice.hpp, sharing nothing with it.
//
//   T1  the cell: forty cells against the oracle; what can be said by
//       hand (a level line, a straight slope, a step that stays between
//       its ends); a cell read backwards is the cell backwards, moved up
//       is moved up, turned over is turned over; the largest values it is
//       promised;
//   T2  the weights: against the oracle, and each row sums to one;
//   T3  the lattice: a mask of five by four whole against the oracle; a
//       pixel's control is the pixel; nothing round the edge; thirty-six
//       masks' lattices as one number;
//   T4  the turn: six turns of that mask, box and bytes, against the
//       oracle; thirty-six more as one number; no turn at a whole pixel is
//       the mask itself; a quarter turn is the mask turned pixel for
//       pixel; any box may be asked for and every byte of it is written;
//       what is and is not a turn; a box too large or too far is refused;
//   T5  masks of every size up to the largest, of noise, of full ink and
//       of a chequerboard, at turns and shifts of every kind: nothing
//       traps, and a pixel's control stays the pixel.

#include "algorithms/glyphs/turn_lattice.hpp"

#include "stxlib/glyph_face.hpp"

#include "test_support.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

namespace detail = stxlib::glyphs::detail;
namespace glyphs = stxlib::glyphs;
using stxlib::i16;
using stxlib::i32;
using stxlib::i64;
using stxlib::u32;
using stxlib::u8;
using stxlib::usize;

#include "glyph_turn_expected.inc"

// xorshift, as the oracle has it.
struct Random final {
    std::uint64_t state{0};
    [[nodiscard]] std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// A value's low sixteen bits into a running number, by exclusive or and
// shifts alone (nothing here wraps).
void mix(std::uint64_t& total, const i64 value) {
    total ^= static_cast<std::uint64_t>(value) & 0xFFFFU;
    total ^= total << 13;
    total ^= total >> 7;
    total ^= total << 17;
}

[[nodiscard]] std::vector<u8> random_mask(Random& random, const u32 width, const u32 height, const u32 kind) {
    std::vector<u8> out{};
    for (u32 index = 0; index < width * height; ++index) {
        const std::uint64_t value = random.next();
        if (kind == 0) {
            out.push_back(static_cast<u8>(value & 0xFFU));
        } else if (kind == 1) {
            out.push_back(static_cast<u8>((value & 3U) != 0 ? 255 : 0));
        } else {
            out.push_back(static_cast<u8>(((index % width) + (index / width)) % 2 == 0 ? 255 : 0));
        }
    }
    return out;
}

struct Lattice final {
    stxlib::Vector<i16> controls{};
    stxlib::Vector<i64> rows{};
    glyphs::Bounds upright{};
    bool made{false};
};

void build(Lattice& lattice, const std::vector<u8>& mask, const u32 width, const u32 height, const i32 left, const i32 top) {
    lattice.upright = glyphs::Bounds{left, top, width, height};
    lattice.made = detail::build_lattice(mask.data(), width, height, lattice.rows, lattice.controls);
}

struct Turned final {
    glyphs::Bounds bounds{};
    std::vector<u8> pixels{};
    bool boxed{false};
};

[[nodiscard]] Turned turn(const Lattice& lattice, const i32 cosine, const i32 sine, const glyphs::Point shift) {
    Turned out{};
    out.boxed = detail::turned_bounds(lattice.upright, cosine, sine, shift, glyphs::Limits{}, out.bounds);
    if (!out.boxed) return out;
    out.pixels.assign(static_cast<usize>(out.bounds.width) * out.bounds.height, 0x5A);
    const detail::TurnLattice view{lattice.controls.view().data(), lattice.upright};
    detail::turn_lattice(view, cosine, sine, shift, out.bounds, out.pixels.data());
    return out;
}

void cells() {
    bool all = true;
    bool mirrored = true;
    bool moved = true;
    bool over = true;
    for (unsigned index = 0; index < expected_cell_count; ++index) {
        const long long* const v = expected_cell_values + 6 * index;
        const long long* const want = expected_cell_controls + 5 * index;
        i64 made[5] = {0, 0, 0, 0, 0};
        detail::lattice_cell(v[0], v[1], v[2], v[3], v[4], v[5], made);
        for (usize k = 0; k < 5; ++k) {
            if (made[k] != want[k]) {
                std::fprintf(stderr, "  cell %u control %zu: %lld, wanted %lld\n", index, k, static_cast<long long>(made[k]), want[k]);
                all = false;
            }
        }
        // Read backwards: the same controls from the other end, the sixth
        // being the far value.
        i64 back[5] = {0, 0, 0, 0, 0};
        detail::lattice_cell(v[5], v[4], v[3], v[2], v[1], v[0], back);
        mirrored = mirrored && back[0] == detail::cell_unit * v[3];
        for (usize k = 1; k < 5; ++k) mirrored = mirrored && back[k] == made[5 - k];
        // Every value 1000 more; and every value its negative.
        i64 up[5] = {0, 0, 0, 0, 0};
        i64 down[5] = {0, 0, 0, 0, 0};
        detail::lattice_cell(v[0] + 1000, v[1] + 1000, v[2] + 1000, v[3] + 1000, v[4] + 1000, v[5] + 1000, up);
        detail::lattice_cell(-v[0], -v[1], -v[2], -v[3], -v[4], -v[5], down);
        for (usize k = 0; k < 5; ++k) {
            moved = moved && up[k] == made[k] + 1000 * detail::cell_unit;
            over = over && down[k] == -made[k];
        }
    }
    CHECK(all);
    CHECK(mirrored);
    CHECK(moved);
    CHECK(over);
    // By hand. A level line stays level; a straight slope of ten a value
    // is two a control; a step from nothing to full neither dips nor
    // passes full on the way.
    i64 level[5] = {0, 0, 0, 0, 0};
    i64 slope[5] = {0, 0, 0, 0, 0};
    i64 step[5] = {0, 0, 0, 0, 0};
    detail::lattice_cell(7, 7, 7, 7, 7, 7, level);
    detail::lattice_cell(0, 10, 20, 30, 40, 50, slope);
    detail::lattice_cell(0, 0, 0, 255, 255, 255, step);
    bool by_hand = true;
    for (usize k = 0; k < 5; ++k) {
        by_hand = by_hand && level[k] == 7 * detail::cell_unit && slope[k] == (20 + 2 * static_cast<i64>(k)) * detail::cell_unit;
        by_hand = by_hand && step[k] >= 0 && step[k] <= 255 * detail::cell_unit && (k == 0 || step[k] >= step[k - 1]);
    }
    CHECK(by_hand && step[0] == 0);
    // The largest values it is promised, in every order of sign: no step
    // leaves what a whole number of 64 bits holds (the sanitized builds
    // would say), and the first control is the cell's own value.
    const i64 far = static_cast<i64>(1) << 31;
    bool held = true;
    for (u32 signs = 0; signs < 64; ++signs) {
        i64 v[6] = {0, 0, 0, 0, 0, 0};
        for (u32 place = 0; place < 6; ++place) v[place] = ((signs >> place) & 1U) != 0 ? far : -far;
        i64 made[5] = {0, 0, 0, 0, 0};
        detail::lattice_cell(v[0], v[1], v[2], v[3], v[4], v[5], made);
        held = held && made[0] == detail::cell_unit * v[2];
    }
    CHECK(held);
}

void weights() {
    std::uint64_t sum = 0xCBF29CE484222325ULL;
    bool whole = true;
    for (usize place = 0; place < detail::weight_places; ++place) {
        i32 total = 0;
        for (usize k = 0; k < 6; ++k) {
            const i16 weight = detail::place_weights.of[place][k];
            mix(sum, weight);
            total = total + weight;
            whole = whole && weight >= 0;
        }
        whole = whole && total == 16384;
    }
    CHECK(whole);
    if (sum != expected_weights) std::fprintf(stderr, "  the weights: %016llx\n", static_cast<unsigned long long>(sum));
    CHECK(sum == expected_weights);
    CHECK(detail::place_weights.of[0][0] == 16384 && detail::place_weights.of[0][1] == 0 && detail::place_weights.of[0][5] == 0);
    bool even = true;
    for (usize k = 0; k < 6; ++k) even = even && detail::place_weights.of[128][k] == detail::place_weights.of[128][5 - k];
    CHECK(even && detail::place_weights.of[128][2] == 5120);
}

void lattices() {
    const std::vector<u8> mask(expected_mask, expected_mask + 20);
    Lattice lattice{};
    build(lattice, mask, 5, 4, -2, 9);
    CHECK(lattice.made && lattice.controls.count() == detail::lattice_controls(5, 4) && lattice.controls.count() == 31 * 26);
    bool same = lattice.made && lattice.controls.count() == 806;
    for (usize index = 0; same && index < 806; ++index) {
        if (lattice.controls[index] != expected_lattice[index]) {
            std::fprintf(stderr, "  control %zu: %d, wanted %d\n", index, lattice.controls[index], expected_lattice[index]);
            same = false;
        }
    }
    CHECK(same);
    // A pixel's control is the pixel, in 32nds; the lattice's edge is
    // nothing.
    bool pixels_kept = lattice.made;
    bool edge_clear = lattice.made;
    const usize stride = detail::lattice_stride(5);
    for (usize row = 0; pixels_kept && row < 4; ++row) {
        for (usize column = 0; column < 5; ++column) pixels_kept = pixels_kept && lattice.controls[5 * (row + 1) * stride + 5 * (column + 1)] == 32 * mask[row * 5 + column];
    }
    for (usize x = 0; edge_clear && x < stride; ++x) edge_clear = lattice.controls[x] == 0 && lattice.controls[25 * stride + x] == 0;
    for (usize y = 0; edge_clear && y < 26; ++y) edge_clear = lattice.controls[y * stride] == 0 && lattice.controls[y * stride + stride - 1] == 0;
    CHECK(pixels_kept);
    CHECK(edge_clear);
}

// The oracle's thirty-six masks: their lattices and their turns.
void many() {
    Random random{0x0123456789ABCDEFULL};
    std::uint64_t lattice_sum = 0xCBF29CE484222325ULL;
    std::uint64_t turned_sum = 0xCBF29CE484222325ULL;
    bool made = true;
    for (unsigned number = 0; number < expected_many_count; ++number) {
        const u32 width = 1 + static_cast<u32>(random.next() % 9);
        const u32 height = 1 + static_cast<u32>(random.next() % 9);
        const std::vector<u8> mask = random_mask(random, width, height, number % 3);
        const i64 degrees = static_cast<i64>(random.next() % (static_cast<std::uint64_t>(360) << 16));
        const i32 shift_x = static_cast<i32>(random.next() % 64);
        const i32 shift_y = -static_cast<i32>(random.next() % 64);
        const i32 left = static_cast<i32>(random.next() % 21) - 10;
        const i32 top = static_cast<i32>(random.next() % 21) - 10;
        Lattice lattice{};
        build(lattice, mask, width, height, left, top);
        made = made && lattice.made;
        if (!lattice.made) continue;
        for (usize index = 0; index < lattice.controls.count(); ++index) mix(lattice_sum, lattice.controls[index]);
        const glyphs::TextMatrix matrix = glyphs::turn_matrix(degrees);
        const Turned turned = turn(lattice, matrix.xx, matrix.yx, glyphs::Point{shift_x, shift_y});
        made = made && turned.boxed;
        mix(turned_sum, turned.bounds.left);
        mix(turned_sum, turned.bounds.top);
        mix(turned_sum, turned.bounds.width);
        mix(turned_sum, turned.bounds.height);
        for (const u8 pixel : turned.pixels) mix(turned_sum, pixel);
    }
    CHECK(made);
    if (lattice_sum != expected_many_lattices || turned_sum != expected_many_turned) {
        std::fprintf(stderr, "  many: lattices %016llx, turned %016llx\n", static_cast<unsigned long long>(lattice_sum), static_cast<unsigned long long>(turned_sum));
    }
    CHECK(lattice_sum == expected_many_lattices);
    CHECK(turned_sum == expected_many_turned);
}

void turns() {
    const std::vector<u8> mask(expected_mask, expected_mask + 20);
    Lattice lattice{};
    build(lattice, mask, 5, 4, -2, 9);
    CHECK(lattice.made);
    if (!lattice.made) return;
    bool all = true;
    for (unsigned index = 0; index < expected_turn_count; ++index) {
        const long long* const want = expected_turns + 10 * index;
        const glyphs::TextMatrix matrix = glyphs::turn_matrix(want[0]);
        const bool angle = matrix.xx == want[1] && matrix.yx == want[2] && matrix.yy == matrix.xx && matrix.xy == -matrix.yx;
        const Turned turned = turn(lattice, matrix.xx, matrix.yx, glyphs::Point{static_cast<i32>(want[3]), static_cast<i32>(want[4])});
        bool same = angle && turned.boxed && turned.bounds.left == want[5] && turned.bounds.top == want[6] && turned.bounds.width == want[7] &&
                    turned.bounds.height == want[8];
        for (usize pixel = 0; same && pixel < turned.pixels.size(); ++pixel) same = turned.pixels[pixel] == expected_turned[static_cast<usize>(want[9]) + pixel];
        if (!same) {
            std::fprintf(stderr, "  turn %u: matrix %d %d, box %d %d %u %u\n", index, matrix.xx, matrix.yx, turned.bounds.left, turned.bounds.top, turned.bounds.width,
                         turned.bounds.height);
        }
        all = all && same;
    }
    CHECK(all);

    // No turn, at a whole pixel: the mask itself, box and bytes.
    const glyphs::Point whole_shifts[] = {{0, 0}, {3 * 64, -2 * 64}, {-640, 64}};
    bool itself = true;
    for (const glyphs::Point shift : whole_shifts) {
        const Turned still = turn(lattice, 0x10000, 0, shift);
        itself = itself && still.boxed && still.bounds.left == -2 + shift.x / 64 && still.bounds.top == 9 + shift.y / 64 && still.bounds.width == 5 &&
                 still.bounds.height == 4 && still.pixels == mask;
    }
    CHECK(itself);
    // A quarter turn, at a whole pixel: (x, y) goes to (-y, x), so the
    // mask's rows are columns from the left and its columns rows from the
    // bottom. A half turn: the mask read from its end.
    const Turned quarter = turn(lattice, 0, 0x10000, glyphs::Point{0, 0});
    bool quartered = quarter.boxed && quarter.bounds.left == -9 && quarter.bounds.top == 3 && quarter.bounds.width == 4 && quarter.bounds.height == 5;
    for (usize row = 0; quartered && row < 4; ++row) {
        for (usize column = 0; column < 5; ++column) quartered = quartered && quarter.pixels[(4 - column) * 4 + row] == mask[row * 5 + column];
    }
    CHECK(quartered);
    const Turned half = turn(lattice, -0x10000, 0, glyphs::Point{64, 0});
    bool halved = half.boxed && half.bounds.left == -3 + 1 && half.bounds.top == -5 && half.bounds.width == 5 && half.bounds.height == 4;
    for (usize index = 0; halved && index < 20; ++index) halved = half.pixels[19 - index] == mask[index];
    CHECK(halved);

    // Any box may be asked for: what of it is off the lattice is nothing,
    // the rest is as in the turn's own box, and every byte is written
    // whatever was there.
    const glyphs::TextMatrix slant = glyphs::turn_matrix(static_cast<i64>(17) << 16);
    const glyphs::Point shift{21, -40};
    const Turned own = turn(lattice, slant.xx, slant.yx, shift);
    const glyphs::Bounds wide{own.bounds.left - 3, own.bounds.top + 2, own.bounds.width + 7, own.bounds.height + 5};
    std::vector<u8> first(static_cast<usize>(wide.width) * wide.height, 0x11);
    std::vector<u8> second(first.size(), 0xEE);
    const detail::TurnLattice view{lattice.controls.view().data(), lattice.upright};
    detail::turn_lattice(view, slant.xx, slant.yx, shift, wide, first.data());
    detail::turn_lattice(view, slant.xx, slant.yx, shift, wide, second.data());
    bool placed = own.boxed && first == second;
    for (usize row = 0; placed && row < wide.height; ++row) {
        for (usize column = 0; column < wide.width; ++column) {
            const i64 x = static_cast<i64>(column) - 3;
            const i64 y = static_cast<i64>(row) - 2;
            const bool inside = x >= 0 && y >= 0 && x < own.bounds.width && y < own.bounds.height;
            const u8 want = inside ? own.pixels[static_cast<usize>(y) * own.bounds.width + static_cast<usize>(x)] : 0;
            placed = placed && first[row * wide.width + column] == want;
        }
    }
    CHECK(placed);
    // A box that misses the lattice altogether is nothing.
    const glyphs::Bounds away{500, 500, 6, 4};
    std::vector<u8> nothing(24, 0x77);
    detail::turn_lattice(view, slant.xx, slant.yx, shift, away, nothing.data());
    bool clear = true;
    for (const u8 pixel : nothing) clear = clear && pixel == 0;
    CHECK(clear);

    // What is a turn: the matrix of every angle, and nothing mirrored,
    // stretched or slanted.
    bool turns_are = true;
    for (i64 degrees = -(static_cast<i64>(360) << 16); degrees < (static_cast<i64>(720) << 16); degrees = degrees + 30011) {
        const glyphs::TextMatrix matrix = glyphs::turn_matrix(degrees);
        turns_are = turns_are && detail::is_turn(matrix.xx, matrix.xy, matrix.yx, matrix.yy);
    }
    CHECK(turns_are);
    CHECK(detail::is_turn(0x10000, 0, 0, 0x10000) && detail::is_turn(0, -0x10000, 0x10000, 0));
    CHECK(!detail::is_turn(0x10000, 0, 0, -0x10000) && !detail::is_turn(0x20000, 0, 0, 0x20000) && !detail::is_turn(0x10000, 0x4000, 0, 0x10000));
    CHECK(!detail::is_turn(0x8000, 0, 0, 0x8000) && !detail::is_turn(slant.xx, slant.yx, slant.yx, slant.yy) && !detail::is_turn(0, 0, 0, 0));
    CHECK(!detail::is_turn(-2147483647 - 1, 0, 0, -2147483647 - 1));

    // A box larger than the limits allow, or further out than a mask can
    // be, is refused.
    glyphs::Limits small{};
    small.width = 6;
    glyphs::Bounds box{};
    CHECK(!detail::turned_bounds(lattice.upright, slant.xx, slant.yx, shift, small, box) && box.width == 0);
    small.width = 7;
    CHECK(detail::turned_bounds(lattice.upright, slant.xx, slant.yx, shift, small, box) && box.width == 7);
    const glyphs::Bounds far_out{0x7FFFFF00, 0x7FFFFF00, 5, 4};
    CHECK(!detail::turned_bounds(far_out, slant.xx, slant.yx, shift, glyphs::Limits{}, box));
    // The largest shift there is, 2^25 pixels, is not too far: the box is
    // the turn's own, moved.
    CHECK(detail::turned_bounds(lattice.upright, slant.xx, slant.yx, glyphs::Point{2147483647, -2147483647}, glyphs::Limits{}, box) && box.width == 7 &&
          box.left > 33554000 && box.top < -33554000);
}

void every_kind() {
    Random random{0xA5A5A5A55A5A5A5AULL};
    bool built = true;
    bool pixels_kept = true;
    bool written = true;
    usize masks = 0;
    const u32 sides[][2] = {{1, 1},   {1, 2},   {2, 1},   {3, 3},  {7, 5},  {13, 17}, {40, 9},  {9, 40},
                            {31, 33}, {64, 64}, {256, 1}, {1, 256}, {100, 3}, {256, 256}};
    for (const u32* const side : sides) {
        for (u32 kind = 0; kind < 4; ++kind) {
            const u32 width = side[0];
            const u32 height = side[1];
            if (width * height > 10000 && kind > 1) continue;
            std::vector<u8> mask = random_mask(random, width, height, kind < 3 ? kind : 0);
            if (kind == 3) mask.assign(mask.size(), 255);
            Lattice lattice{};
            const i32 left = static_cast<i32>(random.next() % 4001) - 2000;
            const i32 top = static_cast<i32>(random.next() % 4001) - 2000;
            build(lattice, mask, width, height, left, top);
            built = built && lattice.made && lattice.controls.count() == detail::lattice_controls(width, height);
            if (!lattice.made) continue;
            masks = masks + 1;
            const usize stride = detail::lattice_stride(width);
            for (usize row = 0; row < height; ++row) {
                for (usize column = 0; column < width; ++column) {
                    pixels_kept = pixels_kept && lattice.controls[5 * (row + 1) * stride + 5 * (column + 1)] == 32 * mask[row * width + column];
                }
            }
            const u32 turn_count = width * height > 10000 ? 2 : 6;
            for (u32 number = 0; number < turn_count; ++number) {
                const i64 degrees = static_cast<i64>(random.next() % (static_cast<std::uint64_t>(720) << 16)) - (static_cast<i64>(360) << 16);
                const i32 reach = number % 3 == 0 ? 64 : (number % 3 == 1 ? 6400 : 64000000);
                const glyphs::Point shift{static_cast<i32>(random.next() % static_cast<std::uint64_t>(2 * reach + 1)) - reach,
                                          static_cast<i32>(random.next() % static_cast<std::uint64_t>(2 * reach + 1)) - reach};
                const glyphs::TextMatrix matrix = glyphs::turn_matrix(number == 5 ? static_cast<i64>(90 * (number + kind)) << 16 : degrees);
                glyphs::Bounds bounds{};
                if (!detail::turned_bounds(lattice.upright, matrix.xx, matrix.yx, shift, glyphs::Limits{}, bounds)) {
                    built = false;
                    continue;
                }
                const usize bytes = static_cast<usize>(bounds.width) * bounds.height;
                std::vector<u8> first(bytes, 0x11);
                std::vector<u8> second(bytes, 0xEE);
                const detail::TurnLattice view{lattice.controls.view().data(), lattice.upright};
                detail::turn_lattice(view, matrix.xx, matrix.yx, shift, bounds, first.data());
                detail::turn_lattice(view, matrix.xx, matrix.yx, shift, bounds, second.data());
                written = written && first == second;
                // The turned box holds the mask's width and height turned,
                // and no more than a pixel or two beside.
                const u32 reach_most = width + height + 3;
                written = written && bounds.width <= reach_most && bounds.height <= reach_most;
            }
        }
    }
    std::printf("masks of every kind: %zu\n", masks);
    CHECK(built);
    CHECK(pixels_kept);
    CHECK(written);
    CHECK(masks >= 40);
}

} // namespace

int main() {
    cells();
    weights();
    lattices();
    many();
    turns();
    every_kind();
    const int status = stxlib_tests::finish("stxlib_glyph_turn_tests");
    return status;
}
