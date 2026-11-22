// Tests of STXlib's outline-atomics helpers (src/hosted/elf/
// outline_atomics_aarch64.cpp; hosted/outline_atomics_backend.hpp), on
// Linux and FreeBSD aarch64:
// - every helper against a plain reference, on the load-exclusive/
//   store-exclusive loop (memo forced to 1), on the LSE path (memo forced to
//   3, only where the CPU has LSE), and through detection (memo reset to 0
//   before each call): the value returned, the bytes stored, the bytes
//   around them untouched, and arguments narrower than a register carrying
//   other bits above them (the helpers must read only the low bytes);
// - the register contract on every one of those calls: only x0 (x0 and x1
//   for cas16), x15, x16, x17 and the flags may change, so detection, which
//   runs inside the first call, is held to it too;
// - detection agrees with simd::detected_features().lse and leaves the memo
//   at 1 or 3;
// - a concurrent stress run (4 threads, 20,000 rounds each) of
//   fetch-add, compare-and-swap loops (4 and 16 bytes), fetch-or and
//   fetch-and-not bit ownership, fetch-xor and swap, in each mode, the first
//   with the memo at 0 so that the threads race the first detection.
// Every helper is called through call_helper, which loads x0 to x14 from a
// record, branches to the helper and stores them back, so the test controls
// every argument bit and sees every register the helper leaves. The stress
// counts are tests, not proofs (AGENTS.md, "Verification").
// Elsewhere the program checks what simd.hpp promises of Features::lse.

#include "hosted/outline_atomics_backend.hpp"
#include "stxlib/simd.hpp"
#include "stxlib/types.hpp"

