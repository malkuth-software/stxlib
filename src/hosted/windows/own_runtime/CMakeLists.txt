# STXlib's own Windows runtime, assembled for x86_64-pc-windows-msvc with no
# Visual C++ tools, Windows SDK or MinGW folder (the no-MinGW switch, owner
# 2026-11-17). A standalone project, built before anything that links it,
# like libc++: its build tree is the prefix the consumer's toolchain names
# (gui_forms cmake/llvm22-windows-msvc-own.cmake, GUI_FORMS_OWN_RUNTIME_PREFIX).
#
#   cmake -S src/hosted/windows/own_runtime -B <prefix> -G Ninja
#         -DCMAKE_TOOLCHAIN_FILE=<gui_forms>/cmake/llvm22-windows-msvc.cmake
#         -DGUI_FORMS_MSVC_RUNTIME=OWN -DGUI_FORMS_OWN_RUNTIME_SOURCE=<this STXlib>
#         -DGUI_FORMS_LIBCXX_PREFIX=<libc++ from tools/windows_libcxx>
#   cmake --build <prefix>
#
# The toolchain runs in its bootstrap form here (no prefix yet): the headers
# are STXlib's (crt/include, api/include, clang's resource headers, libc++'s),
# and nothing is linked. <prefix>/lib then holds, and the consumer names on
# every link:
#   stx_msvc_startup.lib                the startup objects
#                                       (src/hosted/windows_msvc_startup):
#                                       entry points, initializer tables, TLS
#                                       directory, thread-safe statics, the
#                                       cookie, the load configuration,
#                                       __chkstk, atexit;
#   stxlib_msvc_runtime_static.lib      the C++ runtime proper
#                                       (src/hosted/windows/msvc_runtime):
#                                       throw, the frame handler, RTTI,
#                                       exception_ptr, terminate;
#   stxlib_msvc_runtime_per_image.lib   the runtime's per-image pieces the
#                                       startup objects do not supply: the
#                                       string group, operator new and delete,
#                                       type_info's vtable;
#   stxlib_windows_builtins.lib         compiler builtins the build needs
#                                       (src/hosted/windows/builtins), when
#                                       that folder has sources;
#   <name>.lib for each api/lib/<name>.c  static data libraries (uuid: the
#                                       GUIDs our sources reference);
#   <name>.lib for each export list     import libraries written by
#                                       llvm-dlltool: ucrtbase.lib from
#                                       crt/ucrtbase.def, and one per
#                                       api/lib/<name>.def. Each .def names its
#                                       DLL on its LIBRARY line; <name> is the
#                                       name targets link (kernel32, mincore).
#
# The per-image set (README.md of msvc_runtime, "Before it can be the
# default", item 3): the startup objects keep __chkstk, the thread-safe
# statics, the TLS directory and the DLL entry; the runtime keeps the string
# group, operator new and delete and the type_info vtable. Neither part's
# other copy is built here, so no image can hold both. Microsoft's libraries
# are never named, and the consumer links with /NODEFAULTLIB.

cmake_minimum_required(VERSION 3.25)
project(stxlib_windows_own_runtime LANGUAGES C CXX ASM)

if(NOT WIN32 OR MINGW OR NOT CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    message(FATAL_ERROR "the own runtime is built for x86_64-pc-windows-msvc with clang only")
endif()
if(NOT GUI_FORMS_MSVC_RUNTIME STREQUAL "OWN" OR NOT GUI_FORMS_OWN_RUNTIME_PREFIX STREQUAL "")
    message(FATAL_ERROR "build the own runtime with the toolchain's bootstrap: GUI_FORMS_MSVC_RUNTIME=OWN "
                        "and no GUI_FORMS_OWN_RUNTIME_PREFIX")
endif()
if(NOT GUI_FORMS_LIBCXX_PREFIX)
    message(FATAL_ERROR "the C++ runtime includes libc++'s <new>, <typeinfo> and <atomic>: set GUI_FORMS_LIBCXX_PREFIX")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_ASM_COMPILER_TARGET "x86_64-pc-windows-msvc")
# Every archive in one folder, the prefix's lib/.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

