// Row 21, numeric obligations of the bound-effect configuration
// (effect.cpp), established by exhaustive enumeration: every one of the
// 2^32 float bit patterns goes through math::sincos(float), lifted
// verbatim, and each abstracted result is recorded where effect.cpp
// abstracts it (the `//@ subst` lines below only wrap an expression in
// stx_record, which returns it unchanged). The program prints each site's
// range against the bound effect.cpp states, checks the table's 256
// entries are within [-1, 1], and exits 0 only when every bound holds.
// Compiled natively on the M4 by tools/enumerate.sh, without contraction
// (the library's own flags: -ffp-contract=off -fno-fast-math -fno-math-errno).
// Not a proof harness: the runner does not run it.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "stx_auxlib/math.hpp"

namespace {
struct Site {
    const char* name;
    double low;
    double high;
    double seen_low;
    double seen_high;
    unsigned long long count;
    bool nan;
};
Site sites[] = {
    {"x on the reduced paths (sincos.cpp:22)", -0x1p32, 0x1p32, 0, 0, 0, false},
    {"shifted (sincos_core.hpp:83)", 0x1.8p52 - 0x1p38, 0x1.8p52 + 0x1p38, 0, 0, 0, false},
    {"q (sincos_core.hpp:86)", -0x1p38, 0x1p38, 0, 0, 0, false},
    {"r, fast path (sincos.cpp:24)", -0x1p-4, 0x1p-4, 0, 0, 0, false},
    {"r, medium path (sincos.cpp:29)", -0x1p-4, 0x1p-4, 0, 0, 0, false},
    {"r2 (sincos.cpp:55)", 0.0, 0x1p-8, 0, 0, 0, false},
    {"sin_r (sincos.cpp:56)", -0x1.1p-4, 0x1.1p-4, 0, 0, 0, false},
    {"cos_r_minus_1 (sincos.cpp:58)", -0x1p-8, 0x1p-16, 0, 0, 0, false},
    {"sin_correction (sincos.cpp:60)", -0x1p-3, 0x1p-3, 0, 0, 0, false},
    {"cos_correction (sincos.cpp:61)", -0x1p-3, 0x1p-3, 0, 0, 0, false},
    {"sin before rounding (sincos.cpp:62)", -1.125, 1.125, 0, 0, 0, false},
    {"cos before rounding (sincos.cpp:63)", -1.125, 1.125, 0, 0, 0, false},
};
} // namespace

static double stx_record(const int id, const double value) {
    Site& site = sites[id];
    if (value != value) {
        site.nan = true;
    } else if (site.count == 0) {
        site.seen_low = value;
        site.seen_high = value;
    } else {
        if (value < site.seen_low) site.seen_low = value;
        if (value > site.seen_high) site.seen_high = value;
    }
    site.count += 1;
    return value;
}

//@ lift source=stx_auxlib path=src/math/sincos_core.hpp lines=all sha256=bc7aa59bb172309901260fdb44d0d986b13eb2b8403563cde59a7ee43deefca1
//@ subst source=stx_auxlib path=src/math/sincos_core.hpp line=83 from="x * kernel_constants.sincos_inverse_step + kernel_constants.rounding_shift" to="stx_record(1, x * kernel_constants.sincos_inverse_step + kernel_constants.rounding_shift)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos_core.hpp line=86 from="shifted - kernel_constants.rounding_shift" to="stx_record(2, shifted - kernel_constants.rounding_shift)" reason="records the value effect.cpp abstracts"
//@ lift source=stx_auxlib path=src/math/sincos.cpp lines=all sha256=ea2c4abe051a70c2e45651e1030a7e4ec6da12ecf1f83dfe9cbaaa10228720e2
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=6 from="#include \"sincos_core.hpp\"" to="// sincos_core.hpp is lifted above" reason="the header is lifted so its lines can be instrumented"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=22 from="sincos_step(x, index)" to="sincos_step(stx_record(0, x), index)" reason="records the precondition effect.cpp asserts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=24 from="sincos_remainder(x, q)" to="stx_record(3, sincos_remainder(x, q))" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=29 from="sincos_remainder_medium(x, q, r, r_lo);" to="sincos_remainder_medium(x, q, r, r_lo); (void)stx_record(4, r);" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=55 from="r * r" to="stx_record(5, r * r)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=56 from="r + r * (r2 * (detail::sin_3 + r2 * detail::sin_5))" to="stx_record(6, r + r * (r2 * (detail::sin_3 + r2 * detail::sin_5)))" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=58 from="(r2 * r2) * (detail::cos_4 + r2 * detail::cos_6) - r2 * detail::cos_half" to="stx_record(7, (r2 * r2) * (detail::cos_4 + r2 * detail::cos_6) - r2 * detail::cos_half)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=60 from="entry.s * cos_r_minus_1 + entry.c * sin_r" to="stx_record(8, entry.s * cos_r_minus_1 + entry.c * sin_r)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=61 from="entry.c * cos_r_minus_1 - entry.s * sin_r" to="stx_record(9, entry.c * cos_r_minus_1 - entry.s * sin_r)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=62 from="entry.s + sin_correction" to="stx_record(10, entry.s + sin_correction)" reason="records the value effect.cpp abstracts"
//@ subst source=stx_auxlib path=src/math/sincos.cpp line=63 from="entry.c + cos_correction" to="stx_record(11, entry.c + cos_correction)" reason="records the value effect.cpp abstracts"
//@ lift source=stx_auxlib path=src/math/constants.cpp lines=all sha256=d4efbc75fdebadf14484a33bc434641b7b95bf5fcf17947348b9f836293cd038
//@ subst source=stx_auxlib path=src/math/constants.cpp line=9 from="#include \"sincos_core.hpp\"" to="// sincos_core.hpp is lifted above" reason="the header is lifted so its lines can be instrumented"

int main() {
    for (std::uint64_t bits = 0; bits <= 0xffffffffULL; ++bits) {
        const float angle = std::bit_cast<float>(static_cast<std::uint32_t>(bits));
        float sine = 0.0f;
        float cosine = 0.0f;
        stx_auxlib::math::native::sincos(angle, sine, cosine);
    }
    bool held = true;
    for (const Site& site : sites) {
        const bool within = !site.nan && (site.count == 0 || (site.seen_low >= site.low && site.seen_high <= site.high));
        held = held && within;
        std::printf("%s %s: %llu values, seen [%a, %a], bound [%a, %a]%s\n", within ? "PASS" : "FAIL", site.name,
                    site.count, site.seen_low, site.seen_high, site.low, site.high, site.nan ? ", NaN seen" : "");
    }
    bool table = true;
    for (const auto& entry : stx_auxlib::math::detail::sincos_table_256) {
        table = table && std::fabs(entry.s) <= 1.0 && std::fabs(entry.c) <= 1.0;
    }
    std::printf("%s the table's 256 entries are within [-1, 1]\n", table ? "PASS" : "FAIL");
    return held && table ? 0 : 1;
}
