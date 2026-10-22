#!/usr/bin/env bash
# Paired aged-vs-fresh sweep: FRESH=0 and FRESH=1 interleaved per test, so the
# comparison is not confounded by device state drifting between sessions.
set -u
BIN="$(cd "$(dirname "$0")/.." && pwd)/main_profiler"
OUT=${1:?usage: fresh_sweep.sh <outdir>}
REPS=${REPS:-3}; TIMEOUT=${TIMEOUT:-900}
mkdir -p "$OUT"; SUM="$OUT/summary.tsv"
echo -e "rep\ttest\tfresh\trc\tsecs\tlines\tstatus" > "$SUM"
run() {  # run <rep> <test> <fresh> <attempt>
  local f="$OUT/r$1_t$2_f$3.txt" t0 t1 rc lines st=ok
  t0=$(date +%s)
  FRESH=$3 BENCH_CPU=none timeout "$TIMEOUT" "$BIN" "t$2" > "$f" 2>&1; rc=$?
  t1=$(date +%s)
  lines=$(grep -c . "$f" 2>/dev/null); lines=${lines:-0}   # grep -c already prints 0; no '|| echo'
  { [ "$rc" -eq 124 ] || [ "$lines" -lt 5 ]; } && st=STALL
  echo -e "$1\tt$2\t$3\t$rc\t$((t1-t0))\t$lines\t$st(a$4)" >> "$SUM"
  [ "$st" = ok ]
}
for rep in $(seq 1 "$REPS"); do
  for t in 1 3 6 7 8 14; do
    for fr in 0 1; do
      run "$rep" "$t" "$fr" 1 || { echo "### r$rep t$t f$fr stall, retrying" >&2; sleep 5;
        run "$rep" "$t" "$fr" 2 || echo "### r$rep t$t f$fr STALL PERSISTS" >&2; }
    done
  done
done
echo "DONE -> $SUM"