get_filename_component(stxlib_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(stxlib_windows_dir "${stxlib_root}/src/hosted/windows")
set(stxlib_msvc_runtime_dir "${stxlib_windows_dir}/msvc_runtime")
set(stxlib_crt_def "${stxlib_windows_dir}/crt/ucrtbase.def")
if(NOT EXISTS "${stxlib_crt_def}")
    message(FATAL_ERROR "no UCRT export list at ${stxlib_crt_def}")
endif()

# ---- the startup objects and the C++ runtime -------------------------------------
# Both directories are added without their own targets in "all": the startup
# project's vcruntime-mode archive (new_delete.cpp, type_info_destructor.cpp)
# is for a libc++ built on vcruntime and does not compile against this one,
# and the runtime's shared form and per-module archive carry the per-image
# copies this set leaves to the startup objects. Only the archives named
# below are built, as dependencies of stxlib_windows_own_runtime.
set(STXLIB_MSVC_STARTUP_TESTS OFF CACHE BOOL "The startup shim's probes link Microsoft's runtime; off here" FORCE)
add_subdirectory("${stxlib_root}/src/hosted/windows_msvc_startup" startup EXCLUDE_FROM_ALL)
add_subdirectory("${stxlib_msvc_runtime_dir}" msvc_runtime EXCLUDE_FROM_ALL)

# The runtime's per-image pieces this set keeps (see above). Source
# properties are scoped to the directory that defines the target, so the
# runtime directory's are repeated here.
add_library(stxlib_msvc_runtime_per_image STATIC
    "${stxlib_msvc_runtime_dir}/strings.cpp"
    "${stxlib_msvc_runtime_dir}/new_delete.cpp"
    "${stxlib_msvc_runtime_dir}/module_static.cpp")
target_include_directories(stxlib_msvc_runtime_per_image PRIVATE "${stxlib_root}/include")
set_source_files_properties("${stxlib_msvc_runtime_dir}/strings.cpp" PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-builtin")
set_source_files_properties("${stxlib_msvc_runtime_dir}/new_delete.cpp" PROPERTIES COMPILE_OPTIONS "-fexceptions;-fcxx-exceptions")

set(stxlib_own_runtime_archives stx_msvc_startup stxlib_msvc_runtime_static stxlib_msvc_runtime_per_image)

# ---- compiler builtins -------------------------------------------------------------
file(GLOB stxlib_builtins_sources LIST_DIRECTORIES false
     "${stxlib_windows_dir}/builtins/*.c" "${stxlib_windows_dir}/builtins/*.cpp" "${stxlib_windows_dir}/builtins/*.S")
list(SORT stxlib_builtins_sources)
if(stxlib_builtins_sources)
    add_library(stxlib_windows_builtins STATIC ${stxlib_builtins_sources})
    list(APPEND stxlib_own_runtime_archives stxlib_windows_builtins)
endif()

# ---- static data libraries (api/lib/<name>.c) -----------------------------------
file(GLOB stxlib_data_sources LIST_DIRECTORIES false "${stxlib_windows_dir}/api/lib/*.c")
list(SORT stxlib_data_sources)
foreach(stxlib_data_source IN LISTS stxlib_data_sources)
    get_filename_component(stxlib_data_name "${stxlib_data_source}" NAME_WE)
    add_library(stxlib_windows_data_${stxlib_data_name} STATIC "${stxlib_data_source}")
    set_target_properties(stxlib_windows_data_${stxlib_data_name} PROPERTIES OUTPUT_NAME "${stxlib_data_name}")
    list(APPEND stxlib_own_runtime_archives stxlib_windows_data_${stxlib_data_name})
endforeach()

# ---- import libraries ---------------------------------------------------------------
# llvm-dlltool writes a COFF short-import library from each export list
# (machine x64); the DLL's name comes from the list's LIBRARY line.
get_filename_component(stxlib_llvm_bin "${CMAKE_C_COMPILER}" DIRECTORY)
find_program(STXLIB_LLVM_DLLTOOL NAMES llvm-dlltool HINTS "${stxlib_llvm_bin}" NO_DEFAULT_PATH REQUIRED)
file(GLOB stxlib_api_defs LIST_DIRECTORIES false "${stxlib_windows_dir}/api/lib/*.def")
list(SORT stxlib_api_defs)
set(stxlib_import_libraries "")
foreach(stxlib_def IN ITEMS "${stxlib_crt_def}" ${stxlib_api_defs})
    get_filename_component(stxlib_def_name "${stxlib_def}" NAME_WE)
    set(stxlib_import_library "${CMAKE_BINARY_DIR}/lib/${stxlib_def_name}.lib")
    if(stxlib_import_library IN_LIST stxlib_import_libraries)
        message(FATAL_ERROR "two export lists write ${stxlib_def_name}.lib")
    endif()
    add_custom_command(
        OUTPUT "${stxlib_import_library}"
        COMMAND "${STXLIB_LLVM_DLLTOOL}" -m i386:x86-64 -d "${stxlib_def}" -l "${stxlib_import_library}"
        DEPENDS "${stxlib_def}"
        COMMENT "Import library ${stxlib_def_name}.lib"
        VERBATIM)
    list(APPEND stxlib_import_libraries "${stxlib_import_library}")
endforeach()

add_custom_target(stxlib_windows_own_runtime ALL DEPENDS ${stxlib_import_libraries})
add_dependencies(stxlib_windows_own_runtime ${stxlib_own_runtime_archives})
list(LENGTH stxlib_import_libraries stxlib_import_count)
message(STATUS "STXlib own runtime: ${stxlib_own_runtime_archives}; ${stxlib_import_count} import libraries")
