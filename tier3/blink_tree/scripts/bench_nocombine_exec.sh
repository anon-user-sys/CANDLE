#!/bin/bash
# cxl_full --no-combine, timed from execute() to batch end (current rule); 10M keys, scramble, batch 1M.
# Result: results/nocombine_exec_<time>.csv
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="results/nocombine_exec_$(date +%Y%m%d_%H%M%S).csv"
echo "workload,dtasks,rep,mops_exec,mops_wall,launch_ms,stage_ms,readback_ms,readback_inval_ms,dedup_ms,replay_ms,write_prep_ms,split_ms,report_ms,device_lookups,device_writes" > "$OUT"
for wl in a b; do for d in 256 512 1024; do for rep in 1 2 3; do
  log=$(timeout 1800 ./blink_cxl_full_tp -t 4 -o 8M --keys=10M --scramble --workload=$wl --batch=1M --dtasks=$d --no-combine 2>&1 | tr '\r' '\n')
  ph=$(echo "$log" | grep CXL-phases); cx=$(echo "$log" | grep "^\[CXL\]")
  g() { echo "$1" | sed -n "s/.* $2=\([0-9.]*\).*/\1/p"; }
  echo "$wl,$d,$rep,$(echo "$log" | grep " Throughput" | awk '{print $(NF-1)}'),$(echo "$log" | grep "Wall throughput" | awk '{print $4}'),$(g "$ph" launch),$(g "$ph" stage),$(g "$ph" readback),$(g "$ph" readback_inval),$(g "$ph" dedup),$(g "$ph" replay),$(g "$ph" write_prep),$(g "$ph" split),$(g "$ph" report),$(g "$cx" device_lookups),$(g "$cx" device_writes)" | tee -a "$OUT"
done; done; done
echo "results: $OUT"
