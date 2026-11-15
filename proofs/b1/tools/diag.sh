#!/bin/bash
# B1 diagnostics (seat sb3), run on the M4 through m4sbrun from sb3's
# stxlib/ checkout. Prints everything to stdout; writes only under
# B1/diag/. Usage: bash proofs/b1/tools/diag.sh <step>
set -u
B1=/Volumes/Storage/stxlib_workers/b1
DIAG=$B1/diag
SMOKE=$B1/results/b1-smoke-4
TOOLS=/Volumes/Storage/stxlib_workers/tools
export PATH=$TOOLS/esbmc-8.6/release/bin:$TOOLS/genmc-0.20.0/bin:$PATH
mkdir -p "$DIAG"
SDK=$(xcrun --show-sdk-path)
RES21=$(/opt/homebrew/opt/llvm@21/bin/clang -print-resource-dir)/include

section() { printf '\n===== %s\n' "$1"; }
command_of() { # smoke.jsonl command of the first run whose log name starts with $1
  python3 - "$SMOKE/smoke.jsonl" "$1" <<'PY'
import json, sys
for line in open(sys.argv[1]):
    record = json.loads(line)
    if record.get('log', '').rsplit('/', 1)[-1].startswith(sys.argv[2]) and record.get('command'):
        print(record['command']); break
PY
}
attempt() { # $1 label, rest: command (a string evaluated by bash)
  local label=$1; shift
  echo "--- $label"
  echo "\$ $*"
  eval "$*" > "$DIAG/$label.txt" 2>&1
  echo "exit $?"
  grep -v '^\s*$' "$DIAG/$label.txt" | grep -m 12 -E 'error|VERIFICATION|Error|unsupported|fatal' || true
  echo '(last 12 lines)'
  tail -12 "$DIAG/$label.txt"
}

case "$1" in
esbmc-01)
  section 'esbmc install'
  esbmc_bin=$(command -v esbmc); echo "$esbmc_bin"
  otool -L "$esbmc_bin" | grep -i -E 'llvm|clang' | head -5
  find "$(dirname "$esbmc_bin")/.." -maxdepth 5 -name stdarg.h 2>/dev/null | head -5
  echo "llvm@21 resource include: $RES21"
  section 'esbmc help (include options)'
  esbmc --help 2>&1 | grep -i -E 'include|idir|sysroot|resource|isystem|nostd|--std |--os|--arch|target' | head -30
  base=$(command_of 01-treatment-default-esbmc-bounded-z3)
  plain=$(printf '%s' "$base" | sed "s| --idirafter [^ ]*||")
  attempt esbmc-01-idirafter "$base"
  attempt esbmc-01-I "$plain -I $RES21"
  attempt esbmc-01-I-sysroot "$plain -I $RES21 --sysroot $SDK"
  attempt esbmc-01-none "$plain"
  ;;
genmc-22)
  section 'genmc runtime include'
  root=/Volumes/Storage/stxlib_workers/src/genmc-0.20.0/lli/runtime-include
  find "$root" -maxdepth 2 | head -40
  grep -n -E 'memory_order|#\s*(ifndef|define) _' "$root/c/atomic" | head -20
  base=$(command_of 22-treatment-threads2-genmc)
  attempt genmc-22-v "$(printf '%s' "$base" | sed 's/ -- / -- -v /')"
  grep -n -A20 'search starts here' "$DIAG/genmc-22-v.txt" | head -24
  grep -n -m 8 'error' "$DIAG/genmc-22-v.txt"
  attempt genmc-22-cxx17 "$(printf '%s' "$base" | sed 's/-std=c++20/-std=c++17/')"
  ;;
census-23)
  python3 proofs/b1/tools/census.py --sources $B1/sources/sources.json --out $DIAG/census-23 \
      --rows 23 --clang /opt/homebrew/opt/llvm@22/bin/clang++
  python3 -c "import json; r=json.load(open('$DIAG/census-23/census/23.json')); print('clang_exit', r.get('clang_exit'), 'error', r.get('error'))"
  grep -m 6 'error' "$DIAG/census-23/census/23.clang.txt" || echo 'no error lines'
  base=$(head -1 "$DIAG/census-23/census/23.clang.txt" | sed 's/-Xclang -ast-dump=json//')
  for extra in "-DSTX_MODEL_SIZED_DELETE=0" "-fno-sized-deallocation" "-fsized-deallocation"; do
    echo "--- variant $extra"
    eval "$base $extra" > "$DIAG/census-23-variant.txt" 2>&1
    echo "exit $?"; grep -c 'error:' "$DIAG/census-23-variant.txt"; grep -m 3 -A3 'error:' "$DIAG/census-23-variant.txt"
  done
  ;;
genmc-sanity)
  cat > "$DIAG/sanity.c" <<'C'
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
atomic_int x;
void* writer(void* unused) { (void)unused; atomic_store_explicit(&x, 1, memory_order_release); return 0; }
int main(void) {
    pthread_t thread;
    pthread_create(&thread, 0, writer, 0);
    int seen = atomic_load_explicit(&x, memory_order_acquire);
    pthread_join(thread, 0);
    assert(seen == 0 || seen == 1);
#ifdef REFUTE
    assert(seen == 0);
#endif
    return 0;
}
C
  attempt genmc-sanity-holds "genmc --rc11 -- -isysroot $SDK $DIAG/sanity.c"
  attempt genmc-sanity-refute "genmc --rc11 -- -isysroot $SDK -DREFUTE $DIAG/sanity.c"
  ;;
esbmc-smoke)
  python3 proofs/b1/tools/run_b1.py --sources $B1/sources/sources.json --out $DIAG/smoke-esbmc-$2 \
      --smoke --tools esbmc --rows "$2" --timeout 300
  ;;
*) echo "unknown step $1"; exit 2;;
esac
echo "diag $1 done"
