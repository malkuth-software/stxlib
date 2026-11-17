#pragma once

// STXlib's own C++ exception and RTTI runtime for x86_64-pc-windows-msvc
// (the Windows MSVC target compiled by the pinned clang 22.1.8): the
// records clang writes and this runtime reads.
//
// Clean room. Every layout here is taken from LLVM's emitter, Apache-2.0
// WITH LLVM-exception, release/22.x at llvmorg-22.1.8:
// - llvm/lib/CodeGen/AsmPrinter/WinException.cpp, emitCXXFrameHandler3Table
//   (FuncInfo, UnwindMapEntry, TryBlockMapEntry, HandlerType,
//   IPToStateMapEntry) and computeIP2StateTable;
// - clang/lib/CodeGen/MicrosoftCXXABI.cpp, getThrowInfoType,
//   getCatchableTypeType, getCatchableTypeArrayType, getThrowInfo,
//   getCatchableType, getAddrOfCXXCatchHandlerType, getCatchAllTypeInfo,
//   MSRTTIClass, MSRTTIBuilder (the RTTI descriptors), emitThrow;
// - llvm/lib/CodeGen/WinEHPrepare.cpp, calculateCXXStateNumbers (how states
//   nest), and llvm/lib/Target/X86/X86FrameLowering.cpp,
//   getWinEHParentFrameOffset and adjustFrameForMsvcCxxEh (UnwindHelp).
// The operating system's side (RUNTIME_FUNCTION, DISPATCHER_CONTEXT,
// RtlUnwindEx, the handler prototype) is Microsoft Learn's "x64 exception
// handling" and the SDK's winnt.h. No Microsoft runtime header or source,
// and no Wine or ReactOS code, was read (REFERENCES.md beside this file).
//
// On x64 every reference inside these records is a 32-bit offset from the
// base of the image that holds the record ("image-relative"); zero means
// none. Records thrown from another module carry that module's base in
// the exception record (throw.cpp).

#include "stxlib/types.hpp"

