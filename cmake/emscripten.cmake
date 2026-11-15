# The Emscripten toolchain for STXlib's Emscripten family (owner decision
# 2026-11-15, in 1.0 after M3), pinned to the emsdk release GUI.Forms' web
# host uses (its cmake/emscripten.cmake on claude/cp34-web-host). Pass it at
# configure time:
#
#   cmake -S . -B build-web -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/emscripten.cmake
#   cmake --build build-web
#   ctest --test-dir build-web --output-on-failure
#
# ctest runs each test under Node: Emscripten's platform file sets
# CMAKE_CROSSCOMPILING_EMULATOR to emsdk's node
# (cmake/Modules/Platform/Emscripten.cmake:367-372 at emscripten 6.0.13),
# and add_test prefixes it to every executable target.
#
# The emsdk is found through STXLIB_EMSDK (a cache entry or the
# environment) or EMSDK (set by emsdk_env.sh); it must be the pinned
# release, with its upstream/emscripten tree, whose emscripten-version.txt
# names the version ("6.0.13-git" at the tag, so the first three numbers
# are compared).
#
# The flags every translation unit and link takes, GUI.Forms' set so the
# two link into one module:
#   -pthread                threads as Web Workers sharing the memory
#   -msimd128               WebAssembly's 128-bit SIMD (simd.hpp: vector128)
#   -fwasm-exceptions       native WebAssembly exceptions; STXlib's own
#                           targets add -fno-exceptions, tests and consumers
#                           keep them
#   -sSUPPORT_LONGJMP=wasm  setjmp and longjmp by the same mechanism
# and, while STXLIB_EMSCRIPTEN_WASM64 is on (the default):
#   -m64                    wasm64 (Emscripten's MEMORY64 mode, whose
#                           setting's comment recommends -m64, src/settings.js
#                           at 6.0.13). STXlib states a 64-bit target
#                           (include/stxlib/types.hpp: sizeof(void*) == 8)
#                           and its proofs assume it, so wasm32 is refused
#                           there at compile time; GUI.Forms' file sets no
#                           -m64 today, so the two must agree before they
#                           link (hand-in decision).

set(STXLIB_EMSDK_VERSION "6.0.13" CACHE INTERNAL "The emsdk release STXlib's Emscripten family is built with")
option(STXLIB_EMSCRIPTEN_WASM64 "Build for wasm64 (-m64); STXlib's types require a 64-bit target" ON)

set(_stxlib_emsdk "")
if(DEFINED STXLIB_EMSDK AND NOT STXLIB_EMSDK STREQUAL "")
    set(_stxlib_emsdk "${STXLIB_EMSDK}")
elseif(DEFINED ENV{STXLIB_EMSDK})
    set(_stxlib_emsdk "$ENV{STXLIB_EMSDK}")
elseif(DEFINED ENV{EMSDK})
    set(_stxlib_emsdk "$ENV{EMSDK}")
endif()
if(_stxlib_emsdk STREQUAL "")
    message(FATAL_ERROR "Set STXLIB_EMSDK (or source emsdk_env.sh) to emsdk ${STXLIB_EMSDK_VERSION}")
endif()
file(TO_CMAKE_PATH "${_stxlib_emsdk}" _stxlib_emsdk)
set(STXLIB_EMSDK "${_stxlib_emsdk}" CACHE PATH "The emsdk directory (emsdk ${STXLIB_EMSDK_VERSION})")

set(_stxlib_emscripten "${_stxlib_emsdk}/upstream/emscripten")
set(_stxlib_version_file "${_stxlib_emscripten}/emscripten-version.txt")
if(NOT EXISTS "${_stxlib_version_file}")
    message(FATAL_ERROR "${_stxlib_emsdk} holds no upstream/emscripten tree (activate emsdk ${STXLIB_EMSDK_VERSION})")
endif()
file(READ "${_stxlib_version_file}" _stxlib_version_text)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" _stxlib_version "${_stxlib_version_text}")
if(NOT _stxlib_version STREQUAL STXLIB_EMSDK_VERSION)
    message(FATAL_ERROR "STXlib's Emscripten family needs emsdk ${STXLIB_EMSDK_VERSION}; ${_stxlib_emsdk} is ${_stxlib_version}")
endif()

include("${_stxlib_emscripten}/cmake/Modules/Platform/Emscripten.cmake")

set(_stxlib_web_flags "-pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm")
if(STXLIB_EMSCRIPTEN_WASM64)
    string(APPEND _stxlib_web_flags " -m64")
endif()
set(CMAKE_C_FLAGS_INIT "${_stxlib_web_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_stxlib_web_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_stxlib_web_flags}")
