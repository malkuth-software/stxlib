#!/bin/sh
# c2_web.sh [KERNCONF] -- C2: the web interface in a test VM. One fresh VM
# (k-<kernconf>-c2web) set up as in appliance/vm/c1_s5.sh with
# configs/c2-web.conf, stxweb and its pages (sessions/install-web.txt) and a
# provisioned password (../tools/make_password_turn.py, a random test
# password kept beside the logs, never served to the guest).
#
# Boot one: the service makes its TLS identity and prints the fingerprint;
# web_check.py --phase first drives the interface over the forwarded port
# (pages, login, services, CSRF, the user configuration by turns, an
# upload) and asks for a reboot; the supervisor stops its services and
# reboots, which ends QEMU (-no-reboot). Boot two: the same identity read
# back (the fingerprint printed again, the same one), web_check.py --phase
# second (the configuration kept, logout), then the power button.
#
# Each boot is one scenario.py run in the background, with web_check.py in
# the foreground; the scenario's expect steps follow the service's console
# lines that the checks cause. Passes when both scenarios and both checks
# pass and the certificate web_check saw is the one the console printed.
# Environment: STXLIB_STORAGE, STXLIB_IMAGE, STXLIB_CHECKSUMS,
# STXLIB_WEB_PORT (the host port forwarded to the guest's 22; default
# 10443).
set -eu
cd "$(dirname "$0")"
here=$(pwd)
vmtools=$(cd ../../vm && pwd)
static=$(cd ../static && pwd)
tools=$(cd ../tools && pwd)
kernconf=${1:-STXLIB-VM}
case $kernconf in
*[!A-Za-z0-9_-]*|'') echo "c2_web: bad KERNCONF: $kernconf" >&2; exit 2 ;;
esac
storage=${STXLIB_STORAGE:-/Volumes/Storage/stxlib_workers}
image=${STXLIB_IMAGE:-$storage/downloads/FreeBSD-15.2-BETA1-arm64-aarch64-ufs.qcow2.xz}
checksums=${STXLIB_CHECKSUMS:-$storage/downloads/15.2-BETA1-aarch64-CHECKSUM.SHA256}
port=${STXLIB_WEB_PORT:-10443}
case $port in
*[!0-9]*|'') echo "c2_web: bad STXLIB_WEB_PORT: $port" >&2; exit 2 ;;
esac
label=$(printf '%s' "$kernconf" | tr 'A-Z' 'a-z')
kernels=$storage/appliance/kernels/$kernconf
programs=$storage/appliance/programs
configs=$here/configs
for file in "$kernels/kernel.sha256" "$programs/stxinit.sha256" "$programs/stxsupervisor.sha256" \
    "$programs/stxweb.sha256"; do
    [ -f "$file" ] || { echo "c2_web: missing $file" >&2; exit 2; }
done
vm=k-$label-c2web
kernel_sha=$(cut -d' ' -f1 "$kernels/kernel.sha256")
init_sha=$(cut -d' ' -f1 "$programs/stxinit.sha256")
supervisor_sha=$(cut -d' ' -f1 "$programs/stxsupervisor.sha256")
web_sha=$(cut -d' ' -f1 "$programs/stxweb.sha256")
config_sha=$(shasum -a 256 "$configs/c2-web.conf" | cut -d' ' -f1)
sed "s/^/stxweb size: /" "$programs/stxweb.size"

# What the guest fetches: stxweb, the pages, the password record. The test
# password itself stays outside the served directory.
work=$storage/appliance/web_runs/$vm
stage=$work/serve
rm -rf "$work"
mkdir -p "$stage"
chmod 700 "$work"
cp "$programs/stxweb" "$static/index.html" "$static/app.js" "$static/style.css" "$stage/"
secret=$work/password.txt
python3 -I -c 'import secrets; print(secrets.token_urlsafe(18))' > "$secret"
chmod 600 "$secret"
python3 -I "$tools/make_password_turn.py" --password-file "$secret" --out "$stage/password.0"
app_sha=$(shasum -a 256 "$stage/app.js" | cut -d' ' -f1)
password_sha=$(shasum -a 256 "$stage/password.0" | cut -d' ' -f1)

