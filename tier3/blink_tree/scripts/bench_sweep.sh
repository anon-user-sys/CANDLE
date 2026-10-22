#!/bin/bash
# Full sweep, host_cxl vs. cxl_full (no hybrid). Result: results/bench_sweep_<time>.csv
#   KEYS_LIST (default "100k 10M"), WORKLOADS (default "a b c d f"), REPS (default 3), OPS (default 8M), SCRAMBLE (default 1)
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEYS_LIST="${KEYS_LIST:-100k 10M}"
WORKLOADS="${WORKLOADS:-a b c d f}"
REPS="${REPS:-3}"
OPS="${OPS:-8M}"
SCRAMBLE="${SCRAMBLE:-1}"
scr_flag=""; [ "$SCRAMBLE" = "1" ] && scr_flag="--scramble"
mkdir -p results
OUT="results/bench_sweep_$(date +%Y%m%d_%H%M%S).csv"
echo "mode,keys,scramble,workload,ops,threads,batch,dtasks,rep,mops,launches,avg_launch_us,device_lookups,device_writes,host_split_inserts,batch_total_ms,launch_ms,readback_ms,readback_inval_ms,dedup_ms" > "$OUT"
CONFIGS=(
  "host_cxl|-t 16"
  "host_cxl|-t 64"
  "host_cxl|-t 128"
  "cxl_full|-t 4 --batch=65536 --dtasks=1024"
  "cxl_full|-t 4 --batch=262144 --dtasks=1024"
  "cxl_full|-t 4 --batch=1M --dtasks=1024"
  "cxl_full|-t 4 --batch=1M --dtasks=3048"
)
for keys in $KEYS_LIST; do
  for wl in $WORKLOADS; do
    for cfg in "${CONFIGS[@]}"; do
      mode="${cfg%%|*}"; args="${cfg#*|}"
      for rep in $(seq 1 "$REPS"); do
        log=$(timeout 1800 "./blink_${mode}_tp" -o "$OPS" --keys="$keys" --workload="$wl" $scr_flag $args 2>&1 | tr '\r' '\n') || true
        tp=$(echo "$log" | grep Throughput | awk '{print $(NF-1)}')
        cfgl=$(echo "$log" | grep "^\[Config\]")
        f() { echo "$cfgl" | sed -n "s/.* $1=\([0-9]*\).*/\1/p"; }
        cxl=$(echo "$log" | grep "^\[CXL\]" || true); ph=$(echo "$log" | grep "CXL-phases" || true)
        g() { echo "$1" | sed -n "s/.* $2=\([0-9.e+]*\).*/\1/p"; }
        echo "$mode,$keys,$SCRAMBLE,$wl,$OPS,$(f threads),$(f batch),$(f dtasks),$rep,${tp:-ERR},$(g "$cxl" launches),$(g "$cxl" avg_launch_us),$(g "$cxl" device_lookups),$(g "$cxl" device_writes),$(g "$cxl" host_split_inserts),$(g "$ph" batch_total),$(g "$ph" launch),$(g "$ph" readback),$(g "$ph" readback_inval),$(g "$ph" dedup)" | tee -a "$OUT"
      done
    done
  done
done
echo "results: $OUT"
