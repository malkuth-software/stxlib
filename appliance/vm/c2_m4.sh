#!/bin/sh
# c2_m4.sh -- the C2 M4 system-side scenario, in one granted block. One
# stock arm64 VM (k-c2-m4) builds the appliance disk (build_image.sh, board
# vm) with a throwaway signing key and test packages it makes itself
# (build_test_packages.sh); then the disk boots alone, three times:
#
#   boot 1 (slot A, system version 1): normal running: user and data
#     partitions, Ethernet by DHCP, sshd (the host logs in once with a
#     throwaway key and runs true), the devfs ruleset, the targets hello
#     (foreground) and idle (server), each with its uid and folder: hello
#     reports its own folder writable and idle's folder and the program
#     store refused. surface.sh lists devfs and the listeners, and
#     check_surface.py holds them to the configuration. The boot proves
#     good. At 300 s the update service installs hello 2 from the host's
#     HTTPS server into slot b, starts it on trial and keeps it after 5 s;
#     the host then offers hello 3. The system update service installs
#     system 2 into root slot B, arms it and reboots.
#   boot 2 (slot B on trial, version 2): proves good, so B becomes good;
#     hello 2 runs from the store kept on /data. At 300 s hello 3 is
#     installed, exits at once on every start, and falls back to 2.
#   boot 3 (slot B good): version 2, hello 2, nothing pending.
#
# Then the build-time checks (check_root.sh, in the build log) and the
# run-time ones (check_surface.py over boot 1's console) are all passed.
#
# Throwaway material, deleted when the script exits however it ends: the
# SSH login key, the HTTPS server's CA and keys. The package signing key
# never leaves the build VM. Nothing is downloaded but what the guest
# fetches from the host and one SNTP query (pool.ntp.org) per boot.
#
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS.
set -eu
cd "$(dirname "$0")"
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
disk_dir=$(cd ../disk && pwd)
vm=k-c2-m4
ssh_port=52225
https_port=52443
https_base=https://10.0.2.2:$https_port

python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
vmdir=$storage/appliance/vm/$vm
work=$vmdir/c2
payload=$work/payload
serve=$work/serve
test_key=$work/ssh_test_key
known_hosts=$work/known_hosts
server_pid=
cleanup() {
    [ -z "$server_pid" ] || kill "$server_pid" 2>/dev/null || true
    rm -f "$test_key" "$test_key.pub" "$known_hosts" "$work/ca.key" "$work/server.key" "$work/server.csr" \
          "$payload/authorized_keys"
}
trap cleanup EXIT
rm -rf "$work"
mkdir -p "$work/etc" "$serve"

