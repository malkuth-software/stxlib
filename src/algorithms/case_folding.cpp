// Simple Unicode case folding (include/stxlib/case_folding.hpp; M2 C7) over
// the generated table (src/algorithms/case_folding_table.inc, from
// third_party/unicode-18.0.0/CaseFolding.txt by tools/gen_case_folding.py)
// and the header-only core (case_folding_core.hpp).

#include "stxlib/case_folding.hpp"

#include "algorithms/case_folding_core.hpp"

namespace stxlib::text {

namespace core = ::stxlib::case_folding_core;

u32 fold_case_simple(const u32 code_point) noexcept {
    // Not a scalar value: unchanged (no table entry is a surrogate or lies
    // above 0x10FFFF, so the lookup would agree; stated for the reader).
    if (code_point > core::highest_scalar) return code_point;
    if (code_point >= core::surrogate_low && code_point <= core::surrogate_high) return code_point;
    const u32 result = core::fold(code_point);
    return result;
}

usize folded_bytes(const TextView text) noexcept {
    const usize result = core::folded_bytes(text.data(), text.count());
    return result;
}

Formatted fold_case(const MutableView<char> output, const TextView text) noexcept {
    const usize needed = core::folded_bytes(text.data(), text.count());
    if (needed > output.count()) {
        const Formatted result = Formatted::too_small(needed);
        return result;
    }
    core::fold_into(text.data(), text.count(), output.data());
    // Each scalar was encoded whole, so the bytes are well-formed UTF-8.
    const TextView folded = ::stxlib::detail::TextViewAccess::adopt_validated(output.data(), needed);
    const Formatted result = Formatted::written(folded);
    return result;
}

TextOrder compare_folded(const TextView left, const TextView right) noexcept {
    const int order = core::compare(left.data(), left.count(), right.data(), right.count());
    if (order < 0) return TextOrder::before;
    if (order > 0) return TextOrder::after;
    return TextOrder::same;
}

bool equal_folded(const TextView left, const TextView right) noexcept {
    const bool result = compare_folded(left, right) == TextOrder::same;
    return result;
}

} // namespace stxlib::text
