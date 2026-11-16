// stxlib::glyphs: checks at run time that what a structure holds agrees
// with itself. As for the allocator (STX_MINMALLOCPP_CHECK, page_whole):
// each structure that is carried from call to call has a function that
// says whether it is whole, assumed at every entry and checked at every
// exit when STXLIB_GLYPHS_CHECKS is defined. It is defined for the checked
// test programs (stxlib_glyphs_checked in CMakeLists.txt), which run the
// glyph tests again with every check on; in other builds the checks cost
// nothing and the functions can still be called.
//
// A check that fails is a broken requirement: the program stops
// (stxlib/require.hpp), which a test sees as its failure.

#ifndef STXLIB_ALGORITHMS_GLYPHS_GLYPH_CHECKS_HPP
#define STXLIB_ALGORITHMS_GLYPHS_GLYPH_CHECKS_HPP

#include "stxlib/font_file.hpp"
#include "stxlib/require.hpp"

#if defined(STXLIB_GLYPHS_CHECKS)
#define STXLIB_GLYPHS_CHECK(condition, what) STXLIB_REQUIRE((condition), (what))
#else
#define STXLIB_GLYPHS_CHECK(condition, what) static_cast<void>(0)
#endif

namespace stxlib::glyphs::detail {

// Whether an outline is as the reader promises one: as many tags as points,
// its contours' last points going up and ending at its last point, and no
// contour without a point.
[[nodiscard]] bool outline_whole(const GlyphOutline& outline) noexcept;

} // namespace stxlib::glyphs::detail

#endif
