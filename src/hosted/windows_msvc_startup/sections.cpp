// The initializer tables and the image kind.
//
// Learn "CRT initialization": the compiler places a pointer to each C++
// dynamic initializer in .CRT$XCU; the linker sorts the .CRT subsections
// by the name after the '$', so a sentinel in .CRT$XCA comes before every
// initializer and one in .CRT$XCZ after them; the startup code walks the
// pointers between. C initializers (.CRT$XIA..XIZ) return an int, and a
// nonzero value refuses the start. The walking itself is the UCRT's
// _initterm and _initterm_e (corecrt_startup.h), which skip null entries:
// the linker may pad between contributions with zeros.
//
// The sentinels are one null pointer each, const, so that they share the
// read-only characteristics of the compiler's own .CRT$XCU entries.

#include "hosted/windows_msvc_startup/startup.hpp"

extern "C" {

__attribute__((used, section(".CRT$XIA"))) extern const _PIFV __xi_a[1] = {nullptr};
__attribute__((used, section(".CRT$XIZ"))) extern const _PIFV __xi_z[1] = {nullptr};
__attribute__((used, section(".CRT$XCA"))) extern const _PVFV __xc_a[1] = {nullptr};
__attribute__((used, section(".CRT$XCZ"))) extern const _PVFV __xc_z[1] = {nullptr};

// The marker the compiler references from every object that uses floating
// point (Learn's "CRT initialization" does not name it; the study's
// section 5). Its value is conventional and unused.
extern const int _fltused = 0x9875;

}  // extern "C"

namespace stxlib::msvc_startup {

namespace {

// Written once by the entry point, before any initializer, on the loading
// thread; read afterwards only.
constinit int current_image_kind{static_cast<int>(ImageKind::program)};

}  // namespace

void set_image_kind(const ImageKind kind) noexcept {
    current_image_kind = static_cast<int>(kind);
}

ImageKind image_kind() noexcept {
    const ImageKind kind = static_cast<ImageKind>(current_image_kind);
    return kind;
}

int run_c_initializers() noexcept {
    // _initterm_e takes the table's bounds as writable pointers; it only
    // reads them.
    _PIFV* const first = const_cast<_PIFV*>(__xi_a);
    _PIFV* const last = const_cast<_PIFV*>(__xi_z);
    const int status = _initterm_e(first, last);
    return status;
}

void run_cxx_initializers() noexcept {
    _PVFV* const first = const_cast<_PVFV*>(__xc_a);
    _PVFV* const last = const_cast<_PVFV*>(__xc_z);
    _initterm(first, last);
}

}  // namespace stxlib::msvc_startup
