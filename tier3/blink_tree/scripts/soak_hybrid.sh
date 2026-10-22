#!/bin/bash
# Hybrid soak: does hybrid lose device updates on the current code, and only hybrid? Each round = one hybrid
# validation (4 configs in rotation, fresh seed) + one cxl_full canary validation. On any failure: device
# status + other PXL processes. L2 write-back OFF. Result: results/soak_hybrid_<time>.csv (+ .log)
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROUNDS="${ROUNDS:-24}"
HYBRID_ENV="${HYBRID_ENV:-}"          # extra env for the hybrid runs, e.g. BLINK_L2FLUSH=1
OUT="results/soak_hybrid${HYBRID_ENV:+_l2flush}_$(date +%Y%m%d_%H%M%S)"; echo "round,time,kind,config,seed,status,rt_mismatch,final_diff,secs" > "$OUT.csv"
CONFIGS=(
  "lines_par16_1M_b4096|BLINK_LOCATE_PARALLEL=1 BLINK_LOCATE_INVAL=lines|-t 16 -o 1M --keys=1M --workload=a --batch=4096 --dtasks=64"
  "lines_1thr_1M_b4096|BLINK_LOCATE_INVAL=lines|-t 4 -o 1M --keys=1M --workload=a --batch=4096 --dtasks=64"
  "node_par16_1M_b4096|BLINK_LOCATE_PARALLEL=1|-t 16 -o 1M --keys=1M --workload=a --batch=4096 --dtasks=64"
  "node_par64_10M_b262k|BLINK_LOCATE_PARALLEL=1|-t 64 -o 2M --keys=10M --scramble --workload=a --batch=262144 --dtasks=1024"
)
one() { # round kind config seed envs args
  local r=$1 kind=$2 cfg=$3 seed=$4 envs=$5 args=$6 st=$(date +%s)
  local out; out=$(env $envs timeout 1500 ./blink_${kind}_val $args -s $seed 2>&1 | tr '\r' '\n')
  local ok=FAIL; echo "$out" | grep -q PASSED && ok=PASS
  local rt; rt=$(echo "$out" | grep Mismatches | awk '{print $NF}'); local fd; fd=$(echo "$out" | grep Diff | awk '{print $NF}')
  echo "$r,$(date +%H:%M:%S),$kind,$cfg,$seed,$ok,${rt:-?},${fd:-?},$(( $(date +%s)-st ))" | tee -a "$OUT.csv"
  if [ "$ok" != PASS ]; then
    { echo "=== FAIL round $r $kind $cfg seed $seed at $(date)"; xcena_cli num-device 2>&1 | tail -2; xcena_cli device-info 0 2>&1 | head -20
      echo "--- other PXL/device processes:"; ps -eo user,pid,etime,args | grep -iE "pxl|mubin|blink_|hashjoin|xcena" | grep -v -E "grep|pxl_resourced|soak_hybrid"
      echo "--- logged in:"; who; } >> "$OUT.log" 2>&1
  fi
}
for r in $(seq 1 "$ROUNDS"); do
  c="${CONFIGS[$(( (r-1) % ${#CONFIGS[@]} ))]}"; IFS='|' read -r name envs args <<< "$c"
  one "$r" cxl_hybrid "$name" $((1000 + r)) "$envs $HYBRID_ENV" "$args"
  one "$r" cxl_full canary_1M_b4096 $((2000 + r)) "X=1" "-t 4 -o 1M --keys=1M --workload=a --batch=4096 --dtasks=64"
done
echo "results: $OUT.csv"
