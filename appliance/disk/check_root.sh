#!/bin/sh
# check_root.sh <root> <expected> -- the build-time half of the surface
# inventory (C2, appliance/C2_DESIGN.md, "Surface inventory"). Run by
# build_image.sh on a freshly populated root slot, mounted at <root>.
# Every check walks every path of the root; none samples.
#
#   1. The root is exactly its inventory: every path under <root> is in
#      <expected> (build_image.sh writes it: the manifest's paths, every file
#      below the manifest's directories, the libraries the programs need,
#      our own files and mount points, and every parent directory of
#      those), and every path in <expected> is under <root>.
#   2. No file is set-user-ID or set-group-ID, none is writable by others,
#      and there is no device node (devfs makes those at boot).
#   3. Every symbolic link resolves inside the root (except the run-time
#      ones named below), and every ELF file's needed libraries are in the
#      root's /lib or /usr/lib.
#
# The run-time half (listening sockets, visible devices) is in
# appliance/vm/check_surface.py, over what surface.sh prints in the VM.
#
# Prints one STXCHECK line per check, then each finding; exit 1 on any.
set -eu
export LC_ALL=C
root=$1
expected=$2
[ -d "$root" ] && [ -f "$expected" ] || { echo "check_root: usage: check_root.sh <root> <expected>" >&2; exit 2; }
work=$(mktemp -d /tmp/stxcheck.XXXXXX)
trap 'rm -rf "$work"' EXIT
status=0

# Links whose target exists only while the appliance runs.
runtime_links='/etc/resolv.conf'

# 1. Inventory.
( cd "$root" && find . ) | sed 's|^\.||; s|^$|/|' | sort -u > "$work/actual"
# Parents of every expected path: /a/b/c gives /, /a, /a/b, /a/b/c.
awk '{
    path = $0
    print path
    while (path != "" && path != "/") {
        sub(/\/[^\/]*$/, "", path)
        if (path == "") { print "/" } else { print path }
    }
}' "$expected" | sort -u > "$work/wanted"
comm -23 "$work/actual" "$work/wanted" > "$work/unexpected"
comm -13 "$work/actual" "$work/wanted" > "$work/missing"
echo "STXCHECK root paths=$(wc -l < "$work/actual" | tr -d ' ') expected=$(wc -l < "$work/wanted" | tr -d ' ') unexpected=$(wc -l < "$work/unexpected" | tr -d ' ') missing=$(wc -l < "$work/missing" | tr -d ' ')"
sed 's/^/STXCHECK root unexpected /' "$work/unexpected"
sed 's/^/STXCHECK root missing /' "$work/missing"
if [ -s "$work/unexpected" ] || [ -s "$work/missing" ]; then status=1; fi

# 2. Modes and node types.
( cd "$root" && find . \( -perm -4000 -o -perm -2000 \) ) | sed 's|^\.||' > "$work/setid"
( cd "$root" && find . ! -type l -perm -0002 ) | sed 's|^\.||' > "$work/writable"
( cd "$root" && find . \( -type b -o -type c -o -type p -o -type s \) ) | sed 's|^\.||' > "$work/nodes"
echo "STXCHECK modes paths=$(wc -l < "$work/actual" | tr -d ' ') setid=$(wc -l < "$work/setid" | tr -d ' ') other-writable=$(wc -l < "$work/writable" | tr -d ' ') nodes=$(wc -l < "$work/nodes" | tr -d ' ')"
sed 's/^/STXCHECK modes setid /' "$work/setid"
sed 's/^/STXCHECK modes other-writable /' "$work/writable"
sed 's/^/STXCHECK modes node /' "$work/nodes"
if [ -s "$work/setid" ] || [ -s "$work/writable" ] || [ -s "$work/nodes" ]; then status=1; fi

# 3. Links and libraries.
links=0
: > "$work/dangling"
( cd "$root" && find . -type l ) | sed 's|^\.||' > "$work/links"
while IFS= read -r link; do
    links=$((links + 1))
    case " $runtime_links " in *" $link "*) continue ;; esac
    target=$(readlink "$root$link")
    case $target in
    /*) resolved=$target ;;
    *) resolved=$(dirname "$link")/$target ;;
    esac
    [ -e "$root$resolved" ] || echo "$link -> $target" >> "$work/dangling"
done < "$work/links"
elves=0
: > "$work/unmet"
( cd "$root" && find . -type f ) | sed 's|^\.||' > "$work/files"
while IFS= read -r file; do
    readelf -h "$root$file" > /dev/null 2>&1 || continue
    elves=$((elves + 1))
    for library in $(readelf -d "$root$file" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p'); do
        [ -e "$root/lib/$library" ] || [ -e "$root/usr/lib/$library" ] || echo "$file needs $library" >> "$work/unmet"
    done
done < "$work/files"
echo "STXCHECK links links=$links dangling=$(wc -l < "$work/dangling" | tr -d ' ') elf=$elves unmet=$(wc -l < "$work/unmet" | tr -d ' ')"
sed 's/^/STXCHECK links dangling /' "$work/dangling"
sed 's/^/STXCHECK links unmet /' "$work/unmet"
if [ -s "$work/dangling" ] || [ -s "$work/unmet" ]; then status=1; fi

if [ "$status" -eq 0 ]; then echo "STXCHECK root clean"; else echo "STXCHECK root findings"; fi
exit "$status"
