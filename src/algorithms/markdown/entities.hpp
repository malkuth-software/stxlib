#pragma once

// stxlib::markdown: the HTML named character references (the names
// CommonMark recognises between '&' and ';').

#include "stxlib/types.hpp"

namespace stxlib::markdown::detail {

// The longest name in the table, in bytes, and the shortest.
inline constexpr u32 longest_entity_name = 31;

// Whether name[0, length) is a named character reference; on true `first`
// and `second` are its code points (`second` is 0 when it has one).
// Case-sensitive, as the names are.
[[nodiscard]] bool find_named_reference(const char* name, u32 length, u32& first, u32& second) noexcept;

} // namespace stxlib::markdown::detail
