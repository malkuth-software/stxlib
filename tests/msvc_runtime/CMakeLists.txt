# The MSVC C++ runtime conformance suite (src/hosted/windows/msvc_runtime): small
# standalone programs, each built twice from one source, against
# Microsoft's vcruntime (the oracle) and against STXlib's runtime, and
# compared case by case on stdout and exit code by
# tools/msvc_runtime/run_conformance.py.
#
# A standalone project, configured once per side with the GUI.Forms
# x86_64-pc-windows-msvc toolchain file (clang 22 and lld-link, never the
# MSVC compiler):
#   -DSTXLIB_MSVC_RUNTIME_SIDE=oracle  -DGUI_FORMS_LIBCXX_PREFIX=<libc++, vcruntime ABI>
#   -DSTXLIB_MSVC_RUNTIME_SIDE=ours    -DGUI_FORMS_LIBCXX_PREFIX=<libc++, LIBCXX_NO_VCRUNTIME>
# The oracle side links what the toolchain links by default (Microsoft's
# startup objects, vcruntime140.dll, msvcp140.dll for exception_ptr). The
# ours side drops every Microsoft C++ runtime library and links each case
# twice: with the static runtime (case_<name>.exe) and with the shared one,
# stxlib_msvc_runtime.dll (case_<name>_shared.exe).

