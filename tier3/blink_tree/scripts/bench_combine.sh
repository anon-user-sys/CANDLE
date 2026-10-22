#!/bin/bash
# cxl_full: request combining on vs. off (10M keys, scramble). Result: results/bench_combine_<time>.csv
set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p results
OUT="results/bench_combine_$(date +%Y%m%d_%H%M%S).csv"
echo "combine,workload,batch,rep,mops,device_lookups,device_writes,batch_total_ms,launch_ms,readback_ms,dedup_ms" > "$OUT"
for wl in a b c f; do for b in 262144 1M; do for comb in 1 0; do for rep in 1 2 3; do
  flag=""; [ "$comb" = "0" ] && flag="--no-combine"
  log=$(timeout 1800 ./blink_cxl_full_tp -t 4 -o 8M --keys=10M --scramble --workload=$wl --batch=$b --dtasks=1024 $flag 2>&1 | tr '\r' '\n')
  g() { echo "$log" | grep "$1" | sed -n "s/.* $2=\([0-9.e+]*\).*/\1/p"; }
  echo "$comb,$wl,$b,$rep,$(echo "$log" | grep Throughput | awk '{print $(NF-1)}'),$(g '^\[CXL\]' device_lookups),$(g '^\[CXL\]' device_writes),$(g CXL-phases batch_total),$(g CXL-phases launch),$(g CXL-phases readback),$(g CXL-phases dedup)" | tee -a "$OUT"
done; done; done; done
echo "results: $OUT"
