#pragma once

// stx_minmallocpp: the constants of its address-space layout and its size
// classes, with the arithmetic every other file relies on.
//
// stx_minmallocpp is STXlib's own allocator, inspired by and derived from
// mimalloc v3, with many features of its own (the owner, 2026-11-15; NOTICE
// says what it takes and what it adds). The design is in the seat's
// DESIGN.md (notes); this header states the numbers.
//
// Address space. Memory is reserved from the host in regions of 1 GiB,
// aligned to 1 GiB, and cut into slices of 64 KiB. A region's first 288
// slices hold its metadata (page.hpp gives the table); the rest are data.
// Data is committed in units of 1 MiB (16 slices). A directory with one
// cell per GiB of a 48-bit address space says which GiBs are ours.
//
// 32-bit addresses (wasm32, the one 32-bit target types.hpp admits). The
// whole address space is at most 4 GiB, and 2 GiB under Emscripten's
// default MAXIMUM_MEMORY, so 1 GiB regions would leave room for one region
// and no mapping. There a region is 64 MiB, aligned to 64 MiB: 1,024 slices,
// the first 32 of them metadata (page.hpp), 992 data. Addresses at or above
// 2 GiB are never ours, and blocks above 16 MiB have mappings of their own.
// Everything else (slices, units, pages, classes) is the same at both
// widths.
//
// Pages. A page is a run of slices that holds blocks of one size: 1 slice
// for blocks up to 8 KiB, 8 slices up to 64 KiB, 64 slices up to 512 KiB,
// so a page holds between 8 and 4,096 blocks. A larger block has a page to
// itself; above 32 MiB (16 MiB with 32-bit addresses) it has a mapping to
// itself.
//
// Size classes. 56 of them: 16 * (i + 1) for i < 8 (16 to 128), then for
// each k from 7 to 18 the four sizes 2^(k-2) * {5, 6, 7, 8} (160 to
// 524,288). Every class size is a multiple of 16. Above 128 bytes a block
// is at most 25% larger than the request (just above a power of two);
// below, the 16-byte step decides (a request of 17 bytes gets 32).
//
// Alignment (DESIGN.md, section 5.2, with the proof): for a power of two A
// from 16 to 65,536 and a request n, the class of round_up(n, A) has a size
// that is a multiple of A. A page's first block is at its first slice's
// start, a multiple of 65,536, and block i is at i * class size, so every
// block of that class is A-aligned. Aligned allocation is therefore
// ordinary allocation of the rounded size; no block is ever handed out at
// an address inside another block.
//
// tests/stx_minmallocpp_tests.cpp checks class_of and class_bytes
// exhaustively over 1 to 524,288, the alignment claim over every (size,
// alignment) in range, and the block index (page.hpp) over every offset of
// every class.

#include "stxlib/types.hpp"

#include <bit>

