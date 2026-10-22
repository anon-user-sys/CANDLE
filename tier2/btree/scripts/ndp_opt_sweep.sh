#!/bin/bash
# Optimized NDP sweep: compact layout + probes range-partitioned per task (P = tasks).
set -uo pipefail
SF=${1:-1500}; cd "$(dirname "$0")/.." || exit 1
CSV=results/ndp_opt_sf${SF}_$(date +%Y%m%d_%H%M%S).csv; ERR=${CSV%.csv}.err
echo "# ndp_opt sf=$SF layout=compact partition=tasks" | tee "$CSV"; ./main --header >> "$CSV"
for dist in uniform zipf; do for p in 256 512 1024 2048 3048; do
  echo "  $(date +%T) $dist $p" >&2
  numactl --cpunodebind=0 --membind=0 ./main --mode device --label M4_opt --sf "$SF" --layout compact \
     --partition "$p" --par "$p" --dist "$dist" --lookups 100M --iters 3 >> "$CSV" 2>>"$ERR" || echo "  FAILED $dist $p" >&2
done; done
echo "results: $CSV"
