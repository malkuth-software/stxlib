#pragma once

// STXlib's integer and byte vocabulary.
//
// Public headers under include/stxlib/ include no standard and no platform
// header (AGENTS.md, "Hard boundaries"; tools/check_public_headers.py checks
// it). These names are therefore spelled with built-in types and the
// compiler's predefined type macros, and their widths are checked here on
// every target. Clang and GCC define __SIZE_TYPE__, __UINTPTR_TYPE__ and
// __CHAR_BIT__; no other compiler is supported.

namespace stxlib {

static_assert(__CHAR_BIT__ == 8, "STXlib assumes 8-bit bytes");

// One byte of storage or of a foreign byte stream, with no meaning attached.
using Byte = unsigned char;

using u8 = unsigned char;
using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using i8 = signed char;
using i16 = short;
using i32 = int;
using i64 = long long;

// The type of sizeof on this target: object sizes and element counts.
using usize = __SIZE_TYPE__;
// An unsigned integer that holds an address: only at allocation, borrowing
// and foreign-call boundaries (house style, section 1).
using uptr = __UINTPTR_TYPE__;

static_assert(sizeof(u16) == 2 && sizeof(i16) == 2, "16-bit integers");
static_assert(sizeof(u32) == 4 && sizeof(i32) == 4, "32-bit integers");
static_assert(sizeof(u64) == 8 && sizeof(i64) == 8, "64-bit integers");
static_assert(sizeof(usize) == sizeof(void*), "usize holds an object size of this address width");
static_assert(sizeof(uptr) == sizeof(void*), "uptr holds an address");
// Phase I targets are 64-bit (macOS arm64, Linux x86-64 and arm64, Windows
// x86-64, the Raspberry Pi 4 in arm64 mode). TrampolineHandle packs a slot
// and a generation into a context pointer and relies on it.
static_assert(sizeof(void*) == 8, "STXlib phase I targets are 64-bit");

inline constexpr usize usize_max = static_cast<usize>(~static_cast<usize>(0));
inline constexpr u64 u64_max = ~static_cast<u64>(0);
inline constexpr u32 u32_max = ~static_cast<u32>(0);
inline constexpr u16 u16_max = static_cast<u16>(~static_cast<u16>(0));
inline constexpr i64 i64_max = 0x7fffffffffffffffLL;
inline constexpr i64 i64_min = -i64_max - 1;

} // namespace stxlib