namespace stxlib::stx_minmallocpp {

// ---- the address space ---------------------------------------------------------

// True where addresses are 64 bits wide; false on wasm32 (above).
inline constexpr bool wide_addresses = sizeof(uptr) == 8;
static_assert(sizeof(uptr) == 8 || sizeof(uptr) == 4, "stx_minmallocpp: addresses of 32 or 64 bits");

inline constexpr usize slice_shift = 16;
inline constexpr usize slice_bytes = static_cast<usize>(1) << slice_shift;
inline constexpr usize region_shift = wide_addresses ? 30 : 26;
inline constexpr usize region_bytes = static_cast<usize>(1) << region_shift;
inline constexpr usize region_slices = region_bytes / slice_bytes; // 16,384 (32-bit: 1,024)
inline constexpr usize unit_slices = 16;
inline constexpr usize unit_bytes = unit_slices * slice_bytes; // 1 MiB
inline constexpr usize region_units = region_slices / unit_slices; // 1,024 (32-bit: 64)

// Addresses at or above 2^address_bits are never ours (a reservation there
// is refused), so the directory has 2^18 cells (32-bit: 32). With 32-bit
// addresses the limit is 2^31, not 2^32: an address is shifted right by
// address_bits to test it, and a shift by the type's full width is
// undefined; 2 GiB is also Emscripten's default maximum memory.
inline constexpr usize address_bits = wide_addresses ? 48 : 31;
static_assert(address_bits < sizeof(uptr) * 8, "stx_minmallocpp: an address shifted by address_bits is defined");
inline constexpr usize directory_cells = static_cast<usize>(1) << (address_bits - region_shift);

// ---- blocks and pages ----------------------------------------------------------

// Every block is aligned to at least this, and no block is smaller.
inline constexpr usize block_alignment = 16;

// A page's free blocks are bits in at most this many 64-bit words.
inline constexpr usize page_words = 64;
inline constexpr usize word_bits = 64;
inline constexpr usize page_blocks_limit = page_words * word_bits; // 4,096

inline constexpr usize class_count = 56;
inline constexpr usize linear_classes = 8;
inline constexpr usize largest_class_bytes = static_cast<usize>(1) << 19; // 512 KiB

// PageMeta::class_index of a page that holds one block of its own size.
inline constexpr usize singleton_class = class_count;

// Slices per page, by the class's block size.
inline constexpr usize small_page_slices = 1;
inline constexpr usize medium_page_slices = 8;
inline constexpr usize large_page_slices = 64;
inline constexpr usize small_block_limit = slice_bytes / 8; // 8 KiB
inline constexpr usize medium_block_limit = slice_bytes; // 64 KiB

// Alignments up to a slice are met by the class rule above; larger ones, up
// to alignment_limit, by a page or mapping whose start is chosen aligned.
inline constexpr usize class_alignment_limit = slice_bytes;
inline constexpr usize alignment_limit = region_bytes;

// A block larger than this, or aligned to more than this, has a mapping to
// itself (region.hpp, huge mappings): 512 slices (32-bit: 256, so that the
// run a page of one block needs at the largest alignment, 2 * 256 - 1
// slices, fits a new 32-bit region's data; region.hpp checks it).
inline constexpr usize huge_threshold = static_cast<usize>(wide_addresses ? 32 : 16) * 1024 * 1024;

// The largest request considered; anything above is refused before any
// arithmetic, so no rounding below can wrap.
inline constexpr usize largest_request = usize_max >> 1;

// ---- size classes ---------------------------------------------------------------

// The block size of class `index`. Requires index < class_count.
[[nodiscard]] constexpr usize class_bytes(const usize index) noexcept {
    if (index < linear_classes) {
        const usize linear = block_alignment * (index + 1);
        return linear;
    }
    const usize step = index - linear_classes;
    const usize octave = 7 + (step >> 2);
    const usize quarters = 5 + (step & 3);
    const usize result = quarters << (octave - 2);
    return result;
}

// The smallest class whose blocks hold `bytes`. Requires
// 1 <= bytes <= largest_class_bytes.
[[nodiscard]] constexpr usize class_of(const usize bytes) noexcept {
    const usize last = bytes - 1;
    if (last < 128) {
        const usize linear = last >> 4;
        return linear;
    }
    // 2^octave <= last < 2^(octave + 1), octave from 7 to 18.
    const usize octave = static_cast<usize>(std::bit_width(last)) - 1;
    const usize quarter = (last >> (octave - 2)) & 3;
    const usize result = linear_classes + ((octave - 7) << 2) + quarter;
    return result;
}

// `value` rounded up to a multiple of `alignment`, a power of two. Requires
// value <= largest_request and alignment <= alignment_limit, so the sum
// cannot wrap.
[[nodiscard]] constexpr usize round_up(const usize value, const usize alignment) noexcept {
    const usize mask = alignment - 1;
    const usize result = (value + mask) & ~mask;
    return result;
}

// What a class's pages look like. Fixed per class, computed once, at
// compile time (the performance notes' principle 5: the shape is fixed
// before the loop).
struct ClassShape final {
    usize block_bytes{0};
    usize page_slices{0};
    // Blocks per page: floor(page bytes / block bytes), 8 to 4,096.
    usize capacity{0};
    // Bitmap words in use: ceil(capacity / 64).
    usize words{0};
    // ceil(2^42 / block_bytes): the block index of a byte offset in the
    // page is (offset * divide_magic) >> 42 (page.hpp, block_index).
    u64 divide_magic{0};
};

inline constexpr usize divide_shift = 42;

[[nodiscard]] constexpr ClassShape shape_of_class(const usize index) noexcept {
    const usize block = class_bytes(index);
    usize slices = large_page_slices;
    if (block <= small_block_limit) {
        slices = small_page_slices;
    } else if (block <= medium_block_limit) {
        slices = medium_page_slices;
    }
    const usize capacity = (slices * slice_bytes) / block;
    const u64 numerator = static_cast<u64>(1) << divide_shift;
    const u64 magic = (numerator + static_cast<u64>(block) - 1) / static_cast<u64>(block);
    const ClassShape result{
        .block_bytes = block,
        .page_slices = slices,
        .capacity = capacity,
        .words = (capacity + word_bits - 1) / word_bits,
        .divide_magic = magic,
    };
    return result;
}

struct ClassTable final {
    ClassShape shapes[class_count]{};
};

[[nodiscard]] constexpr ClassTable make_class_table() noexcept {
    ClassTable table{};
    for (usize index = 0; index < class_count; ++index) {
        table.shapes[index] = shape_of_class(index);
    }
    return table;
}

inline constexpr ClassTable class_table = make_class_table();

// The table's own invariants, checked for every class at compile time:
// sizes strictly increase in multiples of 16 up to the largest; a page
// holds 8 to 4,096 blocks; every class round-trips through class_of.
[[nodiscard]] constexpr bool class_table_holds() noexcept {
    usize previous = 0;
    for (usize index = 0; index < class_count; ++index) {
        const ClassShape& shape = class_table.shapes[index];
        if (shape.block_bytes <= previous) return false;
        if (shape.block_bytes % block_alignment != 0) return false;
        if (shape.capacity < 8 || shape.capacity > page_blocks_limit) return false;
        if (shape.words == 0 || shape.words > page_words) return false;
        if (class_of(shape.block_bytes) != index) return false;
        if (class_of(previous + 1) != index) return false;
        previous = shape.block_bytes;
    }
    const bool ends_at_largest = previous == largest_class_bytes;
    return ends_at_largest;
}

static_assert(class_table_holds(), "stx_minmallocpp: the size class table");
static_assert(class_bytes(0) == 16 && class_bytes(7) == 128 && class_bytes(8) == 160 && class_bytes(55) == largest_class_bytes,
              "stx_minmallocpp: the class sizes at the joins");

} // namespace stxlib::stx_minmallocpp
