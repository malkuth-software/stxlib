# The MSVC C++ runtime conformance suite (src/hosted/windows/msvc_runtime): small
# standalone programs, each built twice from one source, against
# Microsoft's vcruntime (the oracle) and against STXlib's runtime, and
# compared case by case on stdout and exit code by
# tools/msvc_runtime/run_conformance.py.
#
# A standalone project, configured once per side with the GUI.Forms
# x86_64-pc-windows-msvc toolchain file (clang 22 and lld-link, never the
# MSVC compiler):
#   oracle: -DSTXLIB_MSVC_RUNTIME_SIDE=oracle -DGUI_FORMS_LIBCXX_PREFIX=<libc++, vcruntime ABI>
#   ours:   -DSTXLIB_MSVC_RUNTIME_SIDE=ours -DGUI_FORMS_MSVC_RUNTIME=OWN
#           -DGUI_FORMS_OWN_RUNTIME_SOURCE=<STXlib with crt/ and api/ headers>
#           -DGUI_FORMS_LIBCXX_PREFIX=<libc++ built against them, _LIBCPP_NO_VCRUNTIME>
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
    states
    structured_and_jump)

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
# Configured with the toolchain's own-runtime mode (GUI_FORMS_MSVC_RUNTIME=OWN,
# cmake/llvm22-windows-msvc-own.cmake) in its bootstrap form, with no
# GUI_FORMS_OWN_RUNTIME_PREFIX: it gives -nostdinc and STXlib's C library and
# Windows API include folders, libc++'s headers, and a link that names no
# library. This project then names every library itself: the startup shim,
# the runtime, libc++, and import libraries made from STXlib's export lists.
if(NOT GUI_FORMS_OWN_RUNTIME_SOURCE OR NOT GUI_FORMS_LIBCXX_PREFIX)
    message(FATAL_ERROR "the ours side needs the toolchain's own-runtime mode: GUI_FORMS_MSVC_RUNTIME=OWN, "
                        "GUI_FORMS_OWN_RUNTIME_SOURCE and GUI_FORMS_LIBCXX_PREFIX")
endif()

# Import libraries from the export lists (llvm-dlltool, as the integrator's
# assembly project makes them).
get_filename_component(suite_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
find_program(suite_dlltool NAMES llvm-dlltool HINTS "${suite_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
set(suite_import_defs
    "${GUI_FORMS_OWN_RUNTIME_SOURCE}/src/hosted/windows/crt/ucrtbase.def"
    "${GUI_FORMS_OWN_RUNTIME_SOURCE}/src/hosted/windows/api/lib/kernel32.def")
set(suite_import_libraries "")
foreach(suite_def IN LISTS suite_import_defs)
    get_filename_component(suite_def_name "${suite_def}" NAME_WE)
    set(suite_import_library "${CMAKE_BINARY_DIR}/lib/${suite_def_name}.lib")
    add_custom_command(OUTPUT "${suite_import_library}"
        COMMAND "${suite_dlltool}" -m i386:x86-64 -d "${suite_def}" -l "${suite_import_library}"
        DEPENDS "${suite_def}" VERBATIM)
    list(APPEND suite_import_libraries "${suite_import_library}")
endforeach()
add_custom_target(suite_import_libraries DEPENDS ${suite_import_libraries})
file(GLOB suite_libcxx_archives LIST_DIRECTORIES false "${GUI_FORMS_LIBCXX_PREFIX}/lib/*.lib")

# The startup shim first: the runtime's shared form links it.
set(STXLIB_MSVC_STARTUP_TESTS OFF CACHE BOOL "The startup probes are not part of this suite" FORCE)
add_subdirectory("${suite_dir}/../../src/hosted/windows_msvc_startup" startup)
add_subdirectory("${runtime_dir}" runtime)

set(suite_system_libraries ${suite_libcxx_archives} ${suite_import_libraries})
add_dependencies(stxlib_msvc_runtime suite_import_libraries)
target_link_libraries(stxlib_msvc_runtime PRIVATE ${suite_system_libraries})

function(add_ours_case target source runtime)
    add_executable(${target} "${source}")
    add_dependencies(${target} suite_import_libraries)
    target_compile_options(${target} PRIVATE ${case_options})
    target_link_libraries(${target} PRIVATE stx_msvc_startup ${runtime} stxlib_msvc_runtime_module ${suite_system_libraries})
    target_link_options(${target} PRIVATE "LINKER:/ENTRY:mainCRTStartup" "LINKER:/SUBSYSTEM:CONSOLE")
endfunction()

foreach(case_name IN LISTS conformance_cases)
    add_ours_case(case_${case_name} "${suite_dir}/cases/${case_name}.cpp" stxlib_msvc_runtime_static)
    add_ours_case(case_${case_name}_shared "${suite_dir}/cases/${case_name}.cpp" stxlib_msvc_runtime)
endforeach()

# cross_dll needs the shared runtime: one in-flight count and one set of
# active catches for both images. The DLL's entry is the startup's.
add_library(cross_dll_library SHARED "${suite_dir}/cases/cross_dll_library.cpp")
add_dependencies(cross_dll_library suite_import_libraries)
target_compile_options(cross_dll_library PRIVATE ${case_options})
target_link_libraries(cross_dll_library PRIVATE stx_msvc_startup stxlib_msvc_runtime stxlib_msvc_runtime_module
    ${suite_system_libraries})
target_link_options(cross_dll_library PRIVATE "LINKER:/ENTRY:_DllMainCRTStartup")
add_ours_case(case_cross_dll_shared "${suite_dir}/cases/cross_dll.cpp" stxlib_msvc_runtime)
target_link_libraries(case_cross_dll_shared PRIVATE cross_dll_library)
