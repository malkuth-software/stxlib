// ESBMC harnesses: the platform layer's pure cores (A1 platform-rest),
// src/core/pe_image.hpp, src/core/stack_line.hpp and
// src/core/command_line.hpp. Memory safety (every array access in bounds,
// no overflow of a pointer) is checked by ESBMC on every access; the
// assertions below add the stated bounds.
//
// Not yet run. Intended command (the pinned ESBMC of
// notes planning/VERIFICATION_PROGRAMME.md, from the repository root):
//
//   esbmc proofs/esbmc/platform_rest.cpp --std c++20 -I src -I include \
//     --function <harness> --unwind <bound + 2> --unwinding-assertions
//
// with <bound> the harness's array size below (pe: 256; stack line: 64;
// command line: 8). --unwinding-assertions makes a bound that is too small
// fail the run rather than pass it.
//
// Scope: images of at most 256 bytes with every byte nondeterministic (the
// exhaustive test, tests/platform_rest_exhaustive.cpp, covers a 4 KiB image
// with single-byte damage); names of at most 8 bytes; lines of at most 8
// bytes. What is proved is the bounded statement for those sizes only.
//
// Pointer width: run each harness twice, as above (64-bit) and with
// `--32 -D__wasm32__` added (wasm32's 32-bit usize and uptr; types.hpp
// admits 32-bit pointers only under __wasm32__, and the cores here have no
// other __wasm32__ branch). ESBMC's --32 and -D options are as its --help
// lists them; unconfirmed at the pinned ESBMC (VERIFICATION_PROGRAMME).

#include "core/command_line.hpp"
#include "core/pe_image.hpp"
#include "core/stack_line.hpp"

extern "C" unsigned char __VERIFIER_nondet_uchar(void);
extern "C" unsigned int __VERIFIER_nondet_uint(void);
extern "C" unsigned long __VERIFIER_nondet_ulong(void);
extern "C" void __ESBMC_assume(_Bool);
extern "C" void __ESBMC_assert(_Bool, const char*);

namespace pe = stxlib::pe_image;
namespace sl = stxlib::stack_line;
namespace cl = stxlib::command_line;

using stxlib::Byte;
using stxlib::MutableView;
using stxlib::u32;
using stxlib::usize;
using stxlib::View;

namespace {

constexpr usize image_bound = 256;
constexpr usize name_bound = 8;
constexpr usize line_bound = 8;
constexpr usize capacity_bound = 64;

usize nondet_count(const usize bound) {
    const usize value = static_cast<usize>(__VERIFIER_nondet_ulong());
    __ESBMC_assume(value <= bound);
    return value;
}

} // namespace

// pe_image: on any bytes of any length up to the bound, the lookups read
// inside the view and answer inside it.
void harness_pe_image() {
    Byte bytes[image_bound];
    for (usize index = 0; index < image_bound; ++index) bytes[index] = __VERIFIER_nondet_uchar();
    const usize count = nondet_count(image_bound);
    const View<Byte> image(bytes, count);

    pe::Headers headers{};
    if (!pe::read_headers(image, headers)) return;

    char name[name_bound];
    for (usize index = 0; index < name_bound; ++index) name[index] = static_cast<char>(__VERIFIER_nondet_uchar());
    const usize name_count = nondet_count(name_bound);
    pe::Section section{};
    if (pe::find_section(image, headers, View<char>(name, name_count), section)) {
        __ESBMC_assert(name_count >= 1, "a section needs a name");
        __ESBMC_assert(section.rva <= count && count - section.rva >= section.size, "the section lies in the view");
    }

    const u32 target = __VERIFIER_nondet_uint();
    pe::Export found{};
    if (pe::nearest_export(image, headers, target, found)) {
        __ESBMC_assert(found.rva <= target, "the export is at or below the target");
        __ESBMC_assert(found.name_rva < count && count - found.name_rva > found.name_count,
                       "the export's name and its NUL lie in the view");
    }
}