#include "test_support.hpp"

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#if defined(STXLIB_OUTLINE_ATOMICS)
// The helpers, declared only to take their addresses: they are called
// through call_helper, never through these types.
extern "C" void __aarch64_cas1_relax() noexcept;
extern "C" void __aarch64_cas1_acq() noexcept;
extern "C" void __aarch64_cas1_rel() noexcept;
extern "C" void __aarch64_cas1_acq_rel() noexcept;
extern "C" void __aarch64_cas2_relax() noexcept;
extern "C" void __aarch64_cas2_acq() noexcept;
extern "C" void __aarch64_cas2_rel() noexcept;
extern "C" void __aarch64_cas2_acq_rel() noexcept;
extern "C" void __aarch64_cas4_relax() noexcept;
extern "C" void __aarch64_cas4_acq() noexcept;
extern "C" void __aarch64_cas4_rel() noexcept;
extern "C" void __aarch64_cas4_acq_rel() noexcept;
extern "C" void __aarch64_cas8_relax() noexcept;
extern "C" void __aarch64_cas8_acq() noexcept;
extern "C" void __aarch64_cas8_rel() noexcept;
extern "C" void __aarch64_cas8_acq_rel() noexcept;
extern "C" void __aarch64_cas16_relax() noexcept;
extern "C" void __aarch64_cas16_acq() noexcept;
extern "C" void __aarch64_cas16_rel() noexcept;
extern "C" void __aarch64_cas16_acq_rel() noexcept;
extern "C" void __aarch64_swp1_relax() noexcept;
extern "C" void __aarch64_swp1_acq() noexcept;
extern "C" void __aarch64_swp1_rel() noexcept;
extern "C" void __aarch64_swp1_acq_rel() noexcept;
extern "C" void __aarch64_swp2_relax() noexcept;
extern "C" void __aarch64_swp2_acq() noexcept;
extern "C" void __aarch64_swp2_rel() noexcept;
extern "C" void __aarch64_swp2_acq_rel() noexcept;
extern "C" void __aarch64_swp4_relax() noexcept;
extern "C" void __aarch64_swp4_acq() noexcept;
extern "C" void __aarch64_swp4_rel() noexcept;
extern "C" void __aarch64_swp4_acq_rel() noexcept;
extern "C" void __aarch64_swp8_relax() noexcept;
extern "C" void __aarch64_swp8_acq() noexcept;
extern "C" void __aarch64_swp8_rel() noexcept;
extern "C" void __aarch64_swp8_acq_rel() noexcept;
extern "C" void __aarch64_ldadd1_relax() noexcept;
extern "C" void __aarch64_ldadd1_acq() noexcept;
extern "C" void __aarch64_ldadd1_rel() noexcept;
extern "C" void __aarch64_ldadd1_acq_rel() noexcept;
extern "C" void __aarch64_ldadd2_relax() noexcept;
extern "C" void __aarch64_ldadd2_acq() noexcept;
extern "C" void __aarch64_ldadd2_rel() noexcept;
extern "C" void __aarch64_ldadd2_acq_rel() noexcept;
extern "C" void __aarch64_ldadd4_relax() noexcept;
extern "C" void __aarch64_ldadd4_acq() noexcept;
extern "C" void __aarch64_ldadd4_rel() noexcept;
extern "C" void __aarch64_ldadd4_acq_rel() noexcept;
extern "C" void __aarch64_ldadd8_relax() noexcept;
extern "C" void __aarch64_ldadd8_acq() noexcept;
extern "C" void __aarch64_ldadd8_rel() noexcept;
extern "C" void __aarch64_ldadd8_acq_rel() noexcept;
extern "C" void __aarch64_ldclr1_relax() noexcept;
extern "C" void __aarch64_ldclr1_acq() noexcept;
extern "C" void __aarch64_ldclr1_rel() noexcept;
extern "C" void __aarch64_ldclr1_acq_rel() noexcept;
extern "C" void __aarch64_ldclr2_relax() noexcept;
extern "C" void __aarch64_ldclr2_acq() noexcept;
extern "C" void __aarch64_ldclr2_rel() noexcept;
extern "C" void __aarch64_ldclr2_acq_rel() noexcept;
extern "C" void __aarch64_ldclr4_relax() noexcept;
extern "C" void __aarch64_ldclr4_acq() noexcept;
extern "C" void __aarch64_ldclr4_rel() noexcept;
extern "C" void __aarch64_ldclr4_acq_rel() noexcept;
extern "C" void __aarch64_ldclr8_relax() noexcept;
extern "C" void __aarch64_ldclr8_acq() noexcept;
extern "C" void __aarch64_ldclr8_rel() noexcept;
extern "C" void __aarch64_ldclr8_acq_rel() noexcept;
extern "C" void __aarch64_ldeor1_relax() noexcept;
extern "C" void __aarch64_ldeor1_acq() noexcept;
extern "C" void __aarch64_ldeor1_rel() noexcept;
extern "C" void __aarch64_ldeor1_acq_rel() noexcept;
extern "C" void __aarch64_ldeor2_relax() noexcept;
extern "C" void __aarch64_ldeor2_acq() noexcept;
extern "C" void __aarch64_ldeor2_rel() noexcept;
extern "C" void __aarch64_ldeor2_acq_rel() noexcept;
extern "C" void __aarch64_ldeor4_relax() noexcept;
extern "C" void __aarch64_ldeor4_acq() noexcept;
extern "C" void __aarch64_ldeor4_rel() noexcept;
extern "C" void __aarch64_ldeor4_acq_rel() noexcept;
extern "C" void __aarch64_ldeor8_relax() noexcept;
extern "C" void __aarch64_ldeor8_acq() noexcept;
extern "C" void __aarch64_ldeor8_rel() noexcept;
extern "C" void __aarch64_ldeor8_acq_rel() noexcept;
extern "C" void __aarch64_ldset1_relax() noexcept;
extern "C" void __aarch64_ldset1_acq() noexcept;
extern "C" void __aarch64_ldset1_rel() noexcept;
extern "C" void __aarch64_ldset1_acq_rel() noexcept;
extern "C" void __aarch64_ldset2_relax() noexcept;
extern "C" void __aarch64_ldset2_acq() noexcept;
extern "C" void __aarch64_ldset2_rel() noexcept;
extern "C" void __aarch64_ldset2_acq_rel() noexcept;
extern "C" void __aarch64_ldset4_relax() noexcept;
extern "C" void __aarch64_ldset4_acq() noexcept;
extern "C" void __aarch64_ldset4_rel() noexcept;
extern "C" void __aarch64_ldset4_acq_rel() noexcept;
extern "C" void __aarch64_ldset8_relax() noexcept;
extern "C" void __aarch64_ldset8_acq() noexcept;
extern "C" void __aarch64_ldset8_rel() noexcept;
extern "C" void __aarch64_ldset8_acq_rel() noexcept;
#endif

