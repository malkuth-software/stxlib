#!/bin/sh
# make_package.sh -- make and sign a program package (C2,
# appliance/C2_DESIGN.md, "Packages"), with the base system's tools only
# (OpenSSL 3, sha256, stat, find, sort). Runs in a FreeBSD VM; never on the
# appliance (which carries no signing tool) and never with a key that
# leaves the machine it was made on unless the owner says so.
#
#   make_package.sh keygen <key.pem> <public.pub>
#       A new Ed25519 key, mode 600, and its public half as the appliance
#       reads it (/etc/stx/keys/*.pub): 64 lower-case hex digits and LF.
#   make_package.sh package <key.pem> <name> <version> <directory> <out>
#       Every regular file below <directory> becomes a package file: mode
#       0555 when executable by its owner, else 0444; paths in byte order.
#       Writes <out> as runtime/package/container.h lays it out.
#   make_package.sh latest <version> <url> <out>
#       The update document (runtime/package/latest.h).
#
# The formats are those of runtime/package/manifest.h, container.h and
# latest.h; the appliance's parsers (tested exhaustively on the host)
# refuse anything else, so a mistake here shows as a refused package.
set -eu
umask 077
export LC_ALL=C

usage() {
    echo "usage: make_package.sh keygen <key.pem> <public.pub>" >&2
    echo "       make_package.sh package <key.pem> <name> <version> <directory> <out>" >&2
    echo "       make_package.sh latest <version> <url> <out>" >&2
    exit 2
}

check_version() {
    case $1 in
    ''|0*|*[!0-9]*) echo "make_package: bad version: $1" >&2; exit 2 ;;
    esac
    [ "${#1}" -le 10 ] && [ "$1" -le 4294967295 ] || { echo "make_package: version too large: $1" >&2; exit 2; }
}

public_hex() {
    # An Ed25519 SubjectPublicKeyInfo is 44 bytes; the key is the last 32.
    openssl pkey -in "$1" -pubout -outform DER | tail -c 32 | od -An -v -tx1 | tr -d ' \n'
}

[ $# -ge 1 ] || usage
command=$1
shift
case $command in
keygen)
    [ $# -eq 2 ] || usage
    openssl genpkey -algorithm ed25519 -out "$1"
    chmod 600 "$1"
    hex=$(public_hex "$1")
    [ "${#hex}" -eq 64 ] || { echo "make_package: public key is not 32 bytes" >&2; exit 1; }
    printf '%s\n' "$hex" > "$2"
    chmod 644 "$2"
    ;;
package)
    [ $# -eq 5 ] || usage
    key=$1 name=$2 version=$3 directory=$4 out=$5
    case $name in
    [a-z]*) ;;
    *) echo "make_package: bad name: $name" >&2; exit 2 ;;
    esac
    case $name in
    *[!a-z0-9-]*) echo "make_package: bad name: $name" >&2; exit 2 ;;
    esac
    [ "${#name}" -le 32 ] || { echo "make_package: name too long: $name" >&2; exit 2; }
    check_version "$version"
    [ -d "$directory" ] || { echo "make_package: not a directory: $directory" >&2; exit 2; }
    temporary=$(mktemp -d /tmp/stxpkg.XXXXXX)
    trap 'rm -rf "$temporary"' EXIT
    ( cd "$directory" && find . -type f | sed 's|^\./||' | sort ) > "$temporary/paths"
    count=$(wc -l < "$temporary/paths" | tr -d ' ')
    [ "$count" -ge 1 ] && [ "$count" -le 1024 ] || { echo "make_package: $count files (1 to 1024)" >&2; exit 2; }
    printf 'stx-manifest 1\nname %s\nversion %s\n' "$name" "$version" > "$temporary/manifest"
    while IFS= read -r path; do
        case $path in
        *[!A-Za-z0-9._/-]*) echo "make_package: path outside the package alphabet: $path" >&2; exit 2 ;;
        esac
        [ "${#path}" -le 200 ] || { echo "make_package: path longer than 200 bytes: $path" >&2; exit 2; }
        file="$directory/$path"
        mode=0444
        [ -x "$file" ] && mode=0555
        size=$(stat -f %z "$file")
        sum=$(sha256 -q "$file")
        printf 'file %s %s %s %s\n' "$mode" "$size" "$sum" "$path" >> "$temporary/manifest"
    done < "$temporary/paths"
    length=$(stat -f %z "$temporary/manifest")
    [ "$length" -le 65536 ] || { echo "make_package: manifest is $length bytes (at most 65536)" >&2; exit 2; }
    printf 'stxpkg signature 1\n' > "$temporary/message"
    cat "$temporary/manifest" >> "$temporary/message"
    openssl pkeyutl -sign -inkey "$key" -rawin -in "$temporary/message" -out "$temporary/signature"
    [ "$(stat -f %z "$temporary/signature")" -eq 64 ] || { echo "make_package: signature is not 64 bytes" >&2; exit 1; }
    {
        printf 'STXPKG 1\nmanifest %s\n' "$length"
        cat "$temporary/manifest"
        printf 'signature 64\n'
        cat "$temporary/signature"
        while IFS= read -r path; do
            cat "$directory/$path"
        done < "$temporary/paths"
    } > "$out.partial"
    mv "$out.partial" "$out"
    chmod 644 "$out"
    echo "STXPACKAGE name=$name version=$version files=$count bytes=$(stat -f %z "$out") sha256=$(sha256 -q "$out")"
    ;;
latest)
    [ $# -eq 3 ] || usage
    check_version "$1"
    case $2 in
    https://*) ;;
    *) echo "make_package: the url must be https: $2" >&2; exit 2 ;;
    esac
    printf 'stx-latest 1\nversion %s\nurl %s\n' "$1" "$2" > "$3"
    chmod 644 "$3"
    ;;
*)
    usage
    ;;
esac
