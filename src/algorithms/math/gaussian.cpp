// Standard normal probability between two endpoints. This kernel uses only
// scalar IEEE operations with a fixed evaluation order on every target.
#include "stxlib/math.hpp"
#include "core/subnormal.hpp"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace stxlib::math {
namespace {

// Q(c) and the seven Taylor coefficients of Q(c+t)-Q(c) at c=i/32.
struct GaussianCell final {
    double tail;
    double c1, c2, c3, c4, c5, c6, c7;
};
alignas(64) constexpr GaussianCell gaussian_cells[] = {
#include "gaussian_table.inc"
};
static_assert(sizeof(GaussianCell) == 64);
static_assert(sizeof(gaussian_cells) / sizeof(GaussianCell) == 39 * 32 + 1);

// Past eight sigma the same absolute error accounts for many more bits of
// an interval mass. These terms extend that tail's local polynomial to 15.
using TailTerms = double[8];
alignas(64) constexpr TailTerms gaussian_tail_terms[] = {
#include "gaussian_tail_terms.inc"
};
static_assert(sizeof(gaussian_tail_terms) / sizeof(TailTerms) == (39 - 8) * 32 + 1);

double table_tail(const double x) noexcept {
    const int index = static_cast<int>(32.0 * x + 0.5);
    const double t = x - index * 0x1p-5;
    const auto& row = gaussian_cells[index];
    const double t2 = t * t;
    const double low = row.c1 + t * row.c2;
    const double middle = row.c3 + t * row.c4;
    const double high = row.c5 + t * row.c6 + t2 * row.c7;
    return row.tail + t * (low + t2 * (middle + t2 * high));
}

double positive_tail(const double x) noexcept {
    return x >= 39.0 ? 0.0 : table_tail(x);
}

// Both endpoints are in the table. On a wide interval containing a
// subnormal endpoint its table value is exactly 1/2, as for signed zero.
double wide_mass(const double a, const double b) noexcept {
    const double left = table_tail(std::fabs(a));
    const double right = table_tail(std::fabs(b));
    if (a >= 0.0) return left - right;
    if (b <= 0.0) return right - left;
    return (0.5 - left) + (0.5 - right);
}

double extended_tail(const double x) noexcept {
    if (x >= 39.0) return 0.0;
    const int index = static_cast<int>(32.0 * x + 0.5);
    const double t = x - index * 0x1p-5;
    const auto& terms = gaussian_tail_terms[index - 8 * 32];
    double p = terms[7];
    for (int j = 6; j >= 0; --j) p = terms[j] + t * p;
    const double t2 = t * t;
    const double t4 = t2 * t2;
    return table_tail(x) + (t4 * t4) * p;
}

// Integrate a single local polynomial over [a,b]. Its divided difference
// factors out the exact width for nearby same-sign doubles. Choosing the
// node nearest the midpoint keeps both offsets within 1/32 (plus roundoff).
template<bool extended>
double narrow_mass(const double a, const double b, const double width) noexcept {
    const double midpoint = a + 0.5 * width;
    const int index = static_cast<int>(32.0 * std::fabs(midpoint) + 0.5);
    const double centre = index * 0x1p-5;
    const double u = (midpoint < 0.0 ? -b : a) - centre;
    const double v = (midpoint < 0.0 ? -a : b) - centre;
    const auto& row = gaussian_cells[index];
    double p = row.c7;
    double divided = p;
    if constexpr (extended) {
        const auto& terms = gaussian_tail_terms[index - 8 * 32];
        p = terms[7];
        divided = p;
        for (int j = 6; j >= 0; --j) {
            p = terms[j] + u * p;
            divided = p + v * divided;
        }
        p = row.c7 + u * p;
        divided = p + v * divided;
    }
    for (const double coefficient : {row.c6, row.c5, row.c4, row.c3, row.c2, row.c1}) {
        p = coefficient + u * p;
        divided = p + v * divided;
    }
    return -width * divided;
}

double interval(const double a, const double b) noexcept {
    if (!(a < b)) return a == b ? 0.0 : std::numeric_limits<double>::quiet_NaN();
    if (a >= 38.0 || b <= -38.0) return 0.0;
    if (a >= 8.0 || b <= -8.0) {
        const double lower = a >= 8.0 ? a : -b;
        const double upper = a >= 8.0 ? b : -a;
        if (upper < lower + 0x1p-5) return narrow_mass<true>(lower, upper, upper - lower);
        return extended_tail(lower) - extended_tail(upper);
    }
    if (b < a + 0x1p-5) return narrow_mass<false>(a, b, b - a);
    if (a >= 0.0) return positive_tail(a) - positive_tail(b);
    if (b <= 0.0) return positive_tail(-b) - positive_tail(-a);
    return (0.5 - positive_tail(-a)) + (0.5 - positive_tail(b));
}

} // namespace

double gaussian_interval(const double a, const double b) noexcept {
    // The output on this central path is always normal. This also
    // removes the per-endpoint tail clipping checks from the scanline loop.
    if (b >= a + 0x1p-5 && a > -32.0 && b < 32.0 && a < 8.0 && b > -8.0) {
        return wide_mass(a, b);
    }
    using ::stxlib::detail::flush_subnormal;
    return flush_subnormal(interval(flush_subnormal(a), flush_subnormal(b)));
}

float gaussian_interval(const float a, const float b) noexcept {
    using ::stxlib::detail::flush_subnormal;
    return flush_subnormal(static_cast<float>(interval(flush_subnormal(a), flush_subnormal(b))));
}

} // namespace stxlib::math