# Throwaway keys: the SSH login key, and the HTTPS server's CA and
# certificate for 10.0.2.2 (the guest's view of the host).
ssh-keygen -q -t ed25519 -N '' -C stxlib-c2-test -f "$test_key"
cat > "$work/openssl.cnf" <<'EOF'
[req]
distinguished_name = dn
[dn]
[ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
[server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = IP:10.0.2.2
EOF
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=stxlib-c2-test-ca -config "$work/openssl.cnf" \
    -extensions ca -keyout "$work/ca.key" -out "$work/ca.pem" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -subj /CN=10.0.2.2 -config "$work/openssl.cnf" \
    -keyout "$work/server.key" -out "$work/server.csr" 2>/dev/null
openssl x509 -req -in "$work/server.csr" -CA "$work/ca.pem" -CAkey "$work/ca.key" -CAcreateserial -days 2 \
    -extfile "$work/openssl.cnf" -extensions server -out "$work/server.pem" 2>/dev/null
cat "$work/server.pem" "$work/ca.pem" > "$work/chain.pem"
rm -f "$work/ca.key"

# The payload: the scenario's configuration, surface.sh, sockstat, the
# test CA, the login key; the build scripts beside it.
cp surface.sh "$work/etc/surface.sh"
STXLIB_ETC_STX=$work/etc STXLIB_EXTRA_MANIFEST=$disk_dir/root_manifest_check.txt \
STXLIB_EXTRA_CA=$work/ca.pem STXLIB_AUTHORIZED_KEYS=$test_key.pub \
    sh image_payload.sh vm "$payload" configs/c2-appliance.conf
cp "$disk_dir/build_image.sh" "$disk_dir/build_test_packages.sh" "$disk_dir/make_package.sh" "$payload/"
build_sha=$(shasum -a 256 "$payload/build_image.sh" | cut -d' ' -f1)
test_sha=$(shasum -a 256 "$payload/build_test_packages.sh" | cut -d' ' -f1)
package_sha=$(shasum -a 256 "$payload/make_package.sh" | cut -d' ' -f1)

disk=$vmdir/appliance.raw
rm -f "$disk"
python3 -I -c 'import sys; f = open(sys.argv[1], "wb"); f.truncate(1024 * 1024 * 1024); f.close()' "$disk"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 600 \
    --commands sessions/build-c2-test.txt --serve-dir "$payload" --receive-file "$work/serve.tar" \
    --appliance-disk "$disk" --define "build_sha256=$build_sha" --define "test_sha256=$test_sha" \
    --define "package_sha256=$package_sha" --define "https=$https_base"
tar -xf "$work/serve.tar" -C "$serve"
for file in hello/latest hello/latest-3 hello/hello-2.stxpkg hello/hello-3.stxpkg system/latest system/system-2.stxpkg; do
    [ -f "$serve/$file" ] || { echo "c2_m4: the build sent no $file" >&2; exit 1; }
done

python3 -I https_serve.py --dir "$serve" --cert "$work/chain.pem" --key "$work/server.key" --port "$https_port" \
    2> "$work/https_serve.log" &
server_pid=$!
sleep 1
kill -0 "$server_pid" || { echo "c2_m4: the HTTPS server did not start" >&2; cat "$work/https_serve.log" >&2; exit 1; }

boot() {
    label=$1
    shift
    python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label "$label" \
        --appliance-disk "$disk" --appliance-only "$@"
}
login_step="host:60:ssh -F /dev/null -i $test_key -o IdentitiesOnly=yes -o BatchMode=yes \
-o PasswordAuthentication=no -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=$known_hosts \
-o ConnectTimeout=10 -o ConnectionAttempts=5 -p $ssh_port root@127.0.0.1 true"

boot c2-boot1 --ssh-forward-port "$ssh_port" \
    --step 'expect:30:STXLOADER slot=A tries=0 fallback=0 reason=good' \
    --step 'expect:30:STXINIT uptime_ns=\d+' \
    --step 'expect:10:STXSUPERVISOR boot slot=A tries=0 fallback=0' \
    --step 'expect:30:\(ED25519\)' \
    --step 'expect:10:STXVERSION 1' \
    --step 'expect:30:STXNET dhcp \S+ ok' \
    --step 'expect:20:STXSUPERVISOR devices ruleset=' \
    --step 'expect:20:STXHELLO version=1 folder=/data/targets/hello' \
    --step 'expect:5:STXHELLO own-folder=written' \
    --step 'expect:5:STXHELLO other-folder=refused' \
    --step 'expect:5:STXHELLO store=refused' \
    --step "$login_step" \
    --step 'expect:60:STXSURFACE done' \
    --step 'expect:90:STXSUPERVISOR boot good slot=A' \
    --step 'expect:330:STXSUPERVISOR target hello outcome=installed' \
    --step 'expect:10:STXSUPERVISOR target hello slot=\w note=trial_started' \
    --step 'expect:20:STXHELLO version=2 folder=/data/targets/hello' \
    --step 'expect:30:STXSUPERVISOR target hello slot=\w note=kept' \
    --step "host:5:cp $serve/hello/latest-3 $serve/hello/latest" \
    --step 'expect:330:STXSUPERVISOR system job=update outcome=installed' \
    --step 'expect:10:STXSUPERVISOR system armed slot=B' \
    --step 'expect:10:STXSUPERVISOR reboot requested=system-update' \
    --step 'exit:60' || { echo "c2_m4: boot 1 failed"; exit 1; }
boot1_log=$(ls -d "$storage/appliance/logs/scenarios/$vm/"*-c2-boot1 | tail -1)/serial.lines
# The second target's start, read from the whole boot: two targets' output
# reaches the console in either order, and the steps above are ordered.
grep -q 'STXIDLE version=1 folder=/data/targets/idle' "$boot1_log" \
    || { echo "c2_m4: idle did not start in boot 1"; exit 1; }
python3 -I check_surface.py "$boot1_log" --device 'ttyv*' --device fb0 --device 'input/*' --port tcp/8080 \
    || { echo "c2_m4: the run-time surface has findings"; exit 1; }

boot c2-boot2 \
    --step 'expect:30:STXLOADER slot=B tries=1 fallback=0 reason=trying' \
    --step 'expect:10:STXSUPERVISOR boot slot=B tries=1 fallback=0' \
    --step 'expect:30:STXVERSION 2' \
    --step 'expect:30:STXHELLO version=2 folder=/data/targets/hello' \
    --step 'expect:90:STXSUPERVISOR boot good slot=B' \
    --step 'expect:330:STXSUPERVISOR target hello outcome=installed' \
    --step 'expect:10:STXSUPERVISOR target hello slot=\w note=trial_started' \
    --step 'expect:20:STXHELLO version=3 exiting' \
    --step 'expect:60:STXSUPERVISOR target hello slot=\w note=fell_back' \
    --step 'expect:30:STXHELLO version=2 folder=/data/targets/hello' \
    --step 'monitor:system_powerdown' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:30' || { echo "c2_m4: boot 2 failed"; exit 1; }

boot c2-boot3 \
    --step 'expect:30:STXLOADER slot=B tries=0 fallback=0 reason=good' \
    --step 'expect:30:STXVERSION 2' \
    --step 'expect:30:STXHELLO version=2 folder=/data/targets/hello' \
    --step 'expect:90:STXSUPERVISOR boot good slot=B' \
    --step 'monitor:system_powerdown' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:30' || { echo "c2_m4: boot 3 failed"; exit 1; }
echo "c2_m4: all three boots passed; surface checks clean"