// stack_line: for any frame with names up to the bound and any capacity up
// to its bound, the line fits, ends with a newline, and holds no other
// control byte.
void harness_stack_line() {
    char image_name[name_bound];
    char symbol_name[name_bound];
    for (usize index = 0; index < name_bound; ++index) {
        image_name[index] = static_cast<char>(__VERIFIER_nondet_uchar());
        symbol_name[index] = static_cast<char>(__VERIFIER_nondet_uchar());
    }
    sl::Frame frame{};
    frame.index = static_cast<usize>(__VERIFIER_nondet_ulong());
    frame.address = static_cast<stxlib::uptr>(__VERIFIER_nondet_ulong());
    frame.image = View<char>(image_name, nondet_count(name_bound));
    frame.image_known = __VERIFIER_nondet_uchar() != 0;
    frame.image_offset = static_cast<stxlib::uptr>(__VERIFIER_nondet_ulong());
    frame.symbol = View<char>(symbol_name, nondet_count(name_bound));
    frame.symbol_offset = static_cast<stxlib::uptr>(__VERIFIER_nondet_ulong());

    char output[capacity_bound];
    const usize capacity = nondet_count(capacity_bound);
    const usize written = sl::format(frame, MutableView<char>(output, capacity));
    __ESBMC_assert(written <= capacity, "the line fits its capacity");
    if (capacity == 0) {
        __ESBMC_assert(written == 0, "nothing for no room");
        return;
    }
    __ESBMC_assert(written >= 1 && output[written - 1] == '\n', "the line ends with a newline");
    for (usize index = 0; index + 1 < written; ++index) {
        const unsigned char byte = static_cast<unsigned char>(output[index]);
        __ESBMC_assert(byte >= 0x20 && byte != 0x7f, "no control byte but the newline");
    }
}

// command_line: for any line up to the bound and any rooms, the split stays
// in its rooms (ESBMC's bounds checks) and its counts meet the stated
// bounds; a complete split's starts lie in the text, each argument ending
// with a NUL.
void harness_split() {
    char line[line_bound];
    for (usize index = 0; index < line_bound; ++index) line[index] = static_cast<char>(__VERIFIER_nondet_uchar());
    const usize line_count = nondet_count(line_bound);
    char text[2 * line_bound + 2];
    usize starts[line_bound / 2 + 1];
    const usize text_room = nondet_count(2 * line_bound + 2);
    const usize start_room = nondet_count(line_bound / 2 + 1);
    const cl::SplitCounts counts = cl::split_windows_command_line(
        View<char>(line, line_count), MutableView<char>(text, text_room), MutableView<usize>(starts, start_room));
    __ESBMC_assert(counts.text_bytes <= line_count + counts.argument_count, "text bound");
    __ESBMC_assert(counts.argument_count <= line_count / 2 + 1, "argument bound");
    if (text_room < counts.text_bytes || start_room < counts.argument_count) return;
    for (usize argument = 0; argument < counts.argument_count; ++argument) {
        __ESBMC_assert(starts[argument] < counts.text_bytes, "a start lies in the text");
        if (argument > 0) __ESBMC_assert(text[starts[argument] - 1] == '\0', "the previous argument ended");
    }
    if (counts.text_bytes > 0) __ESBMC_assert(text[counts.text_bytes - 1] == '\0', "the last argument ended");
}

// quote_windows_argument: the quoting takes at most 2n + 2 bytes and
// writes only inside its room.
void harness_quote() {
    char argument[line_bound];
    for (usize index = 0; index < line_bound; ++index) argument[index] = static_cast<char>(__VERIFIER_nondet_uchar());
    const usize count = nondet_count(line_bound);
    char output[2 * line_bound + 2];
    const usize room = nondet_count(2 * line_bound + 2);
    const usize needed = cl::quote_windows_argument(View<char>(argument, count), MutableView<char>(output, room));
    __ESBMC_assert(needed <= 2 * count + 2, "quoting bound");
}
