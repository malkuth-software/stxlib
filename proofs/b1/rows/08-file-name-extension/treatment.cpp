// Row 08: stx_auxlib::Path::file_name and Path::extension with root_length
// (B1.md section 3.1). The path is obtained from Path::parse (row 07's
// code), since Path's text constructor is private.
// P0: no undefined behaviour.
// P2: against the header's own statement (paths.hpp:74-79), written here
//     independently: file_name is the text after the last '/', or empty when
//     that is "", "." or ".."; extension is the file name from its last
//     '.', empty when there is none or that dot is the name's first byte.
// Bound: input length at most N (N in {4, 8}) over row 07's alphabet; the
//        path is then normalised (the controls' P2 against libc++ is no
//        longer part of B1).
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
//@ lift source=stx_auxlib path=src/paths.cpp lines=54-108 namespace=stx_auxlib sha256=e91803bca485e401aedfba40f5046d6a9001875665a2e6431e3e655a057c9d8b
//@ lift source=stx_auxlib path=src/paths.cpp lines=146-161 namespace=stx_auxlib sha256=f2594c848bbb56f1cf07bd95197859be1a2d50aefc9480cd0cb1da1c52870d0b
//@ lift source=stx_auxlib path=src/paths.cpp lines=173-199 namespace=stx_auxlib sha256=9e11e4df74e8c595b2fe572dd2e578c3c300b6a8ea22f0e15b8905b193a7cd2f
//@ mutant id=m1 source=stx_auxlib path=src/paths.cpp line=159 from=" || dot == 0" to=""

using stx_auxlib::Path;
using stx_auxlib::PathParse;

namespace {

constexpr unsigned char alphabet[] = {'/', '.', 'a', 'B', ':', '\\', 0x00, 0xc3, 0xa9};

std::string_view spec_file_name(const std::string_view text) {
    std::size_t start = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '/') start = index + 1;
    }
    const std::string_view segment = text.substr(start);
    if (segment == std::string_view(".") || segment == std::string_view("..")) return std::string_view();
    return segment;
}

std::string_view spec_extension(const std::string_view name) {
    std::size_t dot = name.size();
    for (std::size_t index = 0; index < name.size(); ++index) {
        if (name[index] == '.') dot = index;
    }
    if (dot == name.size() || dot == 0) return std::string_view();
    return name.substr(dot);
}

} // namespace

int main() {
    SymbolicBytes input(STX_N);
    input.restrict_to(std::span<const unsigned char>(alphabet, sizeof(alphabet)));
    const PathParse parsed = Path::parse(input.view());
    const Path* const path = parsed.path();
    if (path != nullptr) {
        const std::string_view text = (*path).text();
        const std::string_view name = (*path).file_name();
        const std::string_view expected_name = spec_file_name(text);
        STX_ASSERT(name == expected_name, "P2: file_name is the last segment, empty for a root, '.' and '..'");
        const std::string_view extension = (*path).extension();
        STX_ASSERT(extension == spec_extension(expected_name), "P2: extension is the name from its last '.'");
    }
    STX_VACUITY_PROBE();
    return 0;
}
