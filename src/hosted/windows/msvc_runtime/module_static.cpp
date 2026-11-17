// The per-module part of the runtime that cannot come from a DLL: the
// type_info vtable. Every TypeDescriptor clang emits holds the address of
// ??_7type_info@@6B@ in its initialiser (MicrosoftCXXABI.cpp,
// getTypeInfoVTable), an address constant that an import cannot supply on
// this target, so each image carries its own copy, as the Microsoft startup
// objects do (the EH runtime study, section 2.2).
//
// Layout (the Microsoft ABI's vftable): the symbol names the first virtual
// slot; the slot before it is the RTTI locator, null here because nothing
// asks type_info for its own type. The one slot is type_info's scalar
// deleting destructor, which no program reaches: TypeDescriptors are
// static data, never deleted. It returns its object and frees nothing.

#include "stxlib/types.hpp"

extern "C" void* stxlib_msvc_type_info_destructor(void* const self, const unsigned flags) {
    static_cast<void>(flags);
    return self;
}

__asm__(".section .rdata,\"dr\"\n"
        ".p2align 3\n"
        "stxlib_msvc_type_info_locator:\n"
        "    .quad 0\n"
        ".globl \"??_7type_info@@6B@\"\n"
        "\"??_7type_info@@6B@\":\n"
        "    .quad stxlib_msvc_type_info_destructor\n"
        ".text\n");
