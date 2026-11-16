// Simple Unicode case folding (include/stxlib/case_folding.hpp; M2 C7) over
// the generated table (src/algorithms/case_folding_table.inc, from
// third_party/unicode-18.0.0/CaseFolding.txt by tools/gen_case_folding.py)
// and the header-only core (case_folding_core.hpp); and the full folding of
// one code point, over the table of F mappings generated beside it
// (case_folding_full_table.inc).

#include "stxlib/case_folding.hpp"

#include "algorithms/case_folding_core.hpp"

namespace stxlib::text {

namespace core = ::stxlib::case_folding_core;

namespace {

// The F mappings (tools/gen_case_folding.py, from the same file).
#include "algorithms/case_folding_full_table.inc"

} // namespace

u32 fold_case_simple(const u32 code_point) noexcept {
    // Not a scalar value: unchanged (no table entry is a surrogate or lies
    // above 0x10FFFF, so the lookup would agree; stated for the reader).
    if (code_point > core::highest_scalar) return code_point;
    if (code_point >= core::surrogate_low && code_point <= core::surrogate_high) return code_point;
    const u32 result = core::fold(code_point);
    return result;
}

FullFold fold_case_full(const u32 code_point) noexcept {
    FullFold result{};
    // Below the first source and above the last there is no F mapping; the
    // search halves [low, high) each step (at most 7 steps for 105 entries).
    if (code_point >= case_folding_full_sources[0] && code_point <= case_folding_full_sources[case_folding_full_count - 1]) {
        usize low = 0;
        usize high = case_folding_full_count;
        while (low < high) {
            const usize middle = low + (high - low) / 2;
            if (case_folding_full_sources[middle] < code_point) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low < case_folding_full_count && case_folding_full_sources[low] == code_point) {
            result.count = case_folding_full_lengths[low];
            for (u32 index = 0; index < result.count; ++index) result.scalars[index] = case_folding_full_targets[low][index];
            return result;
        }
    }
    result.count = 1;
    result.scalars[0] = fold_case_simple(code_point);
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
