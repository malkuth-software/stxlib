// The image's load configuration directory and the Control Flow Guard
// pointers.
//
// The linker writes the load configuration data directory from the symbol
// _load_config_used (Learn "PE Format", "The Load Configuration
// Structure (Image Only)"; winnt.h IMAGE_LOAD_CONFIG_DIRECTORY64). Two of
// its fields matter here:
// - SecurityCookie, the address of __security_cookie, which lets the
//   loader choose the cookie (security_cookie.cpp);
// - the Control Flow Guard fields. With -cfguard, clang calls through
//   __guard_check_icall_fptr before each indirect call and lld, under
//   /guard:cf, emits the tables of valid targets and defines the
//   synthetic symbols named below (__guard_fids_table, __guard_flags and
//   the rest). The loader replaces the two function pointers with the
//   system's checks when the image is guarded (Learn "Control Flow Guard
//   for platform security"); until then, and on a system without CFG,
//   they point at stubs that check nothing: the check returns and the
//   dispatch jumps to the target in rax.
//
// The structure is written in assembly: GuardFlags is a 32-bit field
// holding an absolute symbol's value, which a C++ constant initializer
// cannot express without an integer cast the compiler would turn into
// dynamic initialization, and the loader reads the directory before any
// initializer runs. The counts and flags are absolute symbols, so the
// linker emits no base relocation for them; the tables and pointers are
// VAs and are relocated. The pointers live in .00cfg, the section the
// loader expects to patch (unconfirmed against a Microsoft page; checked
// on the built probe with llvm-readobj --coff-load-config).
//
// The layout follows winnt.h up to GuardEHContinuationCount (280 bytes);
// Size says how much the loader may read, and the later fields (XFG, cast
// guard, memcpy guard, UMA) are absent. The offsets are checked against
// winnt.h below.

#include "hosted/windows_msvc_startup/startup.hpp"

#include <stddef.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, SecurityCookie) == 88, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardCFCheckFunctionPointer) == 112, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardFlags) == 144, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardAddressTakenIatEntryTable) == 160, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardLongJumpTargetTable) == 176, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DynamicValueRelocTable) == 192, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardEHContinuationTable) == 264, "load configuration layout");
static_assert(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, GuardEHContinuationCount) == 272, "load configuration layout");

__asm__(
    // The stubs the guard pointers hold until the loader replaces them.
    "\t.text\n"
    "\t.p2align 4\n"
    "stx_msvc_startup_guard_check_nothing:\n"
    "\tret\n"
    "\t.p2align 4\n"
    "stx_msvc_startup_guard_dispatch_unchecked:\n"
    "\tjmp *%rax\n"

    "\t.section .00cfg,\"dr\"\n"
    "\t.p2align 3\n"
    "\t.globl __guard_check_icall_fptr\n"
    "__guard_check_icall_fptr:\n"
    "\t.quad stx_msvc_startup_guard_check_nothing\n"
    "\t.globl __guard_dispatch_icall_fptr\n"
    "__guard_dispatch_icall_fptr:\n"
    "\t.quad stx_msvc_startup_guard_dispatch_unchecked\n"

    "\t.section .rdata,\"dr\"\n"
    "\t.p2align 3\n"
    "\t.globl _load_config_used\n"
    "_load_config_used:\n"
    "\t.long 280\n"                         // 0 Size
    "\t.long 0\n"                           // 4 TimeDateStamp
    "\t.short 0, 0\n"                       // 8 MajorVersion, MinorVersion
    "\t.long 0, 0, 0\n"                     // 12 GlobalFlagsClear, GlobalFlagsSet, CriticalSectionDefaultTimeout
    "\t.quad 0, 0, 0, 0, 0, 0\n"            // 24 DeCommit*, LockPrefixTable, MaximumAllocationSize, VirtualMemoryThreshold, ProcessAffinityMask
    "\t.long 0\n"                           // 72 ProcessHeapFlags
    "\t.short 0, 0\n"                       // 76 CSDVersion, DependentLoadFlags
    "\t.quad 0\n"                           // 80 EditList
    "\t.quad __security_cookie\n"           // 88 SecurityCookie
    "\t.quad 0, 0\n"                        // 96 SEHandlerTable, SEHandlerCount (x86 only)
    "\t.quad __guard_check_icall_fptr\n"    // 112 GuardCFCheckFunctionPointer
    "\t.quad __guard_dispatch_icall_fptr\n" // 120 GuardCFDispatchFunctionPointer
    "\t.quad __guard_fids_table\n"          // 128 GuardCFFunctionTable
    "\t.quad __guard_fids_count\n"          // 136 GuardCFFunctionCount
    "\t.long __guard_flags\n"               // 144 GuardFlags
    "\t.short 0, 0\n"                       // 148 CodeIntegrity.Flags, Catalog
    "\t.long 0, 0\n"                        // 152 CodeIntegrity.CatalogOffset, Reserved
    "\t.quad __guard_iat_table\n"           // 160 GuardAddressTakenIatEntryTable
    "\t.quad __guard_iat_count\n"           // 168 GuardAddressTakenIatEntryCount
    "\t.quad __guard_longjmp_table\n"       // 176 GuardLongJumpTargetTable
    "\t.quad __guard_longjmp_count\n"       // 184 GuardLongJumpTargetCount
    "\t.quad 0, 0, 0, 0\n"                  // 192 DynamicValueRelocTable, CHPEMetadataPointer, GuardRFFailureRoutine, ...FunctionPointer
    "\t.long 0\n"                           // 224 DynamicValueRelocTableOffset
    "\t.short 0, 0\n"                       // 228 DynamicValueRelocTableSection, Reserved2
    "\t.quad 0\n"                           // 232 GuardRFVerifyStackPointerFunctionPointer
    "\t.long 0, 0\n"                        // 240 HotPatchTableOffset, Reserved3
    "\t.quad 0, 0\n"                        // 248 EnclaveConfigurationPointer, VolatileMetadataPointer
    "\t.quad __guard_eh_cont_table\n"       // 264 GuardEHContinuationTable
    "\t.quad __guard_eh_cont_count\n"       // 272 GuardEHContinuationCount
    "\t.text\n");
