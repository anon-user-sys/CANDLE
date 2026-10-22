#!/usr/bin/env bash
# Burst publish bench: reproduce the in-kernel publish race (counter overtakes data)
# and test fixes.  sudo ./scripts/run_burst_bench.sh [outdir]
# Each chunk is L lines (L=512 -> 32 KB) flushed as one range; P=3048 lanes x C=8
# chunks = ~800 MB of output. 16 host readers chase the counters (tpch_scan_filter
# return_reader sequence). Gate: the counter control must show early_bad > 0, or
# the fixes are untested.
set -u
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "ABORT: run with sudo"; exit 1; }
daxctl list 2>/dev/null | grep -q '"mode":"devdax"' || { echo "ABORT: dax0.0 not in devdax mode"; exit 1; }
pgrep -x main_profiler >/dev/null && { echo "ABORT: orchestration benchmark is using the device"; exit 1; }
[ -x ./main_spike ] || { echo "ABORT: ./main_spike missing; run ./build.sh"; exit 1; }
OUT=${1:-$PWD/results/spike_burst_$(date +%Y%m%d_%H%M%S)}
mkdir -p "$OUT"
run() {
  local name=$1; shift
  local f="$OUT/$name.txt"
  timeout 180 ./main_spike "$@" > "$f" 2>&1; local rc=$?
  echo "[$name] rc=$rc  $(grep -E '^BURST ' "$f")"
  grep -E "first bad|final bad|final_bad at|missing lane" "$f" | sed "s/^/    /"
  if [ "$rc" -ne 0 ]; then
    echo "STOP: $name failed (rc=$rc). Check device: sudo xcena_cli num-device"; tail -5 "$f"
    chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null; exit 1
  fi
}
run sanity_counter --burst 64 --chunks 2 --lines 64 --bmode counter
for r in 1 2 3; do run "counter_r$r" --burst 3048 --chunks 8 --lines 512 --bmode counter; done
for m in atomic flushall linetag checksum; do
  for r in 1 2 3; do run "${m}_r$r" --burst 3048 --chunks 8 --lines 512 --bmode $m; done
done
chown -R "${SUDO_USER:-root}" "$OUT" 2>/dev/null
echo "results -> $OUT"
