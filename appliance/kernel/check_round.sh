#!/bin/sh
# check_round.sh [CONFIG] [BASELINE] -- static check of a cut before its build.
# Runs check_config.py on CONFIG (default STXLIB-VM) and on BASELINE (default
# STXLIB-GENERIC-NOTSLOG, which builds and boots) and prints only the
# missing-header findings CONFIG adds. Exit 0 when it adds none, 1 when it
# adds some, 2 on a usage or tool error. Reads only.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
config=${1:-STXLIB-VM}
baseline=${2:-STXLIB-GENERIC-NOTSLOG}
sys=${STXLIB_FREEBSD_SRC:-/Volumes/Storage/stxlib_workers/src/freebsd-src}/sys
work=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}/appliance/tmp/check_round
mkdir -p "$work"
for name in "$config" "$baseline"; do
    case $name in
    *[!A-Za-z0-9_-]*|'') echo "check_round: bad configuration name: $name" >&2; exit 2 ;;
    esac
    [ -f "$here/$name" ] || { echo "check_round: no configuration $here/$name" >&2; exit 2; }
    status=0
    python3 -I "$here/check_config.py" "$sys" "$here/$name" > "$work/$name.out" || status=$?
    [ "$status" -le 1 ] || { echo "check_round: check_config.py failed on $name" >&2; exit 2; }
    tail -n 1 "$work/$name.out"
done
added=$(grep -vxF -f "$work/$baseline.out" "$work/$config.out" | grep -v '^check_config:' || true)
if [ -n "$added" ]; then
    echo "check_round: $config adds missing generated headers:"
    echo "$added"
    exit 1
fi
echo "check_round: $config adds no missing generated headers over $baseline"
