#!/usr/bin/env bash
# drain_bench runs, in either mode, 3 fresh processes each at 1M and 10M items:
#   sudo ./scripts/run_enqueue_bench.sh               # enqueue mode (default): + host enqueue timing
#   sudo ./scripts/run_enqueue_bench.sh coherence     # original coherence benchmark only
#   sudo ./scripts/run_enqueue_bench.sh enqueue /path # explicit mode + output dir
# Neither mode measures DISPATCH cost: drain_bench is one dispatch, then a
# single-MU-thread memory read + cache-coherence flush per item.
#
# MUST run as root: host stores into memAlloc (CXL) memory SIGBUS otherwise,
# and a crash mid-dispatch can wedge the device. The guards below refuse to
# start rather than risk that.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: run with sudo (non-root host writes SIGBUS)"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' \
  || { echo "ABORT: dax0.0 not in devdax mode -> sudo daxctl reconfigure-device --mode=devdax --force dax0.0"; exit 1; }
pgrep -x main_profiler >/dev/null && { echo "ABORT: orchestration benchmark is using the device"; exit 1; }
[ -x ./main_spike ] || { echo "ABORT: ./main_spike missing; run ./build.sh"; exit 1; }

MODE=${1:-enqueue}
case "$MODE" in enqueue) FLAG=--enqueue ;; coherence) FLAG= ;;
  *) echo "ABORT: mode must be 'enqueue' or 'coherence'"; exit 1 ;; esac
OUT=${2:-$PWD/results/spike_${MODE}_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
for N in 1000000 10000000; do
  for r in 1 2 3; do                               # fresh process per run
    f="$OUT/${MODE}_N${N}_r${r}.txt"
    timeout 600 ./main_spike --bench "$N" $FLAG > "$f" 2>&1; rc=$?
    ok=$(grep -q 'OK' "$f" && echo checksum-OK || echo CHECKSUM-FAIL)
    echo "N=$N r=$r rc=$rc $ok"
    grep -E 'device pull cost|end-to-end \(wall\)|host enqueue|end-to-end/item' "$f" | sed 's/^/    /'
    [ "$rc" -eq 0 ] || { echo "STOP: run failed, not continuing (check: xcena_cli num-device)"; break 2; }
  done
done
chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
echo "results -> $OUT"
