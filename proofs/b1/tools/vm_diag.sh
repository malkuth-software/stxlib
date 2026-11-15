#!/bin/bash
# Run inside vm_genmc.sh (the VM is up). Prints to stdout only; writes the
# shim to /Volumes/Storage/stxlib_workers/b1/diag/ and the guest's home.
set -u
D=/Volumes/Storage/stxlib_workers; V=$D/vm/ubuntu-genmc; P=2232
O="-i $V/id_ed25519 -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile=$V/known_hosts -o ConnectTimeout=5"
S="ssh $O -p $P genmc@127.0.0.1"
DIAG=$D/b1/diag; mkdir -p $DIAG
GP=$D/src/genmc-0.20.0/lli/runtime-include/c/pthread.h
echo "----- genmc pthread.h (declarations)"
grep -n -E 'typedef|define|^[a-z].*\(' "$GP" | head -120
echo "----- guest: pthread names gthr-posix.h uses"
$S 'grep -o -E "\b(pthread_[a-z_]+|sched_yield|PTHREAD_[A-Z_]+)\b" /usr/include/aarch64-linux-gnu/c++/13/bits/gthr-posix.h | sort -u | tr "\n" " "; echo'  > $DIAG/gthr-names.txt
cat $DIAG/gthr-names.txt
echo "----- guest: genmc include search"
$S 'cd /home/genmc/b1run && ls out/units | head -3'
python3 proofs/b1/tools/genmc_shim.py "$GP" $DIAG/gthr-names.txt > $DIAG/genmc_posix_compat.h
echo "----- generated shim"; cat $DIAG/genmc_posix_compat.h
scp -q $O -P $P $DIAG/genmc_posix_compat.h genmc@127.0.0.1:/home/genmc/b1run/genmc_posix_compat.h
for unit in 22-treatment-threads2-genmc-vm-bounded-rc11-N--original 23-treatment-batch1-genmc-vm-bounded-rc11-N--original; do
  cmd=$(python3 - "$DIAG/vm-smoke-1/smoke.jsonl" "$unit" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    r = json.loads(line)
    if r.get('log', '').endswith(sys.argv[2] + '.txt'):
        print(r['guest_command']); break
PY
)
  cmd=${cmd/ -- / -- -include \/home\/genmc\/b1run\/genmc_posix_compat.h }
  echo "----- $unit with the shim"
  echo "\$ $cmd"
  $S "cd /home/genmc/b1run && PATH=/home/genmc/genmc-0.20.0/bin:\$PATH timeout -s KILL 600 $cmd" > $DIAG/$unit-shim.txt 2>&1
  echo "exit $?"
  grep -v '^\s*$' $DIAG/$unit-shim.txt | grep -m 15 -E 'error|errors were|Safety|violation|race|complete|explored|unsupported|Error'
  echo '(last 15)'; tail -15 $DIAG/$unit-shim.txt
done
