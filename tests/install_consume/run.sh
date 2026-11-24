#!/bin/sh
# The install-and-consume check: install STXlib as stx does (package/), then
# configure, build and run a find_package consumer (consumer/). It catches a
# library that links a target no export set holds (aarch64 Linux's
# outline-atomics helpers, 2026-11-23). Usage: run.sh <STXlib source> <work dir>
# [extra cmake arguments for both configures].
set -eu
source_dir=$(cd "$1" && pwd); work=$2; shift 2
here=$(cd "$(dirname "$0")" && pwd)
rm -rf "$work"; mkdir -p "$work"; work=$(cd "$work" && pwd)
cmake -S "$here/package" -B "$work/package" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DSTXLIB_SOURCE="$source_dir" -DCMAKE_INSTALL_PREFIX="$work/prefix" "$@"
cmake --build "$work/package" --parallel 4
cmake --install "$work/package"
cmake -S "$here/consumer" -B "$work/consumer" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$work/prefix" "$@"
cmake --build "$work/consumer"
"$work/consumer/stxlib_consumer"
