// stxlib::text: general category and canonical composition
// (include/stxlib/unicode_properties.hpp), read from the shaper's tables.

#include "stxlib/unicode_properties.hpp"

#include "algorithms/glyphs/shape_unicode.hpp"

namespace stxlib::text {

namespace {

namespace unicode = glyphs::detail;

// The shaper's category numbers (harfrust's, HarfBuzz's) to Unicode's order.
inline constexpr GeneralCategory categories[30] = {
    GeneralCategory::control,               // category_control
    GeneralCategory::format,                // category_format
    GeneralCategory::unassigned,            // category_unassigned
    GeneralCategory::private_use,           // category_private_use
    GeneralCategory::surrogate,             // category_surrogate
    GeneralCategory::lowercase_letter,      // category_lowercase_letter
    GeneralCategory::modifier_letter,       // category_modifier_letter
    GeneralCategory::other_letter,          // category_other_letter
    GeneralCategory::titlecase_letter,      // category_titlecase_letter
    GeneralCategory::uppercase_letter,      // category_uppercase_letter
    GeneralCategory::spacing_mark,          // category_spacing_mark
    GeneralCategory::enclosing_mark,        // category_enclosing_mark
    GeneralCategory::nonspacing_mark,       // category_non_spacing_mark
    GeneralCategory::decimal_number,        // category_decimal_number
    GeneralCategory::letter_number,         // category_letter_number
    GeneralCategory::other_number,          // category_other_number
    GeneralCategory::connector_punctuation, // category_connector_punctuation
    GeneralCategory::dash_punctuation,      // category_dash_punctuation
    GeneralCategory::close_punctuation,     // category_close_punctuation
    GeneralCategory::final_punctuation,     // category_final_punctuation
    GeneralCategory::initial_punctuation,   // category_initial_punctuation
    GeneralCategory::other_punctuation,     // category_other_punctuation
    GeneralCategory::open_punctuation,      // category_open_punctuation
    GeneralCategory::currency_symbol,       // category_currency_symbol
    GeneralCategory::modifier_symbol,       // category_modifier_symbol
    GeneralCategory::math_symbol,           // category_math_symbol
    GeneralCategory::other_symbol,          // category_other_symbol
    GeneralCategory::line_separator,        // category_line_separator
    GeneralCategory::paragraph_separator,   // category_paragraph_separator
    GeneralCategory::space_separator,       // category_space_separator
};

} // namespace

GeneralCategory general_category(const u32 code_point) noexcept {
    const u32 number = unicode::general_category(code_point);
    const GeneralCategory result = number < 30 ? categories[number] : GeneralCategory::unassigned;
    return result;
}

u32 combining_class(const u32 code_point) noexcept {
    const u32 result = unicode::combining_class(code_point);
    return result;
}

bool canonical_decomposition(const u32 code_point, u32& first, u32& second) noexcept {
    const bool result = unicode::decompose(code_point, first, second);
    return result;
}

bool canonical_composition(const u32 first, const u32 second, u32& composed) noexcept {
    const bool result = unicode::compose(first, second, composed);
    return result;
}

} // namespace stxlib::text
