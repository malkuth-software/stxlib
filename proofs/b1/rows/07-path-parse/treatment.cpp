// Row 07: stx_auxlib::Path::parse with find_root, is_ascii_letter,
// upper_ascii, and the valid_utf8 it calls (B1.md section 3.1).
// P0: no undefined behaviour.
// P1: a refusal returns no path and a success no failure.
// P2: parse(parse(t).text()) == parse(t) (idempotence); the result holds no
//     "." segment (except the lone "."), no empty segment, no ".." after a
//     root, and every ".." comes before every other segment.
// Bound: length at most N (N in {4, 8}) over the alphabet below; the
//        alphabet abstraction is part of the bound and is reported.
#include "symbolic_bytes.hpp"
#include "verify.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "stx_auxlib/paths.hpp"
//@ pin source=stx_auxlib path=include/stx_auxlib/paths.hpp sha256=64d086fb9c7b7792a538a298f278ccd65b3d903e30d2caac21e1998e8f3f5af6
//@ lift source=stx_auxlib path=src/unicode.cpp lines=17-76 namespace=stx_auxlib::unicode anonymous sha256=8a953148cc79a4b3b2a96dc1ac8c28153bf3597fa62fb3d78e6f7919d96a3dcb
//@ lift source=stx_auxlib path=src/unicode.cpp lines=82-103 namespace=stx_auxlib::unicode sha256=6c95c9ba894504f76ccde3be4bc5e47a82de605235cfe20a256fbb61f23e3a87
//@ lift source=stx_auxlib path=src/paths.cpp lines=20-50 namespace=stx_auxlib anonymous sha256=3a2ef40391cf91b695e2632e73b2ca2442ff801f8569a134e907f8ef79a5e5d1
//@ lift source=stx_auxlib path=src/paths.cpp lines=54-102 namespace=stx_auxlib sha256=9cc82356b25e80ebe5b132a7c80367c5c15a7732f8f35c761589c81a0ec4b038
//@ lift source=stx_auxlib path=src/paths.cpp lines=163-166 namespace=stx_auxlib sha256=7316fc502dfa3b92f3bc456e517b0a42462ce9685a9b2d97b64a68e61779d693
//@ lift source=stx_auxlib path=src/paths.cpp lines=173-199 namespace=stx_auxlib sha256=9e11e4df74e8c595b2fe572dd2e578c3c300b6a8ea22f0e15b8905b193a7cd2f
//@ mutant id=m1 source=stx_auxlib path=src/paths.cpp line=82 from='segments.back() != ".."' to='segments.back() == ".."'

using stx_auxlib::Path;
using stx_auxlib::PathParse;

namespace {

constexpr unsigned char alphabet[] = {'/', '.', 'a', 'B', ':', '\\', 0x00, 0xc3, 0xa9};

std::size_t spec_root_length(const std::string_view text) {
    if (!text.empty() && text[0] == '/') return 1;
    if (text.size() >= 3 && text[1] == ':' && text[2] == '/') return 3;
    return 0;
}

// The normal-form claims of P2 on a parsed path's text.
void check_normal_form(const std::string_view text) {
    STX_ASSERT(!text.empty(), "P2: a parsed path is not empty");
    if (text == std::string_view(".")) return;
    const std::size_t root = spec_root_length(text);
    const std::string_view rest = text.substr(root);
    bool seen_name = false;
    std::size_t start = 0;
    while (start <= rest.size()) {
        std::size_t end = start;
        while (end < rest.size() && rest[end] != '/') ++end;
        const std::string_view segment = rest.substr(start, end - start);
        start = end + 1;
        if (segment.empty()) {
            STX_ASSERT(rest.empty(), "P2: no empty segment");
            break;
        }
        STX_ASSERT(segment != std::string_view("."), "P2: no '.' segment");
        if (segment == std::string_view("..")) {
            STX_ASSERT(root == 0, "P2: no '..' after a root");
            STX_ASSERT(!seen_name, "P2: every '..' precedes every name");
        } else {
            seen_name = true;
        }
    }
}

} // namespace

int main() {
    SymbolicBytes input(STX_N);
    input.restrict_to(std::span<const unsigned char>(alphabet, sizeof(alphabet)));
    const PathParse first = Path::parse(input.view());
    STX_ASSERT((first.path() == nullptr) != (first.failure() == nullptr), "P1: a path or a failure, never both");
    const Path* const path = first.path();
    if (path != nullptr) {
        const std::string_view text = (*path).text();
        check_normal_form(text);
        const PathParse second = Path::parse(text);
        STX_ASSERT(second.path() != nullptr, "P2: a parsed path parses again");
        STX_ASSERT(*second.path() == *path, "P2: parse is idempotent");
    }
    STX_VACUITY_PROBE();
    return 0;
}
