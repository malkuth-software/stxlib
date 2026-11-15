#!/bin/bash
# Boot the Ubuntu GenMC VM (decision B1-6) headless, run one command on the
# M4 while it is up, and shut the VM down afterwards, whatever the command
# did. The boot line is the integrator's (integration/dev/tools/
# ubuntu_vm_genmc.sh) without its install steps. SSH on 127.0.0.1:2232 only.
#   bash proofs/b1/tools/vm_genmc.sh <command...>
set -u
D=/Volumes/Storage/stxlib_workers; V=$D/vm/ubuntu-genmc; P=2232
if lsof -nP -iTCP:$P -sTCP:LISTEN >/dev/null 2>&1; then echo "vm_genmc: port $P in use; stopping"; exit 1; fi
qemu-system-aarch64 -name stxlib-genmc -M virt -accel hvf -cpu host -smp 4 -m 6144 \
  -drive if=pflash,format=raw,readonly=on,file=$V/efi-code.fd -drive if=pflash,format=raw,file=$V/efi-vars.fd \
  -drive if=virtio,format=qcow2,file=$V/disk.qcow2 -drive if=virtio,format=raw,readonly=on,file=$V/seed.iso \
  -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$P-:22 -device virtio-net-pci,netdev=n0 \
  -display none -serial file:$V/console.log -monitor none -daemonize -pidfile $V/qemu.pid \
  || { echo "vm_genmc: QEMU FAILED"; exit 1; }
QP=$(cat $V/qemu.pid)
O="-i $V/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$V/known_hosts -o ConnectTimeout=5"
S="ssh -n $O -p $P genmc@127.0.0.1"
shutdown_vm() {
  $S 'sudo systemctl poweroff' >/dev/null 2>&1
  t0=$(date +%s)
  while kill -0 $QP 2>/dev/null; do
    [ $(( $(date +%s) - t0 )) -gt 120 ] && { echo "vm_genmc: killing qemu"; kill $QP; break; }
    sleep 2
  done
  echo "vm_genmc: VM down"
}
trap shutdown_vm EXIT
t0=$(date +%s)
until $S true 2>/dev/null; do
  [ $(( $(date +%s) - t0 )) -gt 300 ] && { echo "vm_genmc: SSH not up"; exit 1; }
  sleep 3
done
echo "vm_genmc: ssh up after $(( $(date +%s) - t0 )) s; guest: $($S 'uname -srm; /home/genmc/genmc-0.20.0/bin/genmc --version 2>&1 | head -1; clang-20 --version | head -1' | tr '\n' ' ')"
"$@"
status=$?
echo "vm_genmc: command exit $status"
exit $status
