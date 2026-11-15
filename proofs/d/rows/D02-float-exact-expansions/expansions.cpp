// Row D02 (phase D, targeted), bound-effect configuration: the control flow
// of float_exact's expansion arithmetic (src/core/float_exact.hpp:
// grow_expansion, expansion_sum, scale_expansion, expansion_product,
// expansion_sign) in double, with every error-free transformation
// abstracted to its effect.
// P0: no read or write outside the stated storage: each input is read only
//     in [0, len), and each output written only within the count its
//     comment states (grow: elen + 1; sum: elen + flen, at least one;
//     scale: 2 * elen; product: 2 * elen * flen in h and 4 * elen * flen in
//     scratch). Every buffer is placed at the end of its storage, so one
//     element past the stated count is outside the object and the tool's
//     bounds check fails on it.
// P1: the returned counts: grow returns elen + 1; sum returns 1 to
//     max(1, elen + flen); scale and product return 1 to their stated
//     maximum; the sign is -1, 0 or +1.
// Abstraction (bound effect, common/effect.h's direction): each two_sum,
//     fast_two_sum and product step returns any pair of doubles, NaN and
//     infinities included. No bound is stated and none is needed: the
//     counts and indices depend only on which errors are zero, and every
//     combination is allowed. The values (exactness, nonoverlap) are D01's
//     and Shewchuk's (1997) theorems, not this run's.
// Bound: lengths at most N (N in {2, 3}).
#include "verify.h"

#include "stxlib/types.hpp"

#include <bit>
#include <cstdint>

//@ pin source=stxlib path=include/stxlib/types.hpp sha256=e8c563f2e84124ab284680198c844b971d8f0e48a2c0bec2f9ee8cf400b67b7f
//@ lift source=stxlib path=src/core/float_exact.hpp lines=96-116 sha256=fc27ef5e6487e677ff06a3ebd162b9d685d6a54506b1f6e6b0458d4c26c49da7
//@ lift source=stxlib path=src/core/float_exact.hpp lines=121-238 namespace=stxlib::float_exact sha256=34db4ddd7d8dbe16480845b870b984f00c68f7a3a33268116e88b207eebea1f9

//@ model-begin reason="the effect of an error-free transformation: any pair of doubles (no bound; the checked counts and indices do not depend on the values)"
static stxlib::float_exact::Pair<double> stx_any_pair() {
    const stxlib::float_exact::Pair<double> pair{nondet_double(), nondet_double()};
    return pair;
}
//@ model-end

//@ lift source=stxlib path=src/core/float_exact.hpp lines=401-480 namespace=stxlib::float_exact sha256=8cdfab84ca4cfb33e7bb3fdd1b165bd36586a9e9fb02546cac3667e01b83ae56
//@ lift source=stxlib path=src/core/float_exact.hpp lines=497-545 namespace=stxlib::float_exact sha256=be1c3018c2ef54aedf0faae2cf43f7417dbf41638c44523a0726ceacfd94180c
//@ abstract source=stxlib path=src/core/float_exact.hpp line=406 from="component_two_sum(q, e[index])" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=439 from="component_fast_two_sum(e_now, q)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of fast_two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=443 from="component_fast_two_sum(f_now, q)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of fast_two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=451 from="component_two_sum(q, e_now)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=455 from="component_two_sum(q, f_now)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=464 from="component_two_sum(q, e_now)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=471 from="component_two_sum(q, f_now)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=501 from="expansion_product_step(e[0], b)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of the product step"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=505 from="expansion_product_step(e[index], b)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of the product step"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=506 from="component_two_sum(q, product.error)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of two_sum"
//@ abstract source=stxlib path=src/core/float_exact.hpp line=508 from="component_fast_two_sum(product.value, sum.value)" to="stx_any_pair()" bound="any pair (none needed)" reason="effect of fast_two_sum"
//@ mutant id=m1 source=stxlib path=src/core/float_exact.hpp line=522 from="scratch + 2 * elen * flen" to="scratch + 2 * elen * flen + 1"

#ifndef STX_N
#define STX_N 3
#endif

using stxlib::usize;
namespace fe = stxlib::float_exact;

namespace {

constexpr usize bound = STX_N;

void fill(double* const values, const usize count) {
    for (usize index = 0; index < count; ++index) values[index] = nondet_double();
}

} // namespace

int main() {
    double e_storage[bound];
    double f_storage[bound];
    fill(e_storage, bound);
    fill(f_storage, bound);
    const usize elen = nondet_size();
    const usize flen = nondet_size();
    STX_ASSUME(elen <= bound && flen <= bound);
    // Each input occupies the end of its storage: a read at [len] is outside.
    const double* const e = e_storage + (bound - elen);
    const double* const f = f_storage + (bound - flen);

    double grown_storage[bound + 1];
    const usize grown = fe::grow_expansion(e, elen, nondet_double(), grown_storage + (bound + 1) - (elen + 1));
    STX_ASSERT(grown == elen + 1, "grow_expansion writes elen + 1 components");

    double summed_storage[2 * bound + 1];
    const usize room = elen + flen > 0 ? elen + flen : 1;
    const usize summed = fe::expansion_sum(e, elen, f, flen, summed_storage + (2 * bound + 1) - room);
    STX_ASSERT(summed >= 1 && summed <= room, "expansion_sum writes 1 to max(1, elen + flen) components");

    STX_ASSUME(elen >= 1 && flen >= 1);
    double scaled_storage[2 * bound];
    const usize scaled = fe::scale_expansion(e, elen, nondet_double(), scaled_storage + 2 * bound - 2 * elen);
    STX_ASSERT(scaled >= 1 && scaled <= 2 * elen, "scale_expansion writes 1 to 2 * elen components");

    double product_storage[2 * bound * bound];
    double scratch_storage[4 * bound * bound];
    const usize product = fe::expansion_product(e, elen, f, flen,
                                                product_storage + 2 * bound * bound - 2 * elen * flen,
                                                scratch_storage + 4 * bound * bound - 4 * elen * flen);
    STX_ASSERT(product >= 1 && product <= 2 * elen * flen, "expansion_product writes 1 to 2 * elen * flen");

    const int sign = fe::expansion_sign(e, elen);
    STX_ASSERT(sign >= -1 && sign <= 1, "the sign is -1, 0 or +1");
    STX_VACUITY_PROBE();
    return 0;
}
