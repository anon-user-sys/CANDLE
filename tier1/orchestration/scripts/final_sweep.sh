#!/usr/bin/env bash
# Paper-ready sweep: 3 repetitions x 15 tests, XPTI off, unpinned,
# fresh process per test, with stall detection and retry.
#
# The stall: a test hangs producing NO output (seen on T6, T8, T10). It is a
# hang, not a slowdown -- T10 took 600s+ once and 18s on retry with no code
# change. Survives a pxl_resourced restart. Detect and retry; if a test stalls
# twice, flag it and move on rather than silently recording a bad number.
set -u
BIN="$(cd "$(dirname "$0")/.." && pwd)/main_profiler"
OUT=${1:?usage: final_sweep.sh <outdir>}
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-1200}
mkdir -p "$OUT"
SUMMARY="$OUT/summary.tsv"
echo -e "run\ttest\trc\tsecs\tlines\tstatus" > "$SUMMARY"

run_one() {   # run_one <rep> <test> <attempt>
  local rep=$1 t=$2 att=$3
  local f="$OUT/r${rep}_t${t}.txt"
  local t0 t1 rc lines
  t0=$(date +%s)
  BENCH_CPU=none timeout "$TIMEOUT" "$BIN" "t${t}" > "$f" 2>&1
  rc=$?
  t1=$(date +%s)
  lines=$(grep -c . "$f" 2>/dev/null || echo 0)
  local status="ok"
  # stall signature: timeout, or completed but produced no data rows
  if [ "$rc" -eq 124 ] || [ "$lines" -lt 5 ]; then status="STALL"; fi
  # also flag the ~300ms floor signature on a fixed-tc test
  if [ "$status" = ok ] && grep -qE '^[0-9]+,3[0-9]{5}\.' "$f" 2>/dev/null; then status="STALL_FLOOR"; fi
  echo -e "${rep}\tt${t}\t${rc}\t$((t1-t0))\t${lines}\t${status}(a${att})" >> "$SUMMARY"
  [ "$status" = ok ]
}

for rep in $(seq 1 "$REPS"); do
  for t in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
    if ! run_one "$rep" "$t" 1; then
      echo "### rep${rep} t${t}: stall detected, retrying once" >&2
      sleep 5
      run_one "$rep" "$t" 2 || echo "### rep${rep} t${t}: STALL PERSISTS -- needs pxl_resourced restart" >&2
    fi
  done
done

# One XPTI pass purely for the device-stage table (Table 2).
# Its wall-clock numbers are INVALID (XPTI inflates 1.3-2.4x and corrupts
# getStats) -- use it ONLY for launch_to_start / start_to_end / end_to_terminate.
for t in 3 9; do
  f="$OUT/xpti_t${t}.txt"
  t0=$(date +%s)
  XPTI=1 BENCH_CPU=none timeout "$TIMEOUT" "$BIN" "t${t}" > "$f" 2>&1
  rc=$?; t1=$(date +%s)
  echo -e "xpti\tt${t}\t${rc}\t$((t1-t0))\t$(grep -c . "$f")\tdevice-stages-only" >> "$SUMMARY"
done
echo "DONE -> $SUMMARY"
