# STXlib's startup shim for the MSVC target (x86_64-pc-windows-msvc,
# compiled by the pinned clang): a standalone project, built and installed
# before anything that links it, like the C++ library, because every image
# of the program (STXlib's own tests, GUI.Forms, its C ABI DLL) links it
# statically in place of Microsoft's startup objects. See startup.hpp.
#
#   cmake -S src/hosted/windows_msvc_startup -B <tree> -G Ninja
#         -DCMAKE_TOOLCHAIN_FILE=<gui_forms>/cmake/llvm22-windows-msvc.cmake
#         -DGUI_FORMS_LIBCXX_PREFIX=<libc++> -DCMAKE_INSTALL_PREFIX=<prefix>
#
# Installs lib/stx_msvc_startup.lib and lib/stx_msvc_startup_vcruntime_mode.lib
# (operator new and delete and type_info's destructor, for a libc++ built in
# vcruntime mode only).
# STXLIB_MSVC_STARTUP_TESTS (default ON) builds the probes in
# tests/windows_msvc_startup, linked with the shim, vcruntime (for the
# exception runtime, Stage 1) and the UCRT, and registers them with CTest.

cmake_minimum_required(VERSION 3.25)
project(stx_msvc_startup LANGUAGES C CXX)

if(NOT WIN32 OR MINGW OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "the startup shim is built for x86_64-pc-windows-msvc with clang only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(STXLIB_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

# The shim is itself startup code: no exceptions or RTTI, no stack
# protector (it sets the cookie the protector checks), and Control Flow
# Guard metadata for its address-taken functions (the TLS callbacks the
# loader calls), so that a /guard:cf image accepts them.
set(stx_msvc_startup_options -fno-exceptions -fno-rtti -fno-stack-protector "SHELL:-Xclang -cfguard"
    -Wall -Wextra -Werror)

add_library(stx_msvc_startup STATIC
    sections.cpp
    tls.cpp
    exit_table.cpp
    security_cookie.cpp
    load_config.cpp
    chkstk.cpp
    thread_safe_statics.cpp
    program_startup.cpp
    entry_main.cpp
    entry_wmain.cpp
    entry_winmain.cpp
    entry_wwinmain.cpp
    entry_dll.cpp)
target_include_directories(stx_msvc_startup PRIVATE "${STXLIB_SOURCE_ROOT}/src")
target_compile_options(stx_msvc_startup PRIVATE ${stx_msvc_startup_options})
# The objects name no default library: the image's link chooses them. An
# empty runtime-library property drops CMake's --dependent-lib directive, so
# _DLL (UCRT's functions are declared dllimport) and _MT are spelled here.
set(stx_msvc_startup_runtime_definitions _DLL _MT)
set_target_properties(stx_msvc_startup PROPERTIES MSVC_RUNTIME_LIBRARY "")
target_compile_definitions(stx_msvc_startup PRIVATE ${stx_msvc_startup_runtime_definitions})

add_library(stx_msvc_startup_vcruntime_mode STATIC new_delete.cpp type_info_destructor.cpp)
target_compile_options(stx_msvc_startup_vcruntime_mode PRIVATE -fno-stack-protector "SHELL:-Xclang -cfguard"
    -Wall -Wextra -Werror)
set_target_properties(stx_msvc_startup_vcruntime_mode PROPERTIES MSVC_RUNTIME_LIBRARY "")
target_compile_definitions(stx_msvc_startup_vcruntime_mode PRIVATE ${stx_msvc_startup_runtime_definitions})

include(GNUInstallDirs)
install(TARGETS stx_msvc_startup stx_msvc_startup_vcruntime_mode ARCHIVE DESTINATION lib)

option(STXLIB_MSVC_STARTUP_TESTS "Build and register the startup shim's probes" ON)
if(STXLIB_MSVC_STARTUP_TESTS)
    enable_testing()
    add_subdirectory("${STXLIB_SOURCE_ROOT}/tests/windows_msvc_startup" tests)
endif()
