#!/bin/sh
# build_test_packages.sh -- the VM scenario's throwaway signing key and test
# packages (appliance/vm/c2_m4.sh), in the stock FreeBSD build VM, with
# make_package.sh (/tmp/make_package.sh, fetched and checked by the
# session). Test only: the key is made for one run, its private half never
# leaves the VM and is deleted by "system"; no board image trusts it.
#
#   build_test_packages.sh prepare <dir> <https-base>
#       <dir>/key/test.pem               the private key (deleted by system)
#       <dir>/extra/key-test.pub         trusted by the test image
#       <dir>/extra/package-hello.stxpkg hello 1, installed at the factory
#       <dir>/extra/package-idle.stxpkg  idle 1, installed at the factory
#       <dir>/serve/hello/hello-2.stxpkg a good update
#       <dir>/serve/hello/hello-3.stxpkg an update that exits at once
#       <dir>/serve/hello/latest         names hello 2
#       <dir>/serve/hello/latest-3       names hello 3 (the host swaps it in)
#   build_test_packages.sh system <dir> <disk> <https-base>
#       <dir>/serve/system/system-2.stxpkg  root A of <disk>, version 2
#       <dir>/serve/system/latest           names it
#       then deletes <dir>/key.
#
# hello writes in its own folder, tries to write in idle's and in the
# program store, and says what each did; that is the scenario's check of
# plain separation (each target its own uid and a 0700 folder).
set -eu
make_package=/tmp/make_package.sh
command=$1
shift

hello_program() {
    # hello_program <version> <file>
    cat > "$2" <<EOF
#!/bin/sh
echo "STXHELLO version=$1 folder=\$(pwd)"
if echo probe > ./probe 2>/dev/null; then echo "STXHELLO own-folder=written"; else echo "STXHELLO own-folder=refused"; fi
if echo probe > /data/targets/idle/probe 2>/dev/null; then echo "STXHELLO other-folder=written"; else echo "STXHELLO other-folder=refused"; fi
if echo probe > /data/programs/hello/probe 2>/dev/null; then echo "STXHELLO store=written"; else echo "STXHELLO store=refused"; fi
exec /bin/sleep 86400
EOF
    chmod 555 "$2"
}

case $command in
prepare)
    [ $# -eq 2 ] || { echo "usage: build_test_packages.sh prepare <dir> <https-base>" >&2; exit 2; }
    dir=$1 base=$2
    rm -rf "$dir"
    mkdir -p "$dir/key" "$dir/extra" "$dir/serve/hello" "$dir/serve/system" "$dir/src"
    chmod 700 "$dir/key"
    sh "$make_package" keygen "$dir/key/test.pem" "$dir/extra/key-test.pub"
    for version in 1 2; do
        mkdir -p "$dir/src/hello-$version/bin"
        hello_program "$version" "$dir/src/hello-$version/bin/hello"
    done
    mkdir -p "$dir/src/hello-3/bin"
    printf '#!/bin/sh\necho "STXHELLO version=3 exiting"\nexit 3\n' > "$dir/src/hello-3/bin/hello"
    chmod 555 "$dir/src/hello-3/bin/hello"
    mkdir -p "$dir/src/idle-1/bin"
    printf '#!/bin/sh\necho "STXIDLE version=1 folder=$(pwd)"\nexec /bin/sleep 86400\n' > "$dir/src/idle-1/bin/idle"
    chmod 555 "$dir/src/idle-1/bin/idle"
    sh "$make_package" package "$dir/key/test.pem" hello 1 "$dir/src/hello-1" "$dir/extra/package-hello.stxpkg"
    sh "$make_package" package "$dir/key/test.pem" idle 1 "$dir/src/idle-1" "$dir/extra/package-idle.stxpkg"
    sh "$make_package" package "$dir/key/test.pem" hello 2 "$dir/src/hello-2" "$dir/serve/hello/hello-2.stxpkg"
    sh "$make_package" package "$dir/key/test.pem" hello 3 "$dir/src/hello-3" "$dir/serve/hello/hello-3.stxpkg"
    sh "$make_package" latest 2 "$base/hello/hello-2.stxpkg" "$dir/serve/hello/latest"
    sh "$make_package" latest 3 "$base/hello/hello-3.stxpkg" "$dir/serve/hello/latest-3"
    ;;
system)
    [ $# -eq 3 ] || { echo "usage: build_test_packages.sh system <dir> <disk> <https-base>" >&2; exit 2; }
    dir=$1 disk=$2 base=$3
    case $disk in vtbd[1-9]) ;; *) echo "build_test_packages: refusing disk $disk" >&2; exit 2 ;; esac
    mkdir -p "$dir/a" "$dir/new" "$dir/src/system"
    mount -o ro "/dev/${disk}p3" "$dir/a"
    # The image is the slot's whole size (64 MiB, build_image.sh), so that
    # stxpkg writes every sector of the slot.
    slot_bytes=$(diskinfo "/dev/${disk}p3" | awk '{print $3}')
    truncate -s "$slot_bytes" "$dir/src/system/root.img"
    md=$(mdconfig -a -t vnode -f "$dir/src/system/root.img")
    # A neutral label: stxpkg refuses an image that names a root slot
    # (relabel sets the slot's own after the read-back).
    newfs -L STXROOTNEW "/dev/$md" > /dev/null
    mount "/dev/$md" "$dir/new"
    ( cd "$dir/a" && tar -cf - . ) | tar -xpf - -C "$dir/new"
    chmod 644 "$dir/new/etc/stx/version"
    printf '2\n' > "$dir/new/etc/stx/version"
    chmod 444 "$dir/new/etc/stx/version"
    umount "$dir/new"
    mdconfig -d -u "$md"
    umount "$dir/a"
    chmod 444 "$dir/src/system/root.img"
    sh "$make_package" package "$dir/key/test.pem" system 2 "$dir/src/system" "$dir/serve/system/system-2.stxpkg"
    sh "$make_package" latest 2 "$base/system/system-2.stxpkg" "$dir/serve/system/latest"
    rm -rf "$dir/key" "$dir/src"
    ;;
*)
    echo "usage: build_test_packages.sh prepare|system ..." >&2
    exit 2
    ;;
esac