cd "$vmtools"
python3 prepare_vm.py --image "$image" --checksums "$checksums" --name "$vm" --reset
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/loader-config.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/acpi-verbose.txt
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-config.txt --serve-dir "$configs" \
    --define "config_file=c2-web.conf" --define "config_sha256=$config_sha"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-kernel.txt --serve-dir "$kernels" \
    --define "kernel_sha256=$kernel_sha" --define "kernconf=$kernconf"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands "$here/sessions/install-web.txt" --serve-dir "$stage" \
    --define "web_sha256=$web_sha" --define "app_sha256=$app_sha" --define "password_sha256=$password_sha"
python3 guest_session.py --name "$vm" --firmware-timeout-ms 0 --timeout 180 \
    --commands sessions/install-programs.txt --serve-dir "$programs" \
    --define "init_sha256=$init_sha" --define "supervisor_file=stxsupervisor" \
    --define "supervisor_sha256=$supervisor_sha"

status=0

# Boot one. Each expect scans forward from the previous match; the checks
# cause the lines in this order (web_check.py, phase_first).
python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label web1 --ssh-forward-port "$port" \
    --step 'expect:60:STXSUPERVISOR init done ' \
    --step 'expect:60:STXWEB fingerprint made sha256=[0-9A-F:]{95}' \
    --step 'expect:10:STXWEB listening port=22' \
    --step 'expect:240:STXWEB event=login_failed client=\S+' \
    --step 'expect:60:STXWEB event=login_ok client=\S+' \
    --step 'expect:30:STXWEB event=operated client=\S+ detail=stop service=sleeper status=ok' \
    --step 'expect:30:STXWEB event=operated client=\S+ detail=start service=sleeper status=ok' \
    --step 'expect:30:STXWEB event=refused client=\S+ detail=csrf' \
    --step 'expect:30:STXWEB event=operated client=\S+ detail=stop service=web status=refused_self' \
    --step 'expect:60:STXWEB upload stored path=/user/stx/staging/program.pkg' \
    --step 'expect:60:STXWEB event=operated client=\S+ detail=reboot status=ok' \
    --step 'expect:30:STXSUPERVISOR reboot services=' \
    --step 'exit:60' > "$work/scenario1.txt" 2>&1 &
scenario_pid=$!
python3 -I "$here/web_check.py" --port "$port" --password-file "$secret" --phase first \
    | tee "$work/check1.txt" || status=1
wait "$scenario_pid" || status=1
cat "$work/scenario1.txt"
grep -q '^WEBCHECK first PASS$' "$work/check1.txt" || status=1

# The certificate the check saw is the one the console printed.
logs1=$(sed -n 's/^logs: //p' "$work/scenario1.txt" | tail -1)
console_fingerprint=$(sed -n 's/.*STXWEB fingerprint made sha256=\([0-9A-F:]*\).*/\1/p' "$logs1/serial.lines" 2>/dev/null | head -1)
seen_fingerprint=$(sed -n 's/^WEBCHECK fingerprint=//p' "$work/check1.txt" | head -1)
if [ -z "$console_fingerprint" ] || [ "$console_fingerprint" != "$seen_fingerprint" ]; then
    echo "c2_web: fingerprint console=$console_fingerprint seen=$seen_fingerprint"
    status=1
fi

# Boot two: the identity and the configuration read back.
python3 scenario.py --name "$vm" --firmware-timeout-ms 0 --label web2 --ssh-forward-port "$port" \
    --step 'expect:60:STXSUPERVISOR init done ' \
    --step "expect:60:STXWEB fingerprint sha256=$console_fingerprint" \
    --step 'expect:10:STXWEB listening port=22' \
    --step 'expect:240:STXWEB event=login_ok client=\S+' \
    --step 'expect:60:STXWEB event=logout client=\S+' \
    --step 'expect:60:STXWEB event=login_failed client=\S+' \
    --step 'monitor:system_powerdown' \
    --step 'expect:5:power button pressed' \
    --step 'expect:15:STXINIT shutdown power-off' \
    --step 'exit:20' > "$work/scenario2.txt" 2>&1 &
scenario_pid=$!
python3 -I "$here/web_check.py" --port "$port" --password-file "$secret" --phase second \
    | tee "$work/check2.txt" || status=1
wait "$scenario_pid" || status=1
cat "$work/scenario2.txt"
grep -q '^WEBCHECK second PASS$' "$work/check2.txt" || status=1

if [ "$status" -eq 0 ]; then
    echo "c2_web: PASS"
else
    echo "c2_web: FAIL"
fi
exit "$status"
