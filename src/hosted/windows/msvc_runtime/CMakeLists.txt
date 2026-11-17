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
#                               __chkstk, thread-safe statics
#   stxlib_msvc_image           per image linked without Microsoft's startup
#                               objects: the TLS directory and _fltused
#
# The per-image pieces overlap the startup shim's (src/hosted/
# windows_msvc_startup: new_delete.cpp, chkstk.cpp, thread_safe_statics.cpp,
# tls.cpp, entry_dll.cpp). An image links one set or the other, never both;
# which set the default keeps is decided when the two are proven together
# on Windows (README.md, "Before it can be the default").

if(NOT WIN32 OR MINGW OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "the MSVC runtime is built for x86_64-pc-windows-msvc with clang only")
endif()

set(stxlib_msvc_runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}")
get_filename_component(stxlib_msvc_runtime_include "${CMAKE_CURRENT_SOURCE_DIR}/../../../../include" ABSOLUTE)

# Every Microsoft C++ runtime library is dropped (the startup objects'
# msvcrt.lib and libcmt.lib, vcruntime and the STL's runtime); the UCRT
# DLL's import library stays, the one system C library. Read by the images
# that link this runtime.
set(stxlib_msvc_no_microsoft_runtime_local
    "LINKER:/NODEFAULTLIB:msvcrt.lib"
    "LINKER:/NODEFAULTLIB:libcmt.lib"
    "LINKER:/NODEFAULTLIB:vcruntime.lib"
    "LINKER:/NODEFAULTLIB:libvcruntime.lib"
    "LINKER:/NODEFAULTLIB:msvcprt.lib"
    "LINKER:/NODEFAULTLIB:libcpmt.lib"
    "LINKER:/NODEFAULTLIB:libucrt.lib")
set(stxlib_msvc_no_microsoft_runtime ${stxlib_msvc_no_microsoft_runtime_local} PARENT_SCOPE)

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
    "${stxlib_msvc_runtime_dir}/chkstk_x64.S"
    "${stxlib_msvc_runtime_dir}/thread_statics.cpp"
    "${stxlib_msvc_runtime_dir}/new_delete.cpp")
target_include_directories(stxlib_msvc_runtime_module PRIVATE "${stxlib_msvc_runtime_include}")
# Source properties are scoped to this directory, where the targets that
# compile these files are defined.
set_source_files_properties("${stxlib_msvc_runtime_dir}/strings.cpp" PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-builtin")
set_source_files_properties("${stxlib_msvc_runtime_dir}/new_delete.cpp" PROPERTIES COMPILE_OPTIONS "-fexceptions;-fcxx-exceptions")

add_library(stxlib_msvc_image OBJECT "${stxlib_msvc_runtime_dir}/image_support.c")

add_library(stxlib_msvc_runtime SHARED
    $<TARGET_OBJECTS:stxlib_msvc_runtime_core>
    $<TARGET_OBJECTS:stxlib_msvc_runtime_hooks>
    $<TARGET_OBJECTS:stxlib_msvc_image>
    "${stxlib_msvc_runtime_dir}/dll_entry.cpp"
    "${stxlib_msvc_runtime_dir}/stxlib_msvc_runtime.def")
target_include_directories(stxlib_msvc_runtime PRIVATE "${stxlib_msvc_runtime_include}")
target_link_libraries(stxlib_msvc_runtime PRIVATE stxlib_msvc_runtime_module -lucrt)
target_link_options(stxlib_msvc_runtime PRIVATE ${stxlib_msvc_no_microsoft_runtime_local}
    "LINKER:/ENTRY:stxlib_msvc_runtime_entry")
