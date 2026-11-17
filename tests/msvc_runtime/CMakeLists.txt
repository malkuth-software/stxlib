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
# The runtime's own target file, shared with the root build's
# STXLIB_MSVC_RUNTIME option. It sets stxlib_msvc_no_microsoft_runtime.
add_subdirectory("${runtime_dir}" runtime)
set(no_microsoft_runtime ${stxlib_msvc_no_microsoft_runtime})

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
