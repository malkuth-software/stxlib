#!/bin/sh
# Builds static libc++ 22.1.8 for x86_64-pc-windows-msvc with
# LIBCXX_NO_VCRUNTIME=ON (libc++'s headers declare std::exception, the
# standard exception classes, type_info and the new/delete declarations
# instead of expecting vcruntime's headers), against STXlib's own C and
# Windows API headers only: no Windows SDK, Visual C++ or MinGW header.
#
# Not in the result, supplied by STXlib's runtime and startup shim: operator
# new and delete (LIBCXX_ENABLE_NEW_DELETE_DEFINITIONS stays OFF), the
# exception_ptr helpers (__ExceptionPtr*), the EH and RTTI entries,
# thread-safe static initialisation (_Init_thread_*), the string group
# (memcpy and the rest), __chkstk, _fltused, _tls_index and atexit.
# The same configuration as the EH stream's D:/gw_eh_build/libcxx-novc.
#
# Usage (from PowerShell, through the MSYS2 CLANG64 shell, under the build
# lock as every build on the Windows machine):
#   $env:MSYSTEM='CLANG64'; $env:MSYS2_PATH_TYPE='inherit'
#   & D:\a\_temp\setup-msys2\msys2.cmd -c "<lock> <owner> <tree> tools/windows_libcxx/build_libcxx.sh"
#
# Environment (all optional):
#   LLVM_SRC      llvm-project 22.1.8 source root (has runtimes/ and libcxx/);
#                 default D:/dl/llvm-22.1.8/llvm-project-22.1.8.src, the
#                 release tarball llvm-project-22.1.8.src.tar.xz, sha256
#                 922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888
#   BUILD_DIR     the build tree; default D:/gw_crt_build/libcxx-tree
#   PREFIX        the install prefix; default D:/gw_crt_build/libcxx-own
#   API_INCLUDE   the Windows API headers; default this clone's
#                 src/hosted/windows/api/include
#   JOBS          parallel jobs; default 8
#
# Result: $PREFIX/lib/libc++.lib (and libc++experimental.lib),
# $PREFIX/include/c++/v1 with __config_site carrying _LIBCPP_NO_VCRUNTIME.
# Logs: $BUILD_DIR/../logs/libcxx-{configure,build,install}.log. Exit status
# is the first failing step's.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
LLVM_SRC=${LLVM_SRC:-D:/dl/llvm-22.1.8/llvm-project-22.1.8.src}
BUILD_DIR=${BUILD_DIR:-D:/gw_crt_build/libcxx-tree}
PREFIX=${PREFIX:-D:/gw_crt_build/libcxx-own}
API_INCLUDE=${API_INCLUDE:-$root/src/hosted/windows/api/include}
JOBS=${JOBS:-8}
CRT_INCLUDE=$root/src/hosted/windows/crt/include
LOGS=$(dirname "$BUILD_DIR")/logs
mkdir -p "$LOGS"

for required in "$LLVM_SRC/runtimes/CMakeLists.txt" "$CRT_INCLUDE/stdlib.h" "$API_INCLUDE/windows.h"; do
    if [ ! -f "$required" ]; then
        echo "build_libcxx: missing $required" >&2
        exit 2
    fi
done

cmake -S "$LLVM_SRC/runtimes" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$here/toolchain.cmake" \
    -DSTXLIB_CRT_INCLUDE="$CRT_INCLUDE" -DSTXLIB_API_INCLUDE="$API_INCLUDE" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DLLVM_ENABLE_RUNTIMES=libcxx \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_CXX_ABI=vcruntime -DLIBCXX_NO_VCRUNTIME=ON \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_DOCS=OFF \
    -DLIBCXX_ENABLE_EXPERIMENTAL_LIBRARY=ON -DLIBCXX_INSTALL_MODULES=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    > "$LOGS/libcxx-configure.log" 2>&1
status=$?
echo "libcxx configure rc=$status (log $LOGS/libcxx-configure.log)"
if [ $status -ne 0 ]; then
    grep -E "CMake Error" -A4 "$LOGS/libcxx-configure.log" | head -30
    exit $status
fi

cmake --build "$BUILD_DIR" --parallel "$JOBS" -- -k 0 > "$LOGS/libcxx-build.log" 2>&1
status=$?
echo "libcxx build rc=$status errors=$(grep -c 'error:' "$LOGS/libcxx-build.log")"
grep -E "^FAILED:|error:" "$LOGS/libcxx-build.log" | head -40
if [ $status -ne 0 ]; then exit $status; fi

cmake --install "$BUILD_DIR" > "$LOGS/libcxx-install.log" 2>&1
status=$?
echo "libcxx install rc=$status"
if [ $status -ne 0 ]; then exit $status; fi
ls "$PREFIX/lib"
if ! grep -q "_LIBCPP_NO_VCRUNTIME" "$PREFIX/include/c++/v1/__config_site"; then
    echo "build_libcxx: __config_site lacks _LIBCPP_NO_VCRUNTIME" >&2
    exit 3
fi
echo "build_libcxx: done, $PREFIX"
