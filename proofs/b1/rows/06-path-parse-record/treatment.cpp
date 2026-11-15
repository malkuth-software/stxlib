// Row 06: the result record PathParse: parsed, refused, succeeded, failure,
// path (B1.md section 3.1).
// P1: exactly one of path() and failure() is non-null; succeeded() holds
//     exactly when path() is non-null; the failure read back is the one
//     given. "Built only through the two factories" holds by construction:
//     the constructor is private (include/stx_auxlib/paths.hpp:105), which
//     the compiler checks; no tool run is needed for it.
// Bound: one call of each factory; the failure code symbolic over the
//        enumerators, the path the default "." (Path's other constructor is
//        private to Path and PathParse).
#include "verify.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "stx_auxlib/paths.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/paths.hpp sha256=64d086fb9c7b7792a538a298f278ccd65b3d903e30d2caac21e1998e8f3f5af6
//@ lift source=stx_auxlib path=src/paths.cpp lines=54-56 namespace=stx_auxlib sha256=e3679d083a9dbcc75b065e4665fb02aece17ef2c9e35a0f65ece6952ed17c476
//@ lift source=stx_auxlib path=src/paths.cpp lines=102-102 namespace=stx_auxlib sha256=4c272e8ff3682e241cecc49879480f8642f6e854cca45bd4bef83e0edd10ffb5
//@ lift source=stx_auxlib path=src/paths.cpp lines=173-199 namespace=stx_auxlib sha256=9e11e4df74e8c595b2fe572dd2e578c3c300b6a8ea22f0e15b8905b193a7cd2f
//@ mutant id=m1 source=stx_auxlib path=src/paths.cpp line=186 from="std::holds_alternative<Path>" to="std::holds_alternative<PathFailure>"

using stx_auxlib::Path;
using stx_auxlib::PathFailure;
using stx_auxlib::PathParse;

int main() {
    const PathParse parsed = PathParse::parsed(Path());
    STX_ASSERT(parsed.path() != nullptr, "P1: parsed has a path");
    STX_ASSERT(parsed.failure() == nullptr, "P1: parsed has no failure");
    STX_ASSERT(parsed.succeeded(), "P1: parsed succeeded");
    STX_ASSERT((*parsed.path()).text() == std::string_view("."), "P1: parsed keeps its path");

    const std::uint8_t code = nondet_uchar();
    STX_ASSUME(code >= 1 && code <= 7);
    const PathFailure given = static_cast<PathFailure>(code);
    const PathParse refused = PathParse::refused(given);
    STX_ASSERT(refused.path() == nullptr, "P1: refused has no path");
    STX_ASSERT(refused.failure() != nullptr, "P1: refused has a failure");
    STX_ASSERT(!refused.succeeded(), "P1: refused did not succeed");
    STX_ASSERT(*refused.failure() == given, "P1: refused keeps its failure");
    STX_VACUITY_PROBE();
    return 0;
}
