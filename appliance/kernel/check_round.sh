#!/bin/sh
# check_round.sh [CONFIG] [BASELINE] -- static check of a cut before its build.
# Runs check_config.py on CONFIG (default STXLIB-VM) and on BASELINE (default
# STXLIB-GENERIC-NOTSLOG, which builds and boots) and prints only the
# missing-header findings CONFIG adds. Exit 0 when it adds none, 1 when it
# adds some, 2 on a usage or tool error. Reads only. A name <arch>:<name>
# is the source tree's own sys/<arch>/conf/<name>: the amd64 cut is
# compared with "check_round.sh STXLIB-AMD64 amd64:GENERIC" (check_config.py
# takes each configuration's architecture from its own lines).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
config=${1:-STXLIB-VM}
baseline=${2:-STXLIB-GENERIC-NOTSLOG}
sys=${STXLIB_FREEBSD_SRC:-/Volumes/Storage/stxlib_workers/src/freebsd-src}/sys
work=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}/appliance/tmp/check_round
mkdir -p "$work"
for name in "$config" "$baseline"; do
    case $name in
    arm64:*|amd64:*) file=$sys/${name%%:*}/conf/${name#*:}; out=$(echo "$name" | tr ':' '_') ;;
    *) file=$here/$name; out=$name ;;
    esac
    case ${name#*:} in
    *[!A-Za-z0-9_-]*|'') echo "check_round: bad configuration name: $name" >&2; exit 2 ;;
    esac
    [ -f "$file" ] || { echo "check_round: no configuration $file" >&2; exit 2; }
    status=0
    python3 -I "$here/check_config.py" "$sys" "$file" > "$work/$out.out" || status=$?
    [ "$status" -le 1 ] || { echo "check_round: check_config.py failed on $name" >&2; exit 2; }
    tail -n 1 "$work/$out.out"
done
config_out=$(echo "$config" | tr ':' '_')
baseline_out=$(echo "$baseline" | tr ':' '_')
added=$(grep -vxF -f "$work/$baseline_out.out" "$work/$config_out.out" | grep -v '^check_config:' || true)
if [ -n "$added" ]; then
    echo "check_round: $config adds missing generated headers:"
    echo "$added"
    exit 1
fi
echo "check_round: $config adds no missing generated headers over $baseline"