namespace {

namespace simd = stxlib::simd;
using stxlib::u16;
using stxlib::u32;
using stxlib::u64;
using stxlib::u8;

#if defined(STXLIB_OUTLINE_ATOMICS)

using Helper = void (*)() noexcept;

enum class Kind : u8 { cas, cas16, swp, add, clr, eor, set };

struct HelperEntry final {
    const char* name;
    Helper address;
    Kind kind;
    u32 size; // bytes: 1, 2, 4, 8, or 16 for cas16
};

const HelperEntry helper_table[] = {
    {"aarch64_cas1_relax", &__aarch64_cas1_relax, Kind::cas, 1},
    {"aarch64_cas1_acq", &__aarch64_cas1_acq, Kind::cas, 1},
    {"aarch64_cas1_rel", &__aarch64_cas1_rel, Kind::cas, 1},
    {"aarch64_cas1_acq_rel", &__aarch64_cas1_acq_rel, Kind::cas, 1},
    {"aarch64_cas2_relax", &__aarch64_cas2_relax, Kind::cas, 2},
    {"aarch64_cas2_acq", &__aarch64_cas2_acq, Kind::cas, 2},
    {"aarch64_cas2_rel", &__aarch64_cas2_rel, Kind::cas, 2},
    {"aarch64_cas2_acq_rel", &__aarch64_cas2_acq_rel, Kind::cas, 2},
    {"aarch64_cas4_relax", &__aarch64_cas4_relax, Kind::cas, 4},
    {"aarch64_cas4_acq", &__aarch64_cas4_acq, Kind::cas, 4},
    {"aarch64_cas4_rel", &__aarch64_cas4_rel, Kind::cas, 4},
    {"aarch64_cas4_acq_rel", &__aarch64_cas4_acq_rel, Kind::cas, 4},
    {"aarch64_cas8_relax", &__aarch64_cas8_relax, Kind::cas, 8},
    {"aarch64_cas8_acq", &__aarch64_cas8_acq, Kind::cas, 8},
    {"aarch64_cas8_rel", &__aarch64_cas8_rel, Kind::cas, 8},
    {"aarch64_cas8_acq_rel", &__aarch64_cas8_acq_rel, Kind::cas, 8},
    {"aarch64_cas16_relax", &__aarch64_cas16_relax, Kind::cas16, 16},
    {"aarch64_cas16_acq", &__aarch64_cas16_acq, Kind::cas16, 16},
    {"aarch64_cas16_rel", &__aarch64_cas16_rel, Kind::cas16, 16},
    {"aarch64_cas16_acq_rel", &__aarch64_cas16_acq_rel, Kind::cas16, 16},
    {"aarch64_swp1_relax", &__aarch64_swp1_relax, Kind::swp, 1},
    {"aarch64_swp1_acq", &__aarch64_swp1_acq, Kind::swp, 1},
    {"aarch64_swp1_rel", &__aarch64_swp1_rel, Kind::swp, 1},
    {"aarch64_swp1_acq_rel", &__aarch64_swp1_acq_rel, Kind::swp, 1},
    {"aarch64_swp2_relax", &__aarch64_swp2_relax, Kind::swp, 2},
    {"aarch64_swp2_acq", &__aarch64_swp2_acq, Kind::swp, 2},
    {"aarch64_swp2_rel", &__aarch64_swp2_rel, Kind::swp, 2},
    {"aarch64_swp2_acq_rel", &__aarch64_swp2_acq_rel, Kind::swp, 2},
    {"aarch64_swp4_relax", &__aarch64_swp4_relax, Kind::swp, 4},
    {"aarch64_swp4_acq", &__aarch64_swp4_acq, Kind::swp, 4},
    {"aarch64_swp4_rel", &__aarch64_swp4_rel, Kind::swp, 4},
    {"aarch64_swp4_acq_rel", &__aarch64_swp4_acq_rel, Kind::swp, 4},
    {"aarch64_swp8_relax", &__aarch64_swp8_relax, Kind::swp, 8},
    {"aarch64_swp8_acq", &__aarch64_swp8_acq, Kind::swp, 8},
    {"aarch64_swp8_rel", &__aarch64_swp8_rel, Kind::swp, 8},
    {"aarch64_swp8_acq_rel", &__aarch64_swp8_acq_rel, Kind::swp, 8},
    {"aarch64_ldadd1_relax", &__aarch64_ldadd1_relax, Kind::add, 1},
    {"aarch64_ldadd1_acq", &__aarch64_ldadd1_acq, Kind::add, 1},
    {"aarch64_ldadd1_rel", &__aarch64_ldadd1_rel, Kind::add, 1},
    {"aarch64_ldadd1_acq_rel", &__aarch64_ldadd1_acq_rel, Kind::add, 1},
    {"aarch64_ldadd2_relax", &__aarch64_ldadd2_relax, Kind::add, 2},
    {"aarch64_ldadd2_acq", &__aarch64_ldadd2_acq, Kind::add, 2},
    {"aarch64_ldadd2_rel", &__aarch64_ldadd2_rel, Kind::add, 2},
    {"aarch64_ldadd2_acq_rel", &__aarch64_ldadd2_acq_rel, Kind::add, 2},
    {"aarch64_ldadd4_relax", &__aarch64_ldadd4_relax, Kind::add, 4},
    {"aarch64_ldadd4_acq", &__aarch64_ldadd4_acq, Kind::add, 4},
    {"aarch64_ldadd4_rel", &__aarch64_ldadd4_rel, Kind::add, 4},
    {"aarch64_ldadd4_acq_rel", &__aarch64_ldadd4_acq_rel, Kind::add, 4},
    {"aarch64_ldadd8_relax", &__aarch64_ldadd8_relax, Kind::add, 8},
    {"aarch64_ldadd8_acq", &__aarch64_ldadd8_acq, Kind::add, 8},
    {"aarch64_ldadd8_rel", &__aarch64_ldadd8_rel, Kind::add, 8},
    {"aarch64_ldadd8_acq_rel", &__aarch64_ldadd8_acq_rel, Kind::add, 8},
    {"aarch64_ldclr1_relax", &__aarch64_ldclr1_relax, Kind::clr, 1},
    {"aarch64_ldclr1_acq", &__aarch64_ldclr1_acq, Kind::clr, 1},
    {"aarch64_ldclr1_rel", &__aarch64_ldclr1_rel, Kind::clr, 1},
    {"aarch64_ldclr1_acq_rel", &__aarch64_ldclr1_acq_rel, Kind::clr, 1},
    {"aarch64_ldclr2_relax", &__aarch64_ldclr2_relax, Kind::clr, 2},
    {"aarch64_ldclr2_acq", &__aarch64_ldclr2_acq, Kind::clr, 2},
    {"aarch64_ldclr2_rel", &__aarch64_ldclr2_rel, Kind::clr, 2},
    {"aarch64_ldclr2_acq_rel", &__aarch64_ldclr2_acq_rel, Kind::clr, 2},
    {"aarch64_ldclr4_relax", &__aarch64_ldclr4_relax, Kind::clr, 4},
    {"aarch64_ldclr4_acq", &__aarch64_ldclr4_acq, Kind::clr, 4},
    {"aarch64_ldclr4_rel", &__aarch64_ldclr4_rel, Kind::clr, 4},
    {"aarch64_ldclr4_acq_rel", &__aarch64_ldclr4_acq_rel, Kind::clr, 4},
    {"aarch64_ldclr8_relax", &__aarch64_ldclr8_relax, Kind::clr, 8},
    {"aarch64_ldclr8_acq", &__aarch64_ldclr8_acq, Kind::clr, 8},
    {"aarch64_ldclr8_rel", &__aarch64_ldclr8_rel, Kind::clr, 8},
    {"aarch64_ldclr8_acq_rel", &__aarch64_ldclr8_acq_rel, Kind::clr, 8},
    {"aarch64_ldeor1_relax", &__aarch64_ldeor1_relax, Kind::eor, 1},
    {"aarch64_ldeor1_acq", &__aarch64_ldeor1_acq, Kind::eor, 1},
    {"aarch64_ldeor1_rel", &__aarch64_ldeor1_rel, Kind::eor, 1},
    {"aarch64_ldeor1_acq_rel", &__aarch64_ldeor1_acq_rel, Kind::eor, 1},
    {"aarch64_ldeor2_relax", &__aarch64_ldeor2_relax, Kind::eor, 2},
    {"aarch64_ldeor2_acq", &__aarch64_ldeor2_acq, Kind::eor, 2},
    {"aarch64_ldeor2_rel", &__aarch64_ldeor2_rel, Kind::eor, 2},
    {"aarch64_ldeor2_acq_rel", &__aarch64_ldeor2_acq_rel, Kind::eor, 2},
    {"aarch64_ldeor4_relax", &__aarch64_ldeor4_relax, Kind::eor, 4},
    {"aarch64_ldeor4_acq", &__aarch64_ldeor4_acq, Kind::eor, 4},
    {"aarch64_ldeor4_rel", &__aarch64_ldeor4_rel, Kind::eor, 4},
    {"aarch64_ldeor4_acq_rel", &__aarch64_ldeor4_acq_rel, Kind::eor, 4},
    {"aarch64_ldeor8_relax", &__aarch64_ldeor8_relax, Kind::eor, 8},
    {"aarch64_ldeor8_acq", &__aarch64_ldeor8_acq, Kind::eor, 8},
    {"aarch64_ldeor8_rel", &__aarch64_ldeor8_rel, Kind::eor, 8},
    {"aarch64_ldeor8_acq_rel", &__aarch64_ldeor8_acq_rel, Kind::eor, 8},
    {"aarch64_ldset1_relax", &__aarch64_ldset1_relax, Kind::set, 1},
    {"aarch64_ldset1_acq", &__aarch64_ldset1_acq, Kind::set, 1},
    {"aarch64_ldset1_rel", &__aarch64_ldset1_rel, Kind::set, 1},
    {"aarch64_ldset1_acq_rel", &__aarch64_ldset1_acq_rel, Kind::set, 1},
    {"aarch64_ldset2_relax", &__aarch64_ldset2_relax, Kind::set, 2},
    {"aarch64_ldset2_acq", &__aarch64_ldset2_acq, Kind::set, 2},
    {"aarch64_ldset2_rel", &__aarch64_ldset2_rel, Kind::set, 2},
    {"aarch64_ldset2_acq_rel", &__aarch64_ldset2_acq_rel, Kind::set, 2},
    {"aarch64_ldset4_relax", &__aarch64_ldset4_relax, Kind::set, 4},
    {"aarch64_ldset4_acq", &__aarch64_ldset4_acq, Kind::set, 4},
    {"aarch64_ldset4_rel", &__aarch64_ldset4_rel, Kind::set, 4},
    {"aarch64_ldset4_acq_rel", &__aarch64_ldset4_acq_rel, Kind::set, 4},
    {"aarch64_ldset8_relax", &__aarch64_ldset8_relax, Kind::set, 8},
    {"aarch64_ldset8_acq", &__aarch64_ldset8_acq, Kind::set, 8},
    {"aarch64_ldset8_rel", &__aarch64_ldset8_rel, Kind::set, 8},
    {"aarch64_ldset8_acq_rel", &__aarch64_ldset8_acq_rel, Kind::set, 8},
};

// The memo's values (outline_atomics_backend.hpp).
constexpr u8 memo_undetected = 0;
constexpr u8 memo_loop = 1;
constexpr u8 memo_lse = 3;

// x0 to x14 as the helper receives them and as it leaves them.
struct Registers final {
    u64 x[15];
};

// Loads x0 to x14 from `registers`, calls `helper`, stores x0 to x14 back.
// x0 to x17 and x30 are declared clobbered, so the compiler keeps the record
// address and the helper in registers the helper must preserve (x19 and
// up); x18 is the platform register and is left alone.
void call_helper(const Helper helper, Registers& registers) noexcept {
    u64* const x = registers.x;
    __asm__ volatile("ldp x0, x1, [%[x]]\n"
                     "ldp x2, x3, [%[x], #16]\n"
                     "ldp x4, x5, [%[x], #32]\n"
                     "ldp x6, x7, [%[x], #48]\n"
                     "ldp x8, x9, [%[x], #64]\n"
                     "ldp x10, x11, [%[x], #80]\n"
                     "ldp x12, x13, [%[x], #96]\n"
                     "ldr x14, [%[x], #112]\n"
                     "blr %[helper]\n"
                     "stp x0, x1, [%[x]]\n"
                     "stp x2, x3, [%[x], #16]\n"
                     "stp x4, x5, [%[x], #32]\n"
                     "stp x6, x7, [%[x], #48]\n"
                     "stp x8, x9, [%[x], #64]\n"
                     "stp x10, x11, [%[x], #80]\n"
                     "stp x12, x13, [%[x], #96]\n"
                     "str x14, [%[x], #112]\n"
                     :
                     : [x] "r"(x), [helper] "r"(helper)
                     : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
                       "x15", "x16", "x17", "x30", "cc", "memory");
}

// Every register a sentinel: x<k> holds 0x5e5e...5e00 + k, so a register a
// helper changes, or copies into another, is seen.
Registers sentinel_registers() noexcept {
    Registers registers{};
    for (u32 k = 0; k < 15; k = k + 1) {
        registers.x[k] = 0x5e5e5e5e5e5e5e00ULL + k;
    }
    return registers;
}

// The registers a helper may change and return in: x0, and x1 for cas16.
bool contract_kept(const Registers& before, const Registers& after, const Kind kind) noexcept {
    const u32 first_kept = kind == Kind::cas16 ? 2U : 1U;
    bool kept = true;
    for (u32 k = first_kept; k < 15; k = k + 1) {
        if (before.x[k] != after.x[k]) kept = false;
    }
    return kept;
}

u64 mask_of(const u32 size) noexcept {
    const u64 result = size >= 8 ? ~0ULL : (1ULL << (8U * size)) - 1ULL;
    return result;
}

// The plain reference: the value a fetch-and-op stores.
u64 reference_store(const Kind kind, const u64 old, const u64 argument, const u64 mask) noexcept {
    u64 result = old;
    switch (kind) {
    case Kind::swp:
        result = argument & mask;
        break;
    case Kind::add:
        result = (old + argument) & mask;
        break;
    case Kind::clr:
        result = old & ~argument & mask;
        break;
    case Kind::eor:
        result = (old ^ argument) & mask;
        break;
    case Kind::set:
        result = (old | argument) & mask;
        break;
    case Kind::cas:
    case Kind::cas16:
        break;
    }
    return result;
}

// The test memory: 48 bytes of guard pattern with the operand at offset 16,
// aligned to 16 as every size needs.
struct Cell final {
    alignas(16) u8 bytes[48];
};

constexpr u8 guard = 0xa5;
constexpr u32 operand_offset = 16;

Cell cell_holding(const u64 low, const u64 high, const u32 size) noexcept {
    Cell cell{};
    std::memset(cell.bytes, guard, sizeof(cell.bytes));
    const u32 low_size = size > 8 ? 8U : size;
    std::memcpy(cell.bytes + operand_offset, &low, low_size);
    if (size > 8) std::memcpy(cell.bytes + operand_offset + 8, &high, 8);
    return cell;
}

u64 cell_word(const Cell& cell, const u32 offset, const u32 size) noexcept {
    u64 value = 0;
    std::memcpy(&value, cell.bytes + operand_offset + offset, size > 8 ? 8U : size);
    return value;
}

bool guards_intact(const Cell& cell, const u32 size) noexcept {
    bool intact = true;
    for (u32 i = 0; i < sizeof(cell.bytes); i = i + 1) {
        const bool operand = i >= operand_offset && i < operand_offset + size;
        if (!operand && cell.bytes[i] != guard) intact = false;
    }
    return intact;
}

// Operand and argument patterns: zero, one, the sign bits of each size, all
// ones, and mixed bits, so carries, borrows and every bit position move.
const u64 patterns[] = {
    0x0000000000000000ULL, 0x0000000000000001ULL, 0x0000000000000080ULL, 0x0000000000008000ULL,
    0x0000000080000000ULL, 0x8000000000000000ULL, 0xffffffffffffffffULL, 0x0123456789abcdefULL,
    0xfedcba9876543210ULL, 0x5555555555555555ULL, 0xaaaaaaaaaaaaaaaaULL, 0x00000000000000ffULL,
};
constexpr u32 pattern_count = sizeof(patterns) / sizeof(patterns[0]);

// The bits above a narrow argument: what a caller may leave there.
constexpr u64 dirt = 0xd1d1d1d1d1d1d1d1ULL;

void set_memo(const u8 mode) noexcept {
    stxlib_outline_atomics_memo = mode;
}

// One call in `mode` with the contract checked; returns the registers after.
Registers checked_call(const HelperEntry& entry, const u8 mode, const Registers& before, const u8 detected) {
    set_memo(mode);
    Registers after = before;
    call_helper(entry.address, after);
    CHECK(contract_kept(before, after, entry.kind));
    if (mode == memo_undetected) CHECK(stxlib_outline_atomics_memo == detected);
    if (!contract_kept(before, after, entry.kind)) std::fprintf(stderr, "contract broken by %s\n", entry.name);
    return after;
}

void check_fetch_op(const HelperEntry& entry, const u8 mode, const u8 detected) {
    const u64 mask = mask_of(entry.size);
    for (u32 i = 0; i < pattern_count; i = i + 1) {
        for (u32 j = 0; j < pattern_count; j = j + 1) {
            const u64 old = patterns[i] & mask;
            const u64 argument = (patterns[j] & mask) | (dirt & ~mask);
            Cell cell = cell_holding(old, 0, entry.size);
            Registers before = sentinel_registers();
            before.x[0] = argument;
            before.x[1] = reinterpret_cast<u64>(cell.bytes + operand_offset);
            const Registers after = checked_call(entry, mode, before, detected);
            CHECK((after.x[0] & mask) == old);
            CHECK(cell_word(cell, 0, entry.size) == reference_store(entry.kind, old, argument, mask));
            CHECK(guards_intact(cell, entry.size));
        }
    }
}

void check_cas(const HelperEntry& entry, const u8 mode, const u8 detected) {
    const u64 mask = mask_of(entry.size);
    for (u32 i = 0; i < pattern_count; i = i + 1) {
        for (u32 j = 0; j < pattern_count; j = j + 1) {
            const u64 old = patterns[i] & mask;
            const u64 desired = (patterns[j] & mask) | (dirt & ~mask);
            // Equal in the low bytes (other bits above them), then different.
            const u64 expectations[2] = {old | (dirt & ~mask), ((old ^ 1ULL) & mask) | (dirt & ~mask)};
            for (u32 e = 0; e < 2; e = e + 1) {
                Cell cell = cell_holding(old, 0, entry.size);
                Registers before = sentinel_registers();
                before.x[0] = expectations[e];
                before.x[1] = desired;
                before.x[2] = reinterpret_cast<u64>(cell.bytes + operand_offset);
                const Registers after = checked_call(entry, mode, before, detected);
                CHECK((after.x[0] & mask) == old);
                const u64 stored = e == 0 ? desired & mask : old;
                CHECK(cell_word(cell, 0, entry.size) == stored);
                CHECK(guards_intact(cell, entry.size));
            }
        }
    }
}

void check_cas16(const HelperEntry& entry, const u8 mode, const u8 detected) {
    for (u32 i = 0; i < pattern_count; i = i + 1) {
        for (u32 j = 0; j < pattern_count; j = j + 1) {
            const u64 old_low = patterns[i];
            const u64 old_high = patterns[(i + 5) % pattern_count];
            const u64 desired_low = patterns[j];
            const u64 desired_high = patterns[(j + 3) % pattern_count];
            // Both halves equal; the low half different; the high half different.
            const u64 expected_low[3] = {old_low, old_low ^ 1ULL, old_low};
            const u64 expected_high[3] = {old_high, old_high, old_high ^ 0x100ULL};
            for (u32 e = 0; e < 3; e = e + 1) {
                Cell cell = cell_holding(old_low, old_high, 16);
                Registers before = sentinel_registers();
                before.x[0] = expected_low[e];
                before.x[1] = expected_high[e];
                before.x[2] = desired_low;
                before.x[3] = desired_high;
                before.x[4] = reinterpret_cast<u64>(cell.bytes + operand_offset);
                const Registers after = checked_call(entry, mode, before, detected);
                CHECK(after.x[0] == old_low);
                CHECK(after.x[1] == old_high);
                CHECK(cell_word(cell, 0, 8) == (e == 0 ? desired_low : old_low));
                CHECK(cell_word(cell, 8, 8) == (e == 0 ? desired_high : old_high));
                CHECK(guards_intact(cell, 16));
            }
        }
    }
}

void check_every_helper(const u8 mode, const u8 detected) {
    for (const HelperEntry& entry : helper_table) {
        switch (entry.kind) {
        case Kind::cas:
            check_cas(entry, mode, detected);
            break;
        case Kind::cas16:
            check_cas16(entry, mode, detected);
            break;
        case Kind::swp:
        case Kind::add:
        case Kind::clr:
        case Kind::eor:
        case Kind::set:
            check_fetch_op(entry, mode, detected);
            break;
        }
    }
    set_memo(detected);
}

Helper helper_named(const char* const name) noexcept {
    Helper found = nullptr;
    for (const HelperEntry& entry : helper_table) {
        if (std::strcmp(entry.name, name) == 0) found = entry.address;
    }
    return found;
}

// ---- the stress run -----------------------------------------------------

constexpr u32 stress_threads = 4;
constexpr u32 stress_rounds = 20000;

struct StressShared final {
    alignas(64) u64 count64;
    alignas(64) u32 count32;
    alignas(64) u64 bits;
    alignas(64) u8 count8;
    alignas(64) u16 xor16;
    alignas(64) u32 swap32;
    alignas(64) u64 pair[2]; // a 16-byte counter: low n, high ~n
};

struct StressHelpers final {
    Helper ldadd8;
    Helper cas4;
    Helper ldset8;
    Helper ldclr8;
    Helper ldadd1;
    Helper ldeor2;
    Helper swp4;
    Helper cas16;
};

struct StressResult final {
    u64 failures;
    u64 swapped_in;
    u64 swapped_out;
};

u64 call1(const Helper helper, const u64 a0, void* const a1) noexcept {
    Registers registers = sentinel_registers();
    registers.x[0] = a0;
    registers.x[1] = reinterpret_cast<u64>(a1);
    call_helper(helper, registers);
    return registers.x[0];
}

u64 call_cas(const Helper helper, const u64 expected, const u64 desired, void* const address) noexcept {
    Registers registers = sentinel_registers();
    registers.x[0] = expected;
    registers.x[1] = desired;
    registers.x[2] = reinterpret_cast<u64>(address);
    call_helper(helper, registers);
    return registers.x[0];
}

void stress_worker(StressShared* const shared, const StressHelpers* const helpers, const u32 thread,
                   StressResult* const result) noexcept {
    const u64 bit = 1ULL << thread;
    const u64 xor_pattern = 0x1111ULL * (thread + 1U);
    StressResult local{};
    for (u32 round = 0; round < stress_rounds; round = round + 1) {
        call1((*helpers).ldadd8, 1, &(*shared).count64);

        u64 guess = 0;
        for (;;) {
            const u64 old = call_cas((*helpers).cas4, guess, (guess + 1U) & 0xffffffffULL, &(*shared).count32);
            if (old == guess) break;
            guess = old;
        }

        const u64 before_set = call1((*helpers).ldset8, bit, &(*shared).bits);
        if ((before_set & bit) != 0) local.failures = local.failures + 1;
        const u64 before_clear = call1((*helpers).ldclr8, bit, &(*shared).bits);
        if ((before_clear & bit) == 0) local.failures = local.failures + 1;

        call1((*helpers).ldadd1, 1, &(*shared).count8);
        call1((*helpers).ldeor2, xor_pattern, &(*shared).xor16);
        call1((*helpers).ldeor2, xor_pattern, &(*shared).xor16);

        const u64 value = (static_cast<u64>(thread + 1U) << 24U) | (round + 1U);
        local.swapped_in = local.swapped_in + value;
        local.swapped_out = local.swapped_out + call1((*helpers).swp4, value, &(*shared).swap32);

        u64 low = 0;
        u64 high = ~0ULL;
        for (;;) {
            Registers registers = sentinel_registers();
            registers.x[0] = low;
            registers.x[1] = high;
            registers.x[2] = low + 1U;
            registers.x[3] = ~(low + 1U);
            registers.x[4] = reinterpret_cast<u64>(&(*shared).pair[0]);
            call_helper((*helpers).cas16, registers);
            // Every value read, matched or not, is a whole counter.
            if (registers.x[1] != ~registers.x[0]) local.failures = local.failures + 1;
            if (registers.x[0] == low && registers.x[1] == high) break;
            low = registers.x[0];
            high = registers.x[1];
        }
    }
    *result = local;
}

void stress(const u8 mode, const u8 detected) {
    const StressHelpers helpers{
        .ldadd8 = helper_named("aarch64_ldadd8_acq_rel"),
        .cas4 = helper_named("aarch64_cas4_acq_rel"),
        .ldset8 = helper_named("aarch64_ldset8_acq"),
        .ldclr8 = helper_named("aarch64_ldclr8_rel"),
        .ldadd1 = helper_named("aarch64_ldadd1_relax"),
        .ldeor2 = helper_named("aarch64_ldeor2_acq_rel"),
        .swp4 = helper_named("aarch64_swp4_acq_rel"),
        .cas16 = helper_named("aarch64_cas16_acq_rel"),
    };
    StressShared shared{};
    shared.pair[0] = 0;
    shared.pair[1] = ~0ULL;
    StressResult results[stress_threads]{};
    set_memo(mode);
    std::vector<std::thread> workers;
    for (u32 t = 0; t < stress_threads; t = t + 1) {
        workers.emplace_back(stress_worker, &shared, &helpers, t, &results[t]);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    CHECK(stxlib_outline_atomics_memo == (mode == memo_undetected ? detected : mode));
    const u64 total = static_cast<u64>(stress_threads) * stress_rounds;
    u64 failures = 0;
    u64 swapped_in = 0;
    u64 swapped_out = 0;
    for (u32 t = 0; t < stress_threads; t = t + 1) {
        failures = failures + results[t].failures;
        swapped_in = swapped_in + results[t].swapped_in;
        swapped_out = swapped_out + results[t].swapped_out;
    }
    CHECK(failures == 0);
    CHECK(shared.count64 == total);
    CHECK(shared.count32 == static_cast<u32>(total));
    CHECK(shared.bits == 0);
    CHECK(shared.count8 == static_cast<u8>(total & 0xffU));
    CHECK(shared.xor16 == 0);
    // Every value swapped in is either swapped out by a later swap or left
    // in place at the end; the first swap takes out the initial 0.
    CHECK(swapped_in == swapped_out + shared.swap32);
    CHECK(shared.pair[0] == total);
    CHECK(shared.pair[1] == ~total);
    std::printf("outline atomics: stress in mode %u, %u threads x %u rounds, %llu failures\n",
                static_cast<unsigned>(mode), static_cast<unsigned>(stress_threads),
                static_cast<unsigned>(stress_rounds), static_cast<unsigned long long>(failures));
    set_memo(detected);
}

void outline_atomics() {
    CHECK(sizeof(helper_table) / sizeof(helper_table[0]) == 100);
    set_memo(memo_undetected);
    const bool lse = stxlib_outline_atomics_lse();
    const u8 detected = lse ? memo_lse : memo_loop;
    CHECK(stxlib_outline_atomics_memo == detected);
    CHECK(simd::detected_features().lse == lse);
    std::printf("outline atomics: lse=%d (the LSE path is %s)\n", lse ? 1 : 0, lse ? "tested" : "not tested here");

    check_every_helper(memo_loop, detected);
    if (lse) check_every_helper(memo_lse, detected);
    check_every_helper(memo_undetected, detected);

    stress(memo_undetected, detected);
    stress(memo_loop, detected);
    if (lse) stress(memo_lse, detected);
}

#else

void outline_atomics() {
    const simd::Features features = simd::detected_features();
#if defined(__aarch64__) && defined(__APPLE__)
    CHECK(features.lse);
#elif !defined(__aarch64__) && !defined(_M_ARM64)
    CHECK(!features.lse);
#endif
    std::printf("outline atomics: not an outline-atomics host; lse=%d\n", features.lse ? 1 : 0);
}

#endif

} // namespace

int main() {
    outline_atomics();
    return stxlib_tests::finish("stxlib outline atomics");
}