namespace stxlib::msvc_runtime {

// --- the image-relative reference ---------------------------------------
// A 32-bit offset from an image base. Kept apart from addresses and from
// plain counts, since mixing them is the classic error here (house style
// section 3).
struct ImageOffset final {
    u32 value;
};

[[nodiscard]] inline const Byte* resolve(const uptr image_base, const ImageOffset offset) noexcept {
    if (offset.value == 0) return nullptr;
    const uptr address = image_base + static_cast<uptr>(offset.value);
    const Byte* const result = reinterpret_cast<const Byte*>(address);
    return result;
}

// --- the exception record a C++ throw raises (throw.cpp) ------------------
// RaiseException's code 0xE06D7363 ('msc' with the customer bit set), four
// parameters on x64: the magic number, the exception object, the ThrowInfo
// and the base of the image holding the ThrowInfo. The code and the first
// three parameters are described by Raymond Chen, "Decoding the parameters
// of a thrown C++ exception (0xE06D7363)", The Old New Thing, 2010-07-30;
// the fourth (the image base) and the magic's later values are general
// knowledge, confirmed by this runtime's conformance suite against the
// Microsoft runtime (tests/msvc_runtime).
inline constexpr u32 cxx_exception_code = 0xE06D7363u;
inline constexpr u64 cxx_exception_magic = 0x19930520u;
inline constexpr u64 cxx_exception_magic_newest = 0x19930522u;
inline constexpr u32 cxx_exception_parameters = 4;

// --- the throw-side records (MicrosoftCXXABI.cpp, getThrowInfo) -----------
// ThrowInfo attributes: qualifiers of a thrown pointer's pointee, which a
// handler must match or exceed.
inline constexpr u32 throw_is_const = 1;
inline constexpr u32 throw_is_volatile = 2;
inline constexpr u32 throw_is_unaligned = 4;

struct ThrowInfo final {
    u32 attributes;
    ImageOffset cleanup;                 // the destructor, called on the object; none if trivial
    ImageOffset forward_compatibility;   // always none
    ImageOffset catchable_type_array;
};

// CatchableType properties (getCatchableType): bit 1 scalar (not a class),
// bit 4 the class has virtual bases (its copy constructor takes the extra
// "most derived" argument), bit 16 std::bad_alloc. Bit 2, "by reference
// only", is never set by clang ("FIXME: Figure out how bits 2 or 8 can get
// set"); it is honoured anyway.
inline constexpr u32 catchable_is_scalar = 1;
inline constexpr u32 catchable_by_reference_only = 2;
inline constexpr u32 catchable_has_virtual_bases = 4;

// The member displacement of a base within a derived object (PMD): mdisp
// within the subobject; pdisp, when not negative, the offset of the vbptr
// whose table entry at vdisp gives a virtual base's offset from the vbptr.
struct Displacement final {
    i32 member;            // mdisp
    i32 vbptr;             // pdisp, -1 for a non-virtual path
    i32 vbtable_entry;     // vdisp, a byte offset into the vbtable
};

struct CatchableType final {
    u32 properties;
    ImageOffset type;                    // TypeDescriptor
    Displacement displacement;
    i32 size;
    ImageOffset copy_constructor;        // none for scalars and trivially copied classes
};

// The catchable types: the exact type first (getCatchableTypeArray inserts
// the most derived class, then its unambiguous public bases, then the type
// itself, which a SetVector folds into the first entry), then void* for
// object pointers.
struct CatchableTypeArray final {
    i32 count;
    ImageOffset types[1];                // `count` entries
};

// The TypeDescriptor (getAddrOfRTTIDescriptor): the type_info vtable, a
// pointer the runtime may use, and the decorated name, ".?AV...@@" and so
// on. Its layout is libc++'s std::type_info under _LIBCPP_NO_VCRUNTIME
// (libcxx/include/typeinfo: vtable, __undecorated_name, __decorated_name).
struct TypeDescriptor final {
    const void* vtable;
    void* spare;
    char name[1];                        // NUL-terminated
};

// --- the frame tables (WinException.cpp, emitCXXFrameHandler3Table) -------
inline constexpr u32 function_magic_oldest = 0x19930520u;
inline constexpr u32 function_magic_newest = 0x19930522u;

// FuncInfo flags. Bit 1: synchronous exceptions only (clang writes it
// unless the module has "eh-asynch", -fasync-exceptions); catch(...) then
// never catches a non-C++ exception. Bit 4: the function is noexcept and
// an exception may not leave it (clang 22 never writes it; it terminates
// through its own __std_terminate cleanup instead).
inline constexpr i32 function_synchronous_only = 1;
inline constexpr i32 function_noexcept = 4;

struct FunctionInfo final {
    u32 magic;
    i32 state_count;                     // MaxState: entries in the unwind map
    ImageOffset unwind_map;
    u32 try_block_count;
    ImageOffset try_block_map;
    u32 ip_map_count;
    ImageOffset ip_map;
    i32 unwind_help;                     // offset of the 8-byte UnwindHelp slot from the establisher frame
    ImageOffset exception_specifications; // always none from clang
    i32 flags;
};

struct UnwindMapEntry final {
    i32 to_state;                        // the enclosing state
    ImageOffset action;                  // a cleanup funclet, or none
};

// Handler adjectives (getAddrOfCXXCatchHandlerType, getCatchAllTypeInfo):
// the handler's own qualifiers, a reference, and "catch(...) catches only
// C++ exceptions" (0x40, written unless -EHa).
inline constexpr i32 handler_is_const = 1;
inline constexpr i32 handler_is_volatile = 2;
inline constexpr i32 handler_is_unaligned = 4;
inline constexpr i32 handler_is_reference = 8;
inline constexpr i32 handler_catch_all_cxx_only = 0x40;

struct HandlerType final {
    i32 adjectives;
    ImageOffset type;                    // none for catch(...)
    i32 catch_object_offset;             // from the parent's establisher frame; 0: no catch object
    ImageOffset handler;                 // the catch funclet
    i32 parent_frame_offset;             // in a funclet frame, where the parent's establisher frame is homed
};

// States: the try body is [low, high]; every handler of the block shares
// the one catch state high + 1 (calculateCXXStateNumbers' CatchLow); states
// nested inside the handlers run up to catch_high. On x64 the map lists
// blocks outer first (IsPreOrder), so the runtime orders candidates itself.
struct TryBlockMapEntry final {
    i32 try_low;
    i32 try_high;
    i32 catch_high;
    i32 handler_count;
    ImageOffset handlers;
};

// "PCs after `ip` have this state" (computeIP2StateTable): sorted within
// each funclet, the parent first.
struct IpStateEntry final {
    ImageOffset ip;
    i32 state;
};

inline constexpr i32 empty_state = -1;
// What UnwindHelp holds when no catch of the frame is active: the parent's
// prologue stores -2 (adjustFrameForMsvcCxxEh). This runtime keeps the
// frame's state there while one of its catches runs (frame_handler.cpp).
inline constexpr i64 no_state_override = -2;

// --- the RTTI descriptors (MSRTTIBuilder) ----------------------------------
inline constexpr i32 locator_image_relative = 1;

struct CompleteObjectLocator final {
    i32 signature;                       // 1: image-relative (x64)
    i32 offset;                          // of this vfptr from the complete object
    i32 constructor_displacement;        // vtordisp's offset before the vfptr, or 0
    ImageOffset type;                    // the complete object's TypeDescriptor
    ImageOffset hierarchy;
    ImageOffset self;                    // this locator, for finding the image base
};

// Hierarchy attributes: bit 1 multiple inheritance, bit 2 virtual
// inheritance on a branching path, bit 4 ambiguous bases.
struct ClassHierarchyDescriptor final {
    i32 signature;
    i32 attributes;
    i32 base_count;                      // entries in the base array, the class itself first
    ImageOffset base_array;              // `base_count` ImageOffsets to BaseClassDescriptors
};

// Base attributes (MSRTTIClass): private on the path from the complete
// class (1 | 8), ambiguous (2), this base's own specifier private (4),
// virtual (16), has a hierarchy descriptor (64).
inline constexpr i32 base_private_on_path = 1 | 8;
inline constexpr i32 base_ambiguous = 2;
inline constexpr i32 base_private = 4;

// The base array lists the hierarchy in pre-order: each descriptor is
// followed by the `contained_bases` descriptors of its own bases.
struct BaseClassDescriptor final {
    ImageOffset type;
    i32 contained_bases;
    Displacement displacement;
    i32 attributes;
    ImageOffset hierarchy;
};

static_assert(sizeof(ThrowInfo) == 16);
static_assert(sizeof(CatchableType) == 28);
static_assert(sizeof(FunctionInfo) == 40);
static_assert(sizeof(UnwindMapEntry) == 8);
static_assert(sizeof(HandlerType) == 20);
static_assert(sizeof(TryBlockMapEntry) == 20);
static_assert(sizeof(IpStateEntry) == 8);
static_assert(sizeof(CompleteObjectLocator) == 24);
static_assert(sizeof(ClassHierarchyDescriptor) == 16);
static_assert(sizeof(BaseClassDescriptor) == 28);

// --- shared operations ------------------------------------------------------

// The address of the base described by `displacement` within the object at
// `object` (the PMD rule): a virtual path reads the object's vbtable.
[[nodiscard]] inline Byte* displace(Byte* const object, const Displacement displacement) noexcept {
    Byte* result = object;
    if (displacement.vbptr >= 0) {
        Byte* const vbptr_address = object + displacement.vbptr;
        const Byte* const vbtable = *reinterpret_cast<const Byte* const*>(vbptr_address);
        const i32 virtual_base_offset = *reinterpret_cast<const i32*>(vbtable + displacement.vbtable_entry);
        result = vbptr_address + virtual_base_offset;
    }
    result = result + displacement.member;
    return result;
}

// Two TypeDescriptors name one type when they are the same record or carry
// the same decorated name: each image has its own copy of a type's record,
// so a type thrown from one module and caught in another meets by name.
[[nodiscard]] inline bool same_type(const TypeDescriptor& left, const TypeDescriptor& right) noexcept {
    if (&left == &right) return true;
    const char* a = left.name;
    const char* b = right.name;
    while (*a != '\0' && *a == *b) {
        a = a + 1;
        b = b + 1;
    }
    const bool result = *a == *b;
    return result;
}

} // namespace stxlib::msvc_runtime
