#!/usr/bin/env bash
# Coherence-tax suite under the paper configuration of Sec. 4.3:
#   mx_dma kthreads pinned to CPUs 8,9; process bound to node 0; default C-states.
# A bit-exact device canary (data_movement_tax: main d1 --verify-only) runs before the suite and
# after every test; on a canary failure or a crash, device state is captured and the suite stops.
#
#   sudo QUICK=1 ./scripts/run_suite.sh            # smoke run, small sizes (~5 min)
#   sudo ./scripts/run_suite.sh                    # full suite
#   sudo TESTS="k6 k7 k8" ./scripts/run_suite.sh   # a subset
set -u
cd "$(dirname "$0")/.."
HERE=$PWD; CANARY="$HERE/../data_movement/main"
[ "$(id -u)" -eq 0 ] || { echo "ABORT: needs root"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' || { echo "ABORT: dax0.0 not in devdax mode"; exit 1; }
[ -x ./coh ] && [ -f mu_kernel/coh_kernel.mubin ] || { echo "ABORT: run ./build.sh"; exit 1; }
[ -x "$CANARY" ] || { echo "ABORT: canary binary $CANARY missing"; exit 1; }
for p in main main_profiler coh main_spike; do pgrep -x $p >/dev/null && { echo "ABORT: $p is using the device"; exit 1; }; done

TESTS=${TESTS:-"k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11"}
Q=""; [ "${QUICK:-0}" = 1 ] && Q="--quick"
OUT=${OUT:-$HERE/results/suite_$( [ -n "$Q" ] && echo quick_ )$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
SUB=$(pgrep -x mx_submit_thd0); COMP=$(pgrep -x mx_complete_thd0)
[ -n "$SUB" ] && [ -n "$COMP" ] || { echo "ABORT: mx kthreads not found"; exit 1; }
trap 'taskset -pc 0-255 $SUB >/dev/null 2>&1; taskset -pc 0-255 $COMP >/dev/null 2>&1; chown -R ${SUDO_USER:-root} $OUT' EXIT
taskset -pc 8 "$SUB" >/dev/null; taskset -pc 9 "$COMP" >/dev/null
echo "kthreads=8,9 node0 nocstate=0 quick=${QUICK:-0} tests=$TESTS" > "$OUT/CONFIG.txt"
echo "out: $OUT"

capture() {
  { echo "=== $(date -Is) $1"
    xcena_cli num-device 2>&1; xcena_cli device-info 0 2>&1
    fuser -v /dev/dax* 2>&1; who; ps -eo user,pid,etime,cmd | grep -iE 'pxl|xcena|qemu|main|coh' | grep -v grep
    dmesg -T 2>&1 | grep -iE 'mx_|cxl|dax|poison|uncorrect' | tail -30
  } > "$OUT/ATTRIBUTION_$1.txt" 2>&1
}
canary() {
  local f="$OUT/canary_$1.txt"
  numactl --cpunodebind=0 --membind=0 "$CANARY" d1 --verify-only > "$f" 2>&1
  local ok=$(grep -c 'VERIFY OK' "$f") bad=$(grep -c 'VERIFY FAIL' "$f")
  if [ "$bad" -gt 0 ] || [ "$ok" -lt 12 ]; then
    capture "canary_$1"; echo "STOP: canary $1 failed (ok=$ok fail=$bad) -> $OUT/ATTRIBUTION_canary_$1.txt"; exit 2
  fi
  echo "  canary $1 ok"
}

canary start
for t in $TESTS; do
  f="$OUT/$t.csv"; t0=$(date +%s)
  # SIGINT first: the PXL signal handler shuts the context down cleanly (aborts wedge the device)
  timeout -s INT -k 60 3600 numactl --cpunodebind=0 --membind=0 ./coh "$t" $Q > "$f" 2>"$OUT/$t.err"; rc=$?
  rows=$(grep -cE "^${t}[a-z]?," "$f")
  printf "%-4s rc=%-3s %5ss rows=%s\n" "$t" "$rc" "$(( $(date +%s) - t0 ))" "$rows"
  grep -h '^# k' "$f" | grep -viE 'header' | head -5 | sed 's/^/     /'
  if [ "$rc" -ne 0 ]; then capture "$t"; echo "STOP: $t rc=$rc (see $OUT/$t.err)"; tail -5 "$OUT/$t.err"; canary "after_fail_$t"; exit 1; fi
  canary "after_$t"
done
echo "done -> $OUT"