cmake_minimum_required(VERSION 3.25)
set(CMAKE_ASM_COMPILER_TARGET "x86_64-pc-windows-msvc")
project(stxlib_msvc_runtime_conformance C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(STXLIB_MSVC_RUNTIME_SIDE "oracle" CACHE STRING "oracle (vcruntime) or ours (STXlib's runtime)")
set_property(CACHE STXLIB_MSVC_RUNTIME_SIDE PROPERTY STRINGS oracle ours)

set(suite_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../src/hosted/windows/msvc_runtime")
set(stxlib_include "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

# Single-image cases; cross_dll is added apart below.
set(conformance_cases
    basic
    hierarchy
    nested
    rethrow
    unwind
    destructor_throw
    noexcept_violation
    exception_pointer
    structured
    rtti
    threads
    copy_throw
    unhandled
    standard_exceptions
    objects
    states)

set(case_options -fexceptions -fcxx-exceptions -frtti -Wno-unused-variable -Wno-unreachable-code)

if(STXLIB_MSVC_RUNTIME_SIDE STREQUAL "oracle")
    foreach(case_name IN LISTS conformance_cases)
        add_executable(case_${case_name} "${suite_dir}/cases/${case_name}.cpp")
        target_compile_options(case_${case_name} PRIVATE ${case_options})
    endforeach()
    add_library(cross_dll_library SHARED "${suite_dir}/cases/cross_dll_library.cpp")
    target_compile_options(cross_dll_library PRIVATE ${case_options})
    add_executable(case_cross_dll "${suite_dir}/cases/cross_dll.cpp")
    target_compile_options(case_cross_dll PRIVATE ${case_options})
    target_link_libraries(case_cross_dll PRIVATE cross_dll_library)
    return()
endif()

if(NOT STXLIB_MSVC_RUNTIME_SIDE STREQUAL "ours")
    message(FATAL_ERROR "STXLIB_MSVC_RUNTIME_SIDE is oracle or ours, not ${STXLIB_MSVC_RUNTIME_SIDE}")
endif()

# --- STXlib's runtime -----------------------------------------------------------
# Every Microsoft C++ runtime library is dropped (the startup objects'
# msvcrt.lib and libcmt.lib, vcruntime and the STL's runtime); the UCRT
# DLL's import library stays, the one system C library.
set(no_microsoft_runtime
    "LINKER:/NODEFAULTLIB:msvcrt.lib"
    "LINKER:/NODEFAULTLIB:libcmt.lib"
    "LINKER:/NODEFAULTLIB:vcruntime.lib"
    "LINKER:/NODEFAULTLIB:libvcruntime.lib"
    "LINKER:/NODEFAULTLIB:msvcprt.lib"
    "LINKER:/NODEFAULTLIB:libcpmt.lib"
    "LINKER:/NODEFAULTLIB:libucrt.lib")

# The runtime proper, kept free of exceptions and RTTI.
add_library(stxlib_msvc_runtime_core OBJECT
    "${runtime_dir}/runtime_state.cpp"
    "${runtime_dir}/throw.cpp"
    "${runtime_dir}/frame_handler.cpp"
    "${runtime_dir}/terminate.cpp"
    "${runtime_dir}/rtti.cpp"
    "${runtime_dir}/exception_ptr.cpp"
    "${runtime_dir}/thunks_x64.S")
target_include_directories(stxlib_msvc_runtime_core PRIVATE "${stxlib_include}")
target_compile_options(stxlib_msvc_runtime_core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)

# The hooks that construct and throw libc++'s std::bad_alloc, bad_cast and
# bad_typeid.
add_library(stxlib_msvc_runtime_hooks OBJECT "${runtime_dir}/cxx_hooks.cpp")
target_include_directories(stxlib_msvc_runtime_hooks PRIVATE "${stxlib_include}")
target_compile_options(stxlib_msvc_runtime_hooks PRIVATE -fexceptions -fcxx-exceptions)

add_library(stxlib_msvc_runtime_static STATIC
    $<TARGET_OBJECTS:stxlib_msvc_runtime_core>
    $<TARGET_OBJECTS:stxlib_msvc_runtime_hooks>)

# Per module, whichever runtime form an image uses.
add_library(stxlib_msvc_runtime_module STATIC
    "${runtime_dir}/module_static.cpp"
    "${runtime_dir}/strings.cpp"
    "${runtime_dir}/chkstk_x64.S"
    "${runtime_dir}/thread_statics.cpp"
    "${runtime_dir}/new_delete.cpp")
target_include_directories(stxlib_msvc_runtime_module PRIVATE "${stxlib_include}")
set_source_files_properties("${runtime_dir}/strings.cpp" PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-builtin")
set_source_files_properties("${runtime_dir}/new_delete.cpp" PROPERTIES COMPILE_OPTIONS "-fexceptions;-fcxx-exceptions")

add_library(stxlib_msvc_image OBJECT "${runtime_dir}/image_support.c")

add_library(stxlib_msvc_runtime SHARED
    $<TARGET_OBJECTS:stxlib_msvc_runtime_core>
    $<TARGET_OBJECTS:stxlib_msvc_runtime_hooks>
    $<TARGET_OBJECTS:stxlib_msvc_image>
    "${runtime_dir}/dll_entry.cpp"
    "${runtime_dir}/stxlib_msvc_runtime.def")
target_include_directories(stxlib_msvc_runtime PRIVATE "${stxlib_include}")
target_link_libraries(stxlib_msvc_runtime PRIVATE stxlib_msvc_runtime_module -lucrt)
target_link_options(stxlib_msvc_runtime PRIVATE ${no_microsoft_runtime} "LINKER:/ENTRY:stxlib_msvc_runtime_entry")

add_library(stxlib_msvc_test_entry OBJECT "${suite_dir}/support/test_entry.c")

function(add_ours_case target source runtime)
    add_executable(${target} "${source}"
        $<TARGET_OBJECTS:stxlib_msvc_test_entry>
        $<TARGET_OBJECTS:stxlib_msvc_image>)
    target_compile_options(${target} PRIVATE ${case_options})
    target_link_libraries(${target} PRIVATE ${runtime} stxlib_msvc_runtime_module -lucrt)
    target_link_options(${target} PRIVATE ${no_microsoft_runtime}
        "LINKER:/ENTRY:mainCRTStartup" "LINKER:/SUBSYSTEM:CONSOLE")
endfunction()

foreach(case_name IN LISTS conformance_cases)
    add_ours_case(case_${case_name} "${suite_dir}/cases/${case_name}.cpp" stxlib_msvc_runtime_static)
    add_ours_case(case_${case_name}_shared "${suite_dir}/cases/${case_name}.cpp" stxlib_msvc_runtime)
endforeach()

# cross_dll needs the shared runtime: one in-flight count and one set of
# active catches for both images.
add_library(cross_dll_library SHARED
    "${suite_dir}/cases/cross_dll_library.cpp"
    "${runtime_dir}/dll_entry.cpp"
    $<TARGET_OBJECTS:stxlib_msvc_image>)
target_include_directories(cross_dll_library PRIVATE "${stxlib_include}")
target_compile_options(cross_dll_library PRIVATE ${case_options})
target_compile_definitions(cross_dll_library PRIVATE STXLIB_MSVC_DLL_ENTRY=stxlib_msvc_test_dll_entry)
target_link_libraries(cross_dll_library PRIVATE stxlib_msvc_runtime stxlib_msvc_runtime_module -lucrt)
target_link_options(cross_dll_library PRIVATE ${no_microsoft_runtime} "LINKER:/ENTRY:stxlib_msvc_test_dll_entry")
add_ours_case(case_cross_dll_shared "${suite_dir}/cases/cross_dll.cpp" stxlib_msvc_runtime)
target_link_libraries(case_cross_dll_shared PRIVATE cross_dll_library)
