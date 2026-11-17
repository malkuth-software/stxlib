# STXlib's MSVC C++ runtime (README.md beside this file): the targets only,
# no project of its own. Added by the root build when STXLIB_MSVC_RUNTIME is
# ON (x86_64-pc-windows-msvc only), and by the conformance suite's "ours"
# side (tests/msvc_runtime), so both build the same libraries the same way.
#
# Targets:
#   stxlib_msvc_runtime_static  the runtime proper, linked into one image
#   stxlib_msvc_runtime         the shared form, stxlib_msvc_runtime.dll
#   stxlib_msvc_runtime_module  per image, whichever form it uses: memcpy and
#                               the string group, operator new and delete,
#                               the type_info vtable
#
# The startup shim (src/hosted/windows_msvc_startup, target
# stx_msvc_startup) owns everything else an image needs: the entry points,
# __chkstk, _fltused, thread-safe statics, TLS and the exit tables (the
# per-symbol owners, projects queue claude__windows-msvc-target.md). The
# shared runtime links it for its DLL entry; the shared form is defined only when that
# target exists when this directory is added.
#
# Headers: the sources include only STXlib's own C library and Windows API
# headers and libc++ (system_interim.h); the toolchain gives -nostdinc and
# those include folders.

if(NOT WIN32 OR MINGW OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "the MSVC runtime is built for x86_64-pc-windows-msvc with clang only")
endif()

set(stxlib_msvc_runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(stxlib_msvc_runtime_include "${CMAKE_CURRENT_SOURCE_DIR}/../../../../include" ABSOLUTE)

# The runtime proper, kept free of exceptions and RTTI.
add_library(stxlib_msvc_runtime_core OBJECT
    "${stxlib_msvc_runtime_dir}/runtime_state.cpp"
    "${stxlib_msvc_runtime_dir}/throw.cpp"
    "${stxlib_msvc_runtime_dir}/frame_handler.cpp"
    "${stxlib_msvc_runtime_dir}/terminate.cpp"
    "${stxlib_msvc_runtime_dir}/rtti.cpp"
    "${stxlib_msvc_runtime_dir}/exception_ptr.cpp"
    "${stxlib_msvc_runtime_dir}/structured_handler.cpp"
    "${stxlib_msvc_runtime_dir}/jump.cpp"
    "${stxlib_msvc_runtime_dir}/thunks_x64.S"
    "${stxlib_msvc_runtime_dir}/setjmp_x64.S")
target_include_directories(stxlib_msvc_runtime_core PRIVATE "${stxlib_msvc_runtime_include}")
target_compile_options(stxlib_msvc_runtime_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# The hooks that construct and throw libc++'s std::bad_alloc, bad_cast and
# bad_typeid: one of the two files built with exceptions.
add_library(stxlib_msvc_runtime_hooks OBJECT "${stxlib_msvc_runtime_dir}/cxx_hooks.cpp")
target_include_directories(stxlib_msvc_runtime_hooks PRIVATE "${stxlib_msvc_runtime_include}")
target_compile_options(stxlib_msvc_runtime_hooks PRIVATE -fexceptions -fcxx-exceptions)

add_library(stxlib_msvc_runtime_static STATIC
    $<TARGET_OBJECTS:stxlib_msvc_runtime_core>
    $<TARGET_OBJECTS:stxlib_msvc_runtime_hooks>)

add_library(stxlib_msvc_runtime_module STATIC
    "${stxlib_msvc_runtime_dir}/module_static.cpp"
    "${stxlib_msvc_runtime_dir}/strings.cpp"
    "${stxlib_msvc_runtime_dir}/new_delete.cpp")
target_include_directories(stxlib_msvc_runtime_module PRIVATE "${stxlib_msvc_runtime_include}")
# Source properties are scoped to this directory, where the targets that
# compile these files are defined.
set_source_files_properties("${stxlib_msvc_runtime_dir}/strings.cpp" PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-builtin")
set_source_files_properties("${stxlib_msvc_runtime_dir}/new_delete.cpp" PROPERTIES COMPILE_OPTIONS "-fexceptions;-fcxx-exceptions")

# The shared form takes its entry point, _DllMainCRTStartup, and its exit
# table from the startup; the runtime itself needs no initialisation
# (runtime_state.cpp: constant initialisation, TLS slots on first use).
if(TARGET stx_msvc_startup)
add_library(stxlib_msvc_runtime SHARED
    $<TARGET_OBJECTS:stxlib_msvc_runtime_core>
    $<TARGET_OBJECTS:stxlib_msvc_runtime_hooks>
    "${stxlib_msvc_runtime_dir}/stxlib_msvc_runtime.def")
target_link_libraries(stxlib_msvc_runtime PRIVATE stx_msvc_startup stxlib_msvc_runtime_module)
target_link_options(stxlib_msvc_runtime PRIVATE "LINKER:/ENTRY:_DllMainCRTStartup")
else()
    message(STATUS "MSVC runtime: no stx_msvc_startup target yet, so no shared form (add the startup first)")
endif()
