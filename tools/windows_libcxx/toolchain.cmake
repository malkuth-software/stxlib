# Toolchain for building libc++ 22.1.8 for x86_64-pc-windows-msvc against
# STXlib's own C headers (src/hosted/windows/crt/include) and Windows API
# headers (src/hosted/windows/api/include), with no Windows SDK, Visual C++
# or MinGW header or library. Used by build_libcxx.sh beside this file.
#
# Inputs (cache variables, set by the script):
#   STXLIB_CRT_INCLUDE   the C headers
#   STXLIB_API_INCLUDE   the Windows API headers
#
# Nothing is linked during configuration (CMAKE_TRY_COMPILE_TARGET_TYPE):
# without the startup objects and import libraries no executable can link
# here, and libc++ is a static library.

set(STXLIB_LIBCXX_TARGET "x86_64-pc-windows-msvc")

find_program(_stx_clang NAMES clang REQUIRED)
find_program(_stx_clangxx NAMES clang++ REQUIRED)
get_filename_component(_stx_llvm_bin "${_stx_clang}" DIRECTORY)
find_program(_stx_llvm_ar NAMES llvm-ar HINTS "${_stx_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
find_program(_stx_llvm_ranlib NAMES llvm-ranlib HINTS "${_stx_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
find_program(_stx_lld_link NAMES lld-link HINTS "${_stx_llvm_bin}" NO_DEFAULT_PATH REQUIRED)

set(CMAKE_C_COMPILER "${_stx_clang}" CACHE FILEPATH "LLVM C driver")
set(CMAKE_CXX_COMPILER "${_stx_clangxx}" CACHE FILEPATH "LLVM C++ driver")
set(CMAKE_C_COMPILER_TARGET "${STXLIB_LIBCXX_TARGET}")
set(CMAKE_CXX_COMPILER_TARGET "${STXLIB_LIBCXX_TARGET}")
set(CMAKE_ASM_COMPILER_TARGET "${STXLIB_LIBCXX_TARGET}")
set(CMAKE_AR "${_stx_llvm_ar}" CACHE FILEPATH "LLVM archiver")
set(CMAKE_RANLIB "${_stx_llvm_ranlib}" CACHE FILEPATH "LLVM archive indexer")
set(CMAKE_LINKER "${_stx_lld_link}" CACHE FILEPATH "LLVM COFF linker")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# No runtime-library selection: CMake would otherwise pass -fms-runtime-lib,
# which defines _DLL and _MT and embeds a default-library directive for
# Microsoft's msvcrt.lib in every object. Our headers need neither macro.
set(CMAKE_MSVC_RUNTIME_LIBRARY "")
# The configuration checks re-read this file in their own projects.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES STXLIB_CRT_INCLUDE STXLIB_API_INCLUDE)

if(NOT STXLIB_CRT_INCLUDE OR NOT STXLIB_API_INCLUDE)
    message(FATAL_ERROR "set STXLIB_CRT_INCLUDE and STXLIB_API_INCLUDE")
endif()
execute_process(COMMAND "${_stx_clang}" -print-resource-dir OUTPUT_VARIABLE _stx_resource OUTPUT_STRIP_TRAILING_WHITESPACE)
file(TO_CMAKE_PATH "${_stx_resource}/include" _stx_resource_include)

# Header order: clang's resource headers, then the C headers, then the API
# headers. libc++'s own build puts its headers first (-nostdinc++ and its
# generated include folder).
# api_gaps.h: Windows API names libc++ needs that the API headers lack yet.
set(_stx_flags "-nostdinc -isystem \"${_stx_resource_include}\" -isystem \"${STXLIB_CRT_INCLUDE}\" -isystem \"${STXLIB_API_INCLUDE}\" -include \"${CMAKE_CURRENT_LIST_DIR}/api_gaps.h\" -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00")
set(CMAKE_C_FLAGS_INIT "${_stx_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_stx_flags}")
